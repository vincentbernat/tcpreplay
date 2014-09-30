/*
 *   Copyright (c) 2013-2014 Fred Klassen <tcpreplay at appneta dot com> - AppNeta
 *   Copyright (c) 2014 Alexey Indeev <aindeev at appneta dot com> - AppNeta
 *
 *   The Tcpreplay Suite of tools is free software: you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation, either version 3 of the
 *   License, or with the authors permission any later version.
 *
 *   The Tcpreplay Suite is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with the Tcpreplay Suite.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/quick_tx.h>
#include <net/sch_generic.h>

static inline int quick_tx_clear_skb_list(struct quick_tx_skb *list) {
	int num_freed = 0;
	struct quick_tx_skb *qtx_skb, *tmp;
	list_for_each_entry_safe(qtx_skb, tmp, &list->list, list) {
		num_freed++;
		list_del_init(&qtx_skb->list);
		kmem_cache_free(qtx_skbuff_head_cache, qtx_skb);
	}
	return num_freed;
}

static inline int quick_tx_free_skb(struct quick_tx_dev *dev, bool free_skb)
{
	struct quick_tx_skb *qtx_skb;
	int freed = 0;

	if (!list_empty(&dev->skb_wait_list.list)) {
		qtx_skb = list_first_entry(&dev->skb_wait_list.list, struct quick_tx_skb, list);
		while (qtx_skb != &dev->skb_wait_list) {
			if (atomic_read(&qtx_skb->skb.users) == 1) {
				u32 *dma_block_index = (u32*)(qtx_skb->skb.cb + (sizeof(qtx_skb->skb.cb) - sizeof(u32)));
				atomic_dec(&dev->shared_data->dma_blocks[*dma_block_index].users);
				wmb();

				list_del_init(&qtx_skb->list);

				if (unlikely(free_skb)) {
					freed++;
					kmem_cache_free(qtx_skbuff_head_cache, qtx_skb);
				} else {
					list_add(&qtx_skb->list, &dev->skb_freed_list.list);
				}

				qtx_skb = list_first_entry(&dev->skb_wait_list.list, struct quick_tx_skb, list);
			} else {
				break;
			}
		}
	}

	if (free_skb) {
		freed += quick_tx_clear_skb_list(&dev->skb_freed_list);
	}

	dev->num_skb_freed += freed;

	return freed;
}

static inline int quick_tx_dev_queue_xmit(struct sk_buff *skb, struct net_device *dev, struct netdev_queue *txq)
{
	int status = -ENETDOWN;

	/* Disable soft irqs for various locks below. Also
	 * stops preemption for RCU.
	 */
	rcu_read_lock_bh();

	if (likely(dev->flags & IFF_UP)) {
		HARD_TX_LOCK(dev, txq, smp_processor_id());

		if (!netif_xmit_stopped(txq)) {
			status = dev->netdev_ops->ndo_start_xmit(skb, dev);
		}
		HARD_TX_UNLOCK(dev, txq);
	}

	rcu_read_unlock_bh();
	return status;
}


static inline int quick_tx_send_one_skb(struct quick_tx_skb *qtx_skb,
		struct netdev_queue *txq, struct quick_tx_dev *dev, int *done, int budget, bool all)
{
	netdev_tx_t status = NETDEV_TX_BUSY;
	struct net_device *netdev = qtx_skb->skb.dev;

	atomic_set(&qtx_skb->skb.users, 2);

retry_send:
	status = quick_tx_dev_queue_xmit(&qtx_skb->skb, netdev, txq);
	(*done)++;

	switch(status) {
	case NETDEV_TX_OK:
		dev->num_tx_ok_packets++;
		dev->num_tx_ok_bytes += qtx_skb->skb.len;
		return status;
	case NETDEV_TX_BUSY:
		dev->num_tx_busy++;
		break;
	case NETDEV_TX_LOCKED:
		dev->num_tx_locked++;
		break;
	default:
		dev->num_tq_frozen_or_stopped++;
	}

	if (*done < budget || all) {
		cpu_relax();
		goto retry_send;
	}

	return status;
}

static inline int quick_tx_do_transmit(struct quick_tx_skb *qtx_skb, struct netdev_queue *txq, struct quick_tx_dev *dev, int budget, bool all)
{
	netdev_tx_t status = NETDEV_TX_BUSY;
	struct quick_tx_skb *next_qtx_skb = NULL;
	int done = 0;
	int done_inc = 0;

	if (list_empty(&dev->skb_queued_list.list))
		next_qtx_skb = qtx_skb;
	else if (qtx_skb)
		list_add_tail(&qtx_skb->list, &dev->skb_queued_list.list);

send_next:

	if (!list_empty(&dev->skb_queued_list.list))
		next_qtx_skb = list_first_entry(&dev->skb_queued_list.list, struct quick_tx_skb, list);

	if (!next_qtx_skb)
		goto out;

	do {
		status = quick_tx_send_one_skb(next_qtx_skb, txq, dev, &done_inc, 128, all);

		if (likely(status == NETDEV_TX_OK)) {
			list_del_init(&next_qtx_skb->list);
			list_add_tail(&next_qtx_skb->list, &dev->skb_wait_list.list);
			next_qtx_skb = NULL;

			goto send_next;
		}

		done += done_inc;
	} while (done < budget || all);

	if (list_empty(&dev->skb_queued_list.list))
		list_add_tail(&qtx_skb->list, &dev->skb_queued_list.list);

out:
	RUN_AT_INVERVAL(quick_tx_free_skb(dev, false), 100, quick_tx_free_skb_dummy);
	return status;
}

static inline struct quick_tx_skb* quick_tx_alloc_skb_fill(struct quick_tx_dev * dev, unsigned int data_size, gfp_t gfp_mask,
			    int flags, int node, u8 *data, unsigned int full_size)
{
	struct skb_shared_info *shinfo;
	struct quick_tx_skb *qtx_skb;
	struct sk_buff *skb;

	if (unlikely(list_empty(&dev->skb_freed_list.list))) {
		dev->num_skb_alloced++;
		qtx_skb = kmem_cache_alloc_node(qtx_skbuff_head_cache, gfp_mask & ~__GFP_DMA, node);
		INIT_LIST_HEAD(&qtx_skb->list);
	} else {
		qtx_skb = list_first_entry(&dev->skb_freed_list.list, struct quick_tx_skb, list);
		list_del_init(&qtx_skb->list);
	}

	if (!qtx_skb)
		return NULL;

	skb = &qtx_skb->skb;

	prefetchw(skb);
	prefetchw(data + full_size);

	memset(skb, 0, offsetof(struct sk_buff, tail));

	skb->truesize = SKB_TRUESIZE(SKB_DATA_ALIGN(data_size));
	atomic_set(&skb->users, 1);
	skb->head = data;
	skb->data = data;
	skb_reset_tail_pointer(skb);
	skb->end = skb->tail + data_size;
#ifdef NET_SKBUFF_DATA_USES_OFFSET
	skb->mac_header = ~0U;
	skb->transport_header = ~0U;
#endif

	skb_reserve(skb, NET_SKB_PAD);
	skb_put(skb, data_size - NET_SKB_PAD);

	/* make sure we initialize shinfo sequentially */
	shinfo = skb_shinfo(skb);
	memset(shinfo, 0, offsetof(struct skb_shared_info, dataref));
	atomic_set(&shinfo->dataref, 1);
	kmemcheck_annotate_variable(shinfo->destructor_arg);

	skb_reset_mac_header(skb);

	return qtx_skb;
}

static void inline quick_tx_finish_work(struct quick_tx_dev *dev, struct netdev_queue *txq)
{
	/* flush all remaining SKB's in the list before exiting */
	quick_tx_do_transmit(NULL, txq, dev, 0, true);
	dev->time_end_tx = ktime_get_real();

	qtx_error("All packets have been transmitted successfully, exiting.");

	/* wait until cleaning the SKB list is finished
	 * as well before exiting so we do not have any memory leaks */
	while(!list_empty(&dev->skb_wait_list.list)) {
		int num_freed = quick_tx_free_skb(dev, true);
		schedule_timeout_interruptible(HZ);
	}

	qtx_error("Done freeing free_skb_list");

	quick_tx_calc_mbps(dev);
	quick_tx_print_stats(dev);

}

void quick_tx_worker(struct work_struct *work)
{
	struct quick_tx_dev *dev = container_of(work, struct quick_tx_dev, tx_work);
	struct quick_tx_skb *qtx_skb;
	struct sk_buff *skb;
	struct quick_tx_shared_data *data = dev->shared_data;
	struct quick_tx_packet_entry* entry = data->lookup_table + data->lookup_consumer_index;
	struct quick_tx_dma_block_entry* dma_block;
	struct netdev_queue *txq;
	u32 full_size = 0;
	int ret;

	qtx_error("Starting quick_tx_worker");

	if (!netif_device_present(dev->netdev) || !netif_running(dev->netdev)) {
		qtx_error("Device cannot currently transmit, it is not running.");
		qtx_error("Force stopping transmit..");
		data->error_flags |= QUICK_TX_ERR_NOT_RUNNING;
		return;
	}

	txq = netdev_get_tx_queue(dev->netdev, 0);

	dev->shared_data->lookup_flag = 0;
	wait_event(dev->consumer_q, dev->shared_data->lookup_flag == 1);
	dev->time_start_tx = ktime_get_real();

	while (true) {

		rmb();
		if (entry->length > 0 && entry->consumed == 0) {
			/* Calculate full size of the space required to packet */
			full_size = SKB_DATA_ALIGN(SKB_DATA_ALIGN(NET_SKB_PAD + entry->length) + sizeof(struct skb_shared_info));

			/* Get the DMA block our packet is in */
			dma_block = &data->dma_blocks[entry->dma_block_index];
			atomic_inc(&dma_block->users);

			/* Write memory barrier so that users++ gets executed beforehand */
			wmb();

			/* Fill up skb with data at the DMA block address + offset */
			qtx_skb = quick_tx_alloc_skb_fill(dev, NET_SKB_PAD + entry->length, GFP_NOWAIT,
					0, NUMA_NO_NODE, dma_block->kernel_addr + entry->block_offset, full_size);
			if (unlikely(!qtx_skb)) {
				atomic_dec(&dma_block->users);
				qtx_error("ALLOC_ERROR: Decrement on %d. Users at = %d",
						entry->dma_block_index, atomic_read(&dma_block->users));
				continue;
			}

			skb = &qtx_skb->skb;

			/* Copy over the bits of the DMA block index */
			*(u32*)(skb->cb + (sizeof(skb->cb) - sizeof(u32))) = entry->dma_block_index;

			/* Set netdev */
			skb->dev = dev->netdev;

			quick_tx_do_transmit(qtx_skb, txq, dev, 512, false);

#ifdef QUICK_TX_DEBUG
			qtx_error("Consumed entry at index = %d, dma_block_index = %d, offset = %d, len = %d",
					data->lookup_consumer_index, entry->dma_block_index, entry->block_offset, entry->length);
#endif

			/* Set this entry as consumed, increment to next entry */
			entry->consumed = 1;
			wmb();

			data->lookup_consumer_index = (data->lookup_consumer_index + 1) % LOOKUP_TABLE_SIZE;
			entry = data->lookup_table + data->lookup_consumer_index;
		} else {
			if (unlikely(dev->quit_work)) {
				quick_tx_finish_work(dev, txq);
				break;
			}
#ifdef QUICK_TX_DEBUG
			qtx_error("No packets to process, sleeping (index = %d), entry->consumed = %d", data->lookup_consumer_index,
					entry->consumed);
#endif

			dev->numsleeps++;
			dev->shared_data->lookup_flag = 0;
			wmb();

			/* Free some DMA blocks before going to sleep */
			if(!list_empty(&dev->skb_queued_list.list))
				quick_tx_do_transmit(NULL, txq, dev, 1, false);
			quick_tx_free_skb(dev, false);

			wait_event(dev->consumer_q, dev->shared_data->lookup_flag == 1);
		}
	}

	return;
}
