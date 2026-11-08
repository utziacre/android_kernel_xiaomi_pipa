/*
 * Copyright (c) 2019. Xiaomi Technology Co.
 *
 * All Rights Reserved.
 *
 * Confidential and Proprietary - Xiaomi Technology Co.
 */

#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include "ion_camera_heap.h"
#include "ion_secure_util.h"

static unsigned int camera_order_to_index(unsigned int order)
{
	unsigned int i;

	for (i = 0; i < NUM_ORDERS; i++) {
		if (orders[i] == order)
			return i;
	}
	BUG();
	return 0;
}

static struct page *ion_camera_alloc_page(struct ion_camera_heap *heap,
					  struct ion_buffer *buffer,
					  unsigned int index)
{
	bool cached = ion_buffer_cached(buffer);
	bool from_pool = !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC);
	struct ion_page_pool *pool = cached ? heap->cached_pools[index] :
					     heap->uncached_pools[index];
	struct page *page = ERR_PTR(-ENOMEM);

	if (from_pool)
		page = ion_page_pool_alloc_pool_only(heap->reserved_pools[index]);
	if (IS_ERR(page))
		page = ion_page_pool_alloc(pool, &from_pool);

	if (IS_ENABLED(CONFIG_ION_POOL_AUTO_REFILL) && pool->order &&
	    pool_count_below_lowmark(pool))
		ion_page_pool_worker_wake(&heap->kworker[cached]);

	if (!IS_ERR(page) && (MAKE_ION_ALLOC_DMA_READY || !from_pool))
		ion_pages_sync_for_device(heap->heap.priv, page,
					  PAGE_SIZE << orders[index],
					  DMA_BIDIRECTIONAL);
	return page;
}

static void ion_camera_free_page(struct ion_camera_heap *heap,
				 struct ion_buffer *buffer, struct page *page,
				 unsigned int order)
{
	unsigned int index = camera_order_to_index(order);
	struct ion_page_pool *pool = heap->reserved_pools[index];

	mod_node_page_state(page_pgdat(page), NR_UNRECLAIMABLE_PAGES,
			    -(1 << order));
	if ((buffer->flags & ION_FLAG_POOL_FORCE_ALLOC) ||
	    (buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE)) {
		ion_page_pool_free_immediate(pool, page);
		return;
	}

	if (atomic_read(&pool->count) >= cam_reserved_counts[index])
		pool = ion_buffer_cached(buffer) ? heap->cached_pools[index] :
						  heap->uncached_pools[index];
	ion_page_pool_free(pool, page);
}

static int ion_camera_heap_allocate(struct ion_heap *heap,
				    struct ion_buffer *buffer,
				    unsigned long size,
				    unsigned long flags)
{
	struct ion_camera_heap *camera_heap =
		container_of(heap, struct ion_camera_heap, heap);
	struct ion_page_info *info, *next;
	struct ion_page_info_pool info_pool;
	struct sg_table *table;
	struct scatterlist *sg;
	unsigned int i, max_order = orders[0], nents = 0;
	unsigned long remaining = PAGE_ALIGN(size);
	int ret = -ENOMEM;
	LIST_HEAD(pages);

	if (!remaining || size / PAGE_SIZE > totalram_pages / 2)
		return -ENOMEM;
	if (is_secure_vmid_valid(get_secure_vmid(flags)))
		return -EINVAL;

	ion_page_info_pool_init(&info_pool);
	while (remaining) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto free_pages;
		}
		info = ion_page_info_alloc(&info_pool);
		if (!info)
			goto free_pages;
		for (i = 0; i < NUM_ORDERS; i++) {
			if (orders[i] > max_order ||
			    (PAGE_SIZE << orders[i]) > remaining)
				continue;
			info->page = ion_camera_alloc_page(camera_heap, buffer, i);
			if (!IS_ERR(info->page))
				break;
			if (PTR_ERR(info->page) == -EINTR) {
				ret = -EINTR;
				break;
			}
		}
		if (i == NUM_ORDERS || ret == -EINTR)
			goto free_pages;
		info->order = orders[i];
		list_add_tail(&info->list, &pages);
		mod_node_page_state(page_pgdat(info->page), NR_UNRECLAIMABLE_PAGES,
				    1 << info->order);
		remaining -= PAGE_SIZE << info->order;
		max_order = info->order;
		nents++;
	}

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		goto free_pages;
	ret = sg_alloc_table(table, nents, GFP_KERNEL);
	if (ret) {
		kfree(table);
		goto free_pages;
	}

	sg = table->sgl;
	list_for_each_entry_safe(info, next, &pages, list) {
		sg_set_page(sg, info->page, PAGE_SIZE << info->order, 0);
		sg_dma_address(sg) = page_to_phys(info->page);
		sg = sg_next(sg);
		list_del(&info->list);
	}
	buffer->sg_table = table;
	ion_page_info_pool_destroy(&info_pool);
	return 0;

free_pages:
	list_for_each_entry_safe(info, next, &pages, list) {
		ion_camera_free_page(camera_heap, buffer, info->page, info->order);
		list_del(&info->list);
	}
	ion_page_info_pool_destroy(&info_pool);
	return ret;
}

static void ion_camera_heap_free(struct ion_buffer *buffer)
{
	struct ion_camera_heap *heap =
		container_of(buffer->heap, struct ion_camera_heap, heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	unsigned int i;

	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE) &&
	    !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC) &&
	    ion_heap_buffer_zero(buffer))
		buffer->private_flags |= ION_PRIV_FLAG_SHRINKER_FREE;

	for_each_sg(table->sgl, sg, table->orig_nents, i)
		ion_camera_free_page(heap, buffer, sg_page(sg), get_order(sg->length));
	sg_free_table(table);
	kfree(table);
}

static int ion_camera_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
				 int nr_to_scan)
{
	struct ion_camera_heap *camera_heap =
		container_of(heap, struct ion_camera_heap, heap);
	int i, freed = 0;

	/* Reserved camera pages stay outside the shrinker's reclaim budget. */
	for (i = NUM_ORDERS - 1; i >= 0; i--) {
		freed += ion_page_pool_shrink(camera_heap->uncached_pools[i],
					   gfp_mask,
					   nr_to_scan ? nr_to_scan - freed : 0);
		if (nr_to_scan && freed >= nr_to_scan)
			break;
		freed += ion_page_pool_shrink(camera_heap->cached_pools[i],
					   gfp_mask,
					   nr_to_scan ? nr_to_scan - freed : 0);
		if (nr_to_scan && freed >= nr_to_scan)
			break;
	}
	return freed;
}

static struct ion_heap_ops camera_heap_ops = {
	.allocate = ion_camera_heap_allocate,
	.free = ion_camera_heap_free,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
	.shrink = ion_camera_heap_shrink,
};

struct ion_heap *ion_camera_heap_create(struct ion_platform_heap *data)
{
	struct ion_camera_heap *heap;
	int ret;
	unsigned int i;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->heap.ops = &camera_heap_ops;
	heap->heap.type = (enum ion_heap_type)ION_HEAP_TYPE_CAMERA;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;
	heap->heap.priv = data->priv;

	ret = ion_page_pools_create(heap->uncached_pools, orders, NUM_ORDERS,
				    false, data->priv);
	if (ret)
		goto free_heap;
	ret = ion_page_pools_create(heap->cached_pools, orders, NUM_ORDERS,
				    true, data->priv);
	if (ret)
		goto destroy_uncached;
	ret = ion_page_pools_create(heap->reserved_pools, orders, NUM_ORDERS,
				    false, data->priv);
	if (ret)
		goto destroy_cached;
	for (i = 0; i < NUM_ORDERS; i++)
		ion_page_pool_prealloc(heap->reserved_pools[i], cam_reserved_counts[i]);

	if (IS_ENABLED(CONFIG_ION_POOL_AUTO_REFILL)) {
		ret = ion_page_pool_worker_start(&heap->kworker[0],
						 heap->uncached_pools,
						 NUM_ORDERS, false);
		if (ret)
			goto destroy_reserved;
		ret = ion_page_pool_worker_start(&heap->kworker[1],
						 heap->cached_pools,
						 NUM_ORDERS, true);
		if (ret) {
			ion_page_pool_worker_stop(&heap->kworker[0]);
			goto destroy_reserved;
		}
	}
	return &heap->heap;

destroy_reserved:
	ion_page_pools_destroy(heap->reserved_pools, NUM_ORDERS);
destroy_cached:
	ion_page_pools_destroy(heap->cached_pools, NUM_ORDERS);
destroy_uncached:
	ion_page_pools_destroy(heap->uncached_pools, NUM_ORDERS);
free_heap:
	kfree(heap);
	return ERR_PTR(ret);
}
