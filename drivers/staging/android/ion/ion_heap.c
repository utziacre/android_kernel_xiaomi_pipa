// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/staging/android/ion/ion_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 */

#include <linux/err.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/rtmutex.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include "ion.h"

void *ion_heap_map_kernel(struct ion_heap *heap,
			  struct ion_buffer *buffer)
{
	struct scatterlist *sg;
	int i, j;
	void *vaddr;
	pgprot_t pgprot;
	struct sg_table *table = buffer->sg_table;
	int npages = PAGE_ALIGN(buffer->size) / PAGE_SIZE;
	struct page **pages = vmalloc(array_size(npages,
						 sizeof(struct page *)));
	struct page **tmp = pages;

	if (!pages)
		return ERR_PTR(-ENOMEM);

	if (buffer->flags & ION_FLAG_CACHED)
		pgprot = PAGE_KERNEL;
	else
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	for_each_sg(table->sgl, sg, table->nents, i) {
		int npages_this_entry = PAGE_ALIGN(sg->length) / PAGE_SIZE;
		struct page *page = sg_page(sg);

		BUG_ON(i >= npages);
		for (j = 0; j < npages_this_entry; j++)
			*(tmp++) = page++;
	}
	vaddr = vmap(pages, npages, VM_MAP, pgprot);
	vfree(pages);

	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	return vaddr;
}

void ion_heap_unmap_kernel(struct ion_heap *heap,
			   struct ion_buffer *buffer)
{
	vunmap(buffer->vaddr);
}

int ion_heap_map_user(struct ion_heap *heap, struct ion_buffer *buffer,
		      struct vm_area_struct *vma)
{
	struct sg_table *table = buffer->sg_table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	int i;
	int ret;

	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);
		if (ret)
			return ret;
		addr += len;
		if (addr >= vma->vm_end)
			return 0;
	}
	return 0;
}

#define ION_HEAP_ZERO_BATCH	32

static int ion_heap_clear_pages(struct page **pages, int num, pgprot_t pgprot)
{
	void *addr = vmap(pages, num, VM_MAP, pgprot);

	if (!addr)
		return -ENOMEM;
	memset(addr, 0, PAGE_SIZE * num);
	vunmap(addr);

	return 0;
}

/*
 * Zero an uncached buffer.  The pages are handed to devices with
 * DMA_ATTR_SKIP_CPU_SYNC, so this has to go through a write-combine mapping;
 * a cached zero would leave dirty lines the device never observes.
 *
 * Map the buffer in one piece instead of in 32 page batches.  Every vunmap()
 * broadcasts a TLB invalidation to all CPUs, so a 20MB release used to cost
 * 160 of them, and the deferred-free workqueue runs num_possible_cpus()
 * workers in parallel.  A burst of video or camera releases therefore turned
 * into a TLB shootdown storm that stalled unrelated threads -- audioserver
 * being the one that shows up as multi-hundred-millisecond latency.
 */
static int ion_heap_sglist_zero(struct scatterlist *sgl, unsigned int nents,
				pgprot_t pgprot)
{
	struct sg_page_iter piter;
	struct page *batch[ION_HEAP_ZERO_BATCH];
	struct page **pages;
	void *addr;
	unsigned int count = 0;
	unsigned int i = 0;
	int ret = 0;

	for_each_sg_page(sgl, &piter, nents, 0)
		count++;

	if (!count)
		return 0;

	/* Small buffers never amortise a page array, keep the simple path. */
	if (count <= ION_HEAP_ZERO_BATCH) {
		for_each_sg_page(sgl, &piter, nents, 0)
			batch[i++] = sg_page_iter_page(&piter);

		return ion_heap_clear_pages(batch, count, pgprot);
	}

	pages = kvmalloc_array(count, sizeof(*pages), GFP_KERNEL | __GFP_NOWARN);
	if (!pages)
		return -ENOMEM;

	for_each_sg_page(sgl, &piter, nents, 0)
		pages[i++] = sg_page_iter_page(&piter);

	addr = vmap(pages, count, VM_MAP, pgprot);
	if (addr) {
		memset(addr, 0, PAGE_SIZE * count);
		vunmap(addr);
		goto out;
	}

	/*
	 * The vmalloc area could not take the whole buffer at once.  Fall back
	 * to batched mappings so the release still succeeds.
	 */
	for (i = 0; i < count; i += ION_HEAP_ZERO_BATCH) {
		unsigned int num = min_t(unsigned int, ION_HEAP_ZERO_BATCH,
					 count - i);

		ret = ion_heap_clear_pages(pages + i, num, pgprot);
		if (ret)
			break;
	}

out:
	kvfree(pages);
	return ret;
}

int ion_heap_buffer_zero(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	struct sg_page_iter piter;

	/*
	 * Uncached buffers have to be zeroed through a write-combine mapping:
	 * they are mapped for DMA with DMA_ATTR_SKIP_CPU_SYNC, so leaving dirty
	 * cache lines behind would hand a device stale contents.
	 */
	if (!(buffer->flags & ION_FLAG_CACHED))
		return ion_heap_sglist_zero(table->sgl, table->nents,
					    pgprot_writecombine(PAGE_KERNEL));

	/*
	 * Cached buffers can be zeroed through the linear mapping instead.
	 * clear_page() uses dc zva where it is available, so it does not read
	 * the old contents first, and this does away with one vmap()/vunmap()
	 * pair per 32 pages: every vunmap() broadcasts a TLB invalidation to
	 * all CPUs, which is a very expensive way to free a buffer.
	 */
	for_each_sg_page(table->sgl, &piter, table->nents, 0)
		clear_page(page_address(sg_page_iter_page(&piter)));

	return 0;
}

int ion_heap_pages_zero(struct page *page, size_t size, pgprot_t pgprot)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	return ion_heap_sglist_zero(&sg, 1, pgprot);
}

static unsigned long ion_heap_shrink_count(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
	struct ion_heap *heap = container_of(shrinker, typeof(*heap), shrinker);

	if (heap->ops->shrink)
		return heap->ops->shrink(heap, sc->gfp_mask, 0);

	return 0;
}

static unsigned long ion_heap_shrink_scan(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	struct ion_heap *heap = container_of(shrinker, typeof(*heap), shrinker);

	if (heap->ops->shrink)
		return heap->ops->shrink(heap, sc->gfp_mask, sc->nr_to_scan);

	return 0;
}

int ion_heap_init_shrinker(struct ion_heap *heap)
{
	heap->shrinker.count_objects = ion_heap_shrink_count;
	heap->shrinker.scan_objects = ion_heap_shrink_scan;
	heap->shrinker.seeks = DEFAULT_SEEKS;
	heap->shrinker.batch = 0;

	return register_shrinker(&heap->shrinker);
}

struct ion_heap *ion_heap_create(struct ion_platform_heap *heap_data)
{
	struct ion_heap *heap = NULL;
	int heap_type = heap_data->type;

	switch (heap_type) {
	case ION_HEAP_TYPE_SYSTEM_CONTIG:
		pr_err("%s: Heap type is disabled: %d\n", __func__,
		       heap_data->type);
		break;
	case ION_HEAP_TYPE_SYSTEM:
		heap = ion_system_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CARVEOUT:
		heap = ion_carveout_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CHUNK:
		heap = ion_chunk_heap_create(heap_data);
		break;
#ifdef CONFIG_CMA
	case (enum ion_heap_type)ION_HEAP_TYPE_SECURE_DMA:
		heap = ion_secure_cma_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_DMA:
		heap = ion_cma_heap_create(heap_data);
		break;
	case (enum ion_heap_type)ION_HEAP_TYPE_HYP_CMA:
		heap = ion_cma_secure_heap_create(heap_data);
		break;
#endif
	case (enum ion_heap_type)ION_HEAP_TYPE_SYSTEM_SECURE:
		heap = ion_system_secure_heap_create(heap_data);
		break;
	case (enum ion_heap_type)ION_HEAP_TYPE_SECURE_CARVEOUT:
		heap = ion_secure_carveout_heap_create(heap_data);
		break;
#ifdef CONFIG_ION_CAMERA_HEAP
	case (enum ion_heap_type)ION_HEAP_TYPE_CAMERA:
		heap = ion_camera_heap_create(heap_data);
		break;
#endif
	default:
		pr_err("%s: Invalid heap type %d\n", __func__,
		       heap_data->type);
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR_OR_NULL(heap)) {
		pr_err("%s: error creating heap %s type %d base %pa size %zu\n",
		       __func__, heap_data->name, heap_data->type,
		       &heap_data->base, heap_data->size);
		return ERR_PTR(-EINVAL);
	}

	heap->name = heap_data->name;
	heap->id = heap_data->id;
	heap->priv = heap_data->priv;
	return heap;
}
EXPORT_SYMBOL(ion_heap_create);
