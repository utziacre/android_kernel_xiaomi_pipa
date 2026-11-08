// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/staging/android/ion/ion_system_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2020, The Linux Foundation. All rights reserved.
 *
 */

#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <soc/qcom/secure_buffer.h>
#include "ion.h"
#include "ion_system_heap.h"
#include "ion_system_secure_heap.h"
#include "ion_secure_util.h"

static const gfp_t low_order_gfp_flags = GFP_HIGHUSER | __GFP_ZERO;

bool pool_auto_refill_en  __read_mostly =
		IS_ENABLED(CONFIG_ION_POOL_AUTO_REFILL);

int order_to_index(unsigned int order)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		if (order == orders[i])
			return i;
	BUG();
	return -1;
}

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

int ion_heap_is_system_heap_type(enum ion_heap_type type)
{
	return type == ((enum ion_heap_type)ION_HEAP_TYPE_SYSTEM);
}

static struct page *alloc_buffer_page(struct ion_system_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long order,
				      bool *from_pool)
{
	int cached = (int)ion_buffer_cached(buffer);
	struct page *page;
	struct ion_page_pool *pool;
	int vmid = get_secure_vmid(buffer->flags);
	struct device *dev = heap->heap.priv;

	if (vmid > 0)
		pool = heap->secure_pools[vmid][order_to_index(order)];
	else if (!cached)
		pool = heap->uncached_pools[order_to_index(order)];
	else
		pool = heap->cached_pools[order_to_index(order)];

	page = ion_page_pool_alloc(pool, from_pool);

	if (pool_auto_refill_en && pool->order &&
	    pool_count_below_lowmark(pool) && vmid <= 0) {
		ion_page_pool_worker_wake(&heap->kworker[cached]);
	}

	if (IS_ERR(page))
		return page;

	if ((MAKE_ION_ALLOC_DMA_READY && vmid <= 0) || !(*from_pool))
		ion_pages_sync_for_device(dev, page, PAGE_SIZE << order,
					  DMA_BIDIRECTIONAL);

	return page;
}

/*
 * For secure pages that need to be freed and not added back to the pool; the
 *  hyp_unassign should be called before calling this function
 */
void free_buffer_page(struct ion_system_heap *heap,
		      struct ion_buffer *buffer, struct page *page,
		      unsigned int order)
{
	bool cached = ion_buffer_cached(buffer);
	int vmid = get_secure_vmid(buffer->flags);
	struct ion_page_pool *pool;
	unsigned int index = order_to_index(order);

	if (vmid > 0)
		pool = heap->secure_pools[vmid][index];
	else if (cached)
		pool = heap->cached_pools[index];
	else
		pool = heap->uncached_pools[index];

	mod_node_page_state(page_pgdat(page), NR_UNRECLAIMABLE_PAGES,
			    -(1 << order));
	if ((buffer->flags & ION_FLAG_POOL_FORCE_ALLOC) ||
	    (buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE))
		ion_page_pool_free_immediate(pool, page);
	else
		ion_page_pool_free(pool, page);
}

static int alloc_largest_available(struct ion_system_heap *heap,
				   struct ion_buffer *buffer,
				   unsigned long size, unsigned int max_order,
				   struct ion_page_info *info)
{
	struct page *page;
	int i;
	bool from_pool;

	for (i = 0; i < NUM_ORDERS; i++) {
		if (orders[i] > max_order || size < order_to_size(orders[i]))
			continue;
		from_pool = !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC);
		page = alloc_buffer_page(heap, buffer, orders[i], &from_pool);
		if (IS_ERR(page)) {
			if (PTR_ERR(page) == -EINTR)
				return -EINTR;
			continue;
		}
		info->page = page;
		info->order = orders[i];
		info->from_pool = from_pool;
		return 0;
	}
	return -ENOMEM;
}

static int alloc_from_pool_preferred(struct ion_system_heap *heap,
				     struct ion_buffer *buffer,
				     unsigned long size,
				     unsigned int max_order,
				     struct ion_page_info *info)
{
	struct page *page;
	int i;

	if (buffer->flags & ION_FLAG_POOL_FORCE_ALLOC)
		goto force_alloc;
	for (i = 0; i < NUM_ORDERS; i++) {
		if (orders[i] > max_order || size < order_to_size(orders[i]))
			continue;
		page = alloc_from_secure_pool_order(heap, buffer, orders[i]);
		if (IS_ERR(page))
			continue;
		info->page = page;
		info->order = orders[i];
		info->from_pool = true;
		return 0;
	}
	page = split_page_from_secure_pool(heap, buffer);
	if (!IS_ERR(page)) {
		info->page = page;
		info->order = 0;
		info->from_pool = true;
		return 0;
	}
force_alloc:
	return alloc_largest_available(heap, buffer, size, max_order, info);
}

static void process_info(struct ion_page_info *info, struct scatterlist *sg,
			 struct scatterlist *sg_sync)
{
	struct page *page = info->page;
	unsigned int size = PAGE_SIZE << info->order;

	if (sg_sync) {
		sg_set_page(sg_sync, page, size, 0);
		sg_dma_address(sg_sync) = page_to_phys(page);
	}
	sg_set_page(sg, page, size, 0);
	/* Preserve the physical-address convention used by Qualcomm heaps. */
	sg_dma_address(sg) = page_to_phys(page);
	list_del(&info->list);
}

static int ion_system_heap_allocate(struct ion_heap *heap,
				    struct ion_buffer *buffer,
				    unsigned long size,
				    unsigned long flags)
{
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table;
	struct sg_table table_sync = {0};
	struct scatterlist *sg;
	struct scatterlist *sg_sync;
	int ret = -ENOMEM;
	struct list_head pages;
	struct list_head pages_from_pool;
	struct ion_page_info *info, *tmp_info;
	struct ion_page_info_pool info_pool;
	int i = 0;
	unsigned int nents_sync = 0;
	unsigned long size_remaining = PAGE_ALIGN(size);
	unsigned int max_order = orders[0];
	unsigned int sz;
	int vmid = get_secure_vmid(buffer->flags);

	if (!size_remaining || size / PAGE_SIZE > totalram_pages / 2)
		return -ENOMEM;

	if (ion_heap_is_system_heap_type(buffer->heap->type) &&
	    is_secure_vmid_valid(vmid)) {
		pr_info("%s: System heap doesn't support secure allocations\n",
			__func__);
		return -EINVAL;
	}

	ion_page_info_pool_init(&info_pool);
	INIT_LIST_HEAD(&pages);
	INIT_LIST_HEAD(&pages_from_pool);

	while (size_remaining > 0) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto err;
		}
		info = ion_page_info_alloc(&info_pool);
		if (!info) {
			ret = -ENOMEM;
			goto err;
		}
		if (is_secure_vmid_valid(vmid))
			ret = alloc_from_pool_preferred(sys_heap, buffer,
							size_remaining, max_order, info);
		else
			ret = alloc_largest_available(sys_heap, buffer,
						      size_remaining, max_order, info);
		if (ret)
			goto err;

		sz = (1 << info->order) * PAGE_SIZE;

		mod_node_page_state(page_pgdat(info->page),
				    NR_UNRECLAIMABLE_PAGES,
				    (1 << (info->order)));

		if (info->from_pool) {
			list_add_tail(&info->list, &pages_from_pool);
		} else {
			list_add_tail(&info->list, &pages);
			if (vmid > 0)
				++nents_sync;
		}
		size_remaining -= sz;
		max_order = info->order;
		i++;
	}

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto err;
	}

	ret = sg_alloc_table(table, i, GFP_KERNEL);
	if (ret)
		goto err1;

	if (nents_sync) {
		ret = sg_alloc_table(&table_sync, nents_sync, GFP_KERNEL);
		if (ret)
			goto err_free_sg;
	}

	sg = table->sgl;
	sg_sync = table_sync.sgl;

	/*
	 * We now have two separate lists. One list contains pages from the
	 * pool and the other pages from buddy. We want to merge these
	 * together while preserving the ordering of the pages (higher order
	 * first).
	 */
	do {
		info = list_first_entry_or_null(&pages, struct ion_page_info, list);
		tmp_info = list_first_entry_or_null(&pages_from_pool,
						    struct ion_page_info, list);
		if (info && tmp_info) {
			if (info->order >= tmp_info->order) {
				process_info(info, sg, sg_sync);
				if (sg_sync)
					sg_sync = sg_next(sg_sync);
			} else {
				process_info(tmp_info, sg, NULL);
			}
		} else if (info) {
			process_info(info, sg, sg_sync);
			if (sg_sync)
				sg_sync = sg_next(sg_sync);
		} else if (tmp_info) {
			process_info(tmp_info, sg, NULL);
		}
		sg = sg_next(sg);

	} while (sg);

	if (nents_sync) {
		if (vmid > 0) {
			ret = ion_hyp_assign_sg(&table_sync, &vmid, 1, true);
			if (ret)
				goto err_free_sg2;
		}
	}

	buffer->sg_table = table;
	if (nents_sync)
		sg_free_table(&table_sync);
	ion_page_info_pool_destroy(&info_pool);
	return 0;

err_free_sg2:
	/* We failed to zero buffers. Bypass pool */
	buffer->private_flags |= ION_PRIV_FLAG_SHRINKER_FREE;

	if (vmid > 0)
		if (ion_hyp_unassign_sg(table, &vmid, 1, true, false))
			goto err_free_table_sync;

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg),
				 get_order(sg->length));
err_free_table_sync:
	if (nents_sync)
		sg_free_table(&table_sync);
err_free_sg:
	sg_free_table(table);
err1:
	kfree(table);
err:
	list_for_each_entry_safe(info, tmp_info, &pages, list) {
		free_buffer_page(sys_heap, buffer, info->page, info->order);
		list_del(&info->list);
	}
	list_for_each_entry_safe(info, tmp_info, &pages_from_pool, list) {
		free_buffer_page(sys_heap, buffer, info->page, info->order);
		list_del(&info->list);
	}
	ion_page_info_pool_destroy(&info_pool);
	return ret;
}

void ion_system_heap_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	int i;
	int vmid = get_secure_vmid(buffer->flags);

	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE) &&
	    !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC)) {
		if (vmid < 0 && ion_heap_buffer_zero(buffer))
			buffer->private_flags |= ION_PRIV_FLAG_SHRINKER_FREE;
	} else if (vmid > 0) {
		if (ion_hyp_unassign_sg(table, &vmid, 1, true, false))
			goto out_table;
	}

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg),
				 get_order(sg->length));

out_table:
	sg_free_table(table);
	kfree(table);
}

static int ion_system_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
				 int nr_to_scan)
{
	struct ion_system_heap *sys_heap =
		container_of(heap, struct ion_system_heap, heap);
	int i, j, freed = 0;

	/* Reclaim lower orders first, sharing one budget across all pools. */
	for (i = NUM_ORDERS - 1; i >= 0; i--) {
		for (j = 0; j < VMID_LAST; j++) {
			if (!is_secure_vmid_valid(j))
				continue;
			freed += ion_secure_page_pool_shrink(sys_heap, j, i,
					 nr_to_scan ? nr_to_scan - freed : 0);
			if (nr_to_scan && freed >= nr_to_scan)
				return freed;
		}
		freed += ion_page_pool_shrink(sys_heap->uncached_pools[i],
					   gfp_mask,
					   nr_to_scan ? nr_to_scan - freed : 0);
		if (nr_to_scan && freed >= nr_to_scan)
			return freed;
		freed += ion_page_pool_shrink(sys_heap->cached_pools[i],
					   gfp_mask,
					   nr_to_scan ? nr_to_scan - freed : 0);
		if (nr_to_scan && freed >= nr_to_scan)
			return freed;
	}
	return freed;
}

static struct ion_heap_ops system_heap_ops = {
	.allocate = ion_system_heap_allocate,
	.free = ion_system_heap_free,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
	.shrink = ion_system_heap_shrink,
};

struct ion_heap *ion_system_heap_create(struct ion_platform_heap *data)
{
	struct ion_system_heap *heap;
	int ret = -ENOMEM;
	int i;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->heap.ops = &system_heap_ops;
	heap->heap.type = ION_HEAP_TYPE_SYSTEM;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;
	heap->heap.priv = data->priv;

	for (i = 0; i < VMID_LAST; i++) {
		if (!is_secure_vmid_valid(i))
			continue;
		ret = ion_page_pools_create(heap->secure_pools[i], orders,
					    NUM_ORDERS, false, data->priv);
		if (ret)
			goto destroy_secure_pools;
	}

	ret = ion_page_pools_create(heap->uncached_pools, orders, NUM_ORDERS,
				    false, data->priv);
	if (ret)
		goto destroy_secure_pools;
	ret = ion_page_pools_create(heap->cached_pools, orders, NUM_ORDERS,
				    true, data->priv);
	if (ret)
		goto destroy_uncached_pools;

	if (pool_auto_refill_en) {
		ret = ion_page_pool_worker_start(&heap->kworker[0],
						 heap->uncached_pools,
						 NUM_ORDERS, false);
		if (ret)
			goto destroy_pools;
		ret = ion_page_pool_worker_start(&heap->kworker[1],
						 heap->cached_pools,
						 NUM_ORDERS, true);
		if (ret) {
			ion_page_pool_worker_stop(&heap->kworker[0]);
			goto destroy_pools;
		}
	}

	mutex_init(&heap->split_page_mutex);

	return &heap->heap;
destroy_pools:
	ion_page_pools_destroy(heap->cached_pools, NUM_ORDERS);
destroy_uncached_pools:
	ion_page_pools_destroy(heap->uncached_pools, NUM_ORDERS);
destroy_secure_pools:
	for (i = 0; i < VMID_LAST; i++) {
		ion_page_pools_destroy(heap->secure_pools[i], NUM_ORDERS);
	}
	kfree(heap);
	return ERR_PTR(ret);
}

static int ion_system_contig_heap_allocate(struct ion_heap *heap,
					   struct ion_buffer *buffer,
					   unsigned long len,
					   unsigned long flags)
{
	int order = get_order(len);
	struct page *page;
	struct sg_table *table;
	unsigned long i;
	int ret;

	page = alloc_pages(low_order_gfp_flags | __GFP_NOWARN, order);
	if (!page)
		return -ENOMEM;

	split_page(page, order);

	len = PAGE_ALIGN(len);
	for (i = len >> PAGE_SHIFT; i < (1 << order); i++)
		__free_page(page + i);

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto free_pages;
	}

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto free_table;

	sg_set_page(table->sgl, page, len, 0);

	buffer->sg_table = table;

	ion_pages_sync_for_device(NULL, page, len, DMA_BIDIRECTIONAL);

	return 0;

free_table:
	kfree(table);
free_pages:
	for (i = 0; i < len >> PAGE_SHIFT; i++)
		__free_page(page + i);

	return ret;
}

static void ion_system_contig_heap_free(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	struct page *page = sg_page(table->sgl);
	unsigned long pages = PAGE_ALIGN(buffer->size) >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < pages; i++)
		__free_page(page + i);
	sg_free_table(table);
	kfree(table);
}

static struct ion_heap_ops kmalloc_ops = {
	.allocate = ion_system_contig_heap_allocate,
	.free = ion_system_contig_heap_free,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
};

struct ion_heap *ion_system_contig_heap_create(struct ion_platform_heap *unused)
{
	struct ion_heap *heap;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->ops = &kmalloc_ops;
	heap->type = ION_HEAP_TYPE_SYSTEM_CONTIG;
	return heap;
}
