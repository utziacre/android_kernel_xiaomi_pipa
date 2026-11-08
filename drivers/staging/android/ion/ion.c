// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2019, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 Sultan Alsawaf <sultan@kerneltoast.com>.
 *
 */

#include <linux/cpumask.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "ion_secure_util.h"
#include "ion_system_secure_heap.h"

struct ion_dma_buf_attachment {
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	bool dma_mapped;
};

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static const struct file_operations ion_fops = {
	.unlocked_ioctl = ion_ioctl,
	.compat_ioctl = compat_ptr_ioctl
};

static struct ion_device ion_dev = {
	.heaps = PLIST_HEAD_INIT(ion_dev.heaps),
	.dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "ion",
		.fops = &ion_fops
	}
};

static struct ion_buffer *ion_dma_buf_to_buffer(struct dma_buf *dmabuf)
{
	return container_of(dmabuf->priv, struct ion_buffer, iommu_data);
}

static void ion_free_attachments(struct list_head *attachments)
{
	struct ion_dma_buf_attachment *a, *next;

	list_for_each_entry_safe(a, next, attachments, list) {
		list_del(&a->list);
		sg_free_table(&a->table);
		kfree(a);
	}
}

static void ion_buffer_free_work(struct work_struct *work)
{
	struct ion_buffer *buffer = container_of(work, typeof(*buffer), free);
	struct ion_heap *heap = buffer->heap;

	msm_dma_buf_freed(&buffer->iommu_data);
	ion_free_attachments(&buffer->attachments);
	ion_free_attachments(&buffer->map_freelist);
	if (buffer->vaddr)
		heap->ops->unmap_kernel(heap, buffer);
	heap->ops->free(buffer);
	kfree(buffer);
}

static struct ion_buffer *ion_buffer_create(struct ion_heap *heap, size_t len,
					    unsigned int flags)
{
	struct ion_buffer *buffer;
	int ret;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	*buffer = (typeof(*buffer)){
		.flags = flags,
		.heap = heap,
		.size = len,
		.free = __WORK_INITIALIZER(buffer->free, ion_buffer_free_work),
		.attachments = LIST_HEAD_INIT(buffer->attachments),
		.map_freelist = LIST_HEAD_INIT(buffer->map_freelist),
		.lock = __MUTEX_INITIALIZER(buffer->lock),
		.iommu_data = {
			.map_list = LIST_HEAD_INIT(buffer->iommu_data.map_list),
			.lock = __MUTEX_INITIALIZER(buffer->iommu_data.lock)
		}
	};

	ret = heap->ops->allocate(heap, buffer, len, flags);
	if (ret) {
		if (ret == -EINTR || !(heap->flags & ION_HEAP_FLAG_DEFER_FREE))
			goto free_buffer;

		flush_workqueue(heap->wq);
		ret = heap->ops->allocate(heap, buffer, len, flags);
		if (ret)
			goto free_buffer;
	}

	return buffer;

free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

static unsigned long
ion_dma_buf_map_attrs(struct ion_buffer *buffer,
		      struct dma_buf_attachment *attachment)
{
	unsigned long attrs = attachment->dma_map_attrs;

	if (!ion_buffer_cached(buffer) || !hlos_accessible_buffer(buffer))
		attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	return attrs;
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a = attachment->priv;
	unsigned long map_attrs = ion_dma_buf_map_attrs(buffer, attachment);
	int count;

	mutex_lock(&buffer->lock);
	if (map_attrs & DMA_ATTR_DELAYED_UNMAP)
		count = msm_dma_map_sg_attrs(attachment->dev, a->table.sgl,
					     a->table.nents, dir, dmabuf,
					     map_attrs);
	else
		count = dma_map_sg_attrs(attachment->dev, a->table.sgl,
					 a->table.nents, dir, map_attrs);
	if (count > 0)
		a->dma_mapped = true;
	mutex_unlock(&buffer->lock);
	if (count <= 0)
		return ERR_PTR(count < 0 ? count : -ENOMEM);

	return &a->table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a = attachment->priv;
	unsigned long map_attrs = ion_dma_buf_map_attrs(buffer, attachment);

	mutex_lock(&buffer->lock);
	if (map_attrs & DMA_ATTR_DELAYED_UNMAP)
		msm_dma_unmap_sg_attrs(attachment->dev, table->sgl,
				       table->nents, dir, dmabuf, map_attrs);
	else
		dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
				   dir, map_attrs);
	a->dma_mapped = false;
	mutex_unlock(&buffer->lock);
}

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_heap *heap = buffer->heap;

	if (!buffer->heap->ops->map_user)
		return -EINVAL;

	if (!(buffer->flags & ION_FLAG_CACHED))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return heap->ops->map_user(heap, buffer, vma);
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_heap *heap = buffer->heap;

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		queue_work(heap->wq, &buffer->free);
	else
		ion_buffer_free_work(&buffer->free);
}

static void *ion_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_heap *heap = buffer->heap;
	void *vaddr;

	if (!heap->ops->map_kernel)
		return ERR_PTR(-ENODEV);

	/*
	 * No lock here.  dma_buf_vmap() is the only caller of this op and it
	 * holds dmabuf->lock, so only one thread can ever be in the mapping
	 * branch, and it refcounts the callers itself.  The mapping is created
	 * once and then lives until the buffer is freed.
	 */
	if (!buffer->vaddr) {
		vaddr = heap->ops->map_kernel(heap, buffer);
		if (IS_ERR_OR_NULL(vaddr))
			return ERR_PTR(-EINVAL);

		buffer->vaddr = vaddr;
	}

	return buffer->vaddr;
}

static void ion_dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	/*
	 * Keep the mapping alive until ion_buffer_free_work() tears it down.
	 *
	 * unmap_kernel() is a vunmap(): it pulls the range back out of the
	 * global vmalloc space and broadcasts a TLB invalidation to every CPU.
	 * Clients map and unmap the same buffer over and over, so paying that on
	 * every last reference turned into a system wide shootdown storm for a
	 * mapping that was about to be created again.  The mapping is instead
	 * dropped once, when the buffer itself goes away.
	 */
}

static void *ion_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	void *vaddr;

	/*
	 * dma_buf_kmap() has no locking of its own, so ask the core for the
	 * mapping instead of calling ion_dma_buf_vmap() directly: dma_buf_vmap()
	 * serialises against itself and against dma_buf_vunmap().
	 */
	vaddr = dma_buf_vmap(dmabuf);
	if (!vaddr)
		return NULL;

	return vaddr + offset * PAGE_SIZE;
}

static void ion_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	if (!ptr)
		return;

	dma_buf_vunmap(dmabuf, ptr - offset * PAGE_SIZE);
}

static int ion_dup_sg_table(struct sg_table *dst, struct sg_table *src)
{
	struct scatterlist *d, *s;
	unsigned int i;
	int ret;

	ret = sg_alloc_table(dst, src->orig_nents, GFP_KERNEL);
	if (ret)
		return ret;

	/* Keep the destination's chain links and leave DMA state unmapped. */
	d = dst->sgl;
	for_each_sg(src->sgl, s, src->orig_nents, i) {
		sg_set_page(d, sg_page(s), s->length, s->offset);
		d = sg_next(d);
	}

	return 0;
}

static int ion_dma_buf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->map_freelist, list) {
		if (a->dev == attachment->dev) {
			list_move(&a->list, &buffer->attachments);
			mutex_unlock(&buffer->lock);
			attachment->priv = a;
			return 0;
		}
	}
	mutex_unlock(&buffer->lock);

	a = kmalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	if (ion_dup_sg_table(&a->table, buffer->sg_table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->dev = attachment->dev;
	a->dma_mapped = false;
	attachment->priv = a;
	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void ion_dma_buf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_move(&a->list, &buffer->map_freelist);
	mutex_unlock(&buffer->lock);
}

static int ion_dma_buf_cpu_access(struct dma_buf *dmabuf,
				  enum dma_data_direction dir, bool for_cpu)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (!(buffer->flags & ION_FLAG_CACHED))
		return 0;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->dma_mapped)
			continue;

		if (for_cpu)
			dma_sync_sg_for_cpu(a->dev, a->table.sgl,
					    a->table.nents, dir);
		else
			dma_sync_sg_for_device(a->dev, a->table.sgl,
					       a->table.nents, dir);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction dir)
{
	return ion_dma_buf_cpu_access(dmabuf, dir, true);
}

static int ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction dir)
{
	return ion_dma_buf_cpu_access(dmabuf, dir, false);
}

static void ion_sgl_sync_range(struct device *dev, struct scatterlist *sgl,
			       unsigned int nents, unsigned long offset,
			       unsigned long len, enum dma_data_direction dir,
			       bool for_cpu)
{
	dma_addr_t dma_addr = sg_dma_address(sgl);
	struct scatterlist *sg;
	int i;

	/* A contiguous IOVA can still span discontiguous physical segments. */
	for_each_sg(sgl, sg, nents, i) {
		unsigned long size;

		if (offset >= sg->length) {
			offset -= sg->length;
			goto next;
		}

		size = min_t(unsigned long, len, sg->length - offset);
		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, dma_addr, offset,
						      size, dir);
		else
			dma_sync_single_range_for_device(dev, dma_addr, offset,
							 size, dir);
		len -= size;
		if (!len)
			break;
		offset = 0;
next:
		dma_addr += sg->length;
	}
}

static int ion_dma_buf_cpu_access_partial(struct dma_buf *dmabuf,
					  enum dma_data_direction dir,
					  unsigned int offset, unsigned int len,
					  bool start)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (offset > buffer->size || len > buffer->size - offset)
		return -EINVAL;

	if (!len || !ion_buffer_cached(buffer))
		return 0;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->dma_mapped)
			continue;

		/* Partial sync requires a single contiguous DMA segment. */
		if (a->table.nents > 1 && sg_dma_len(sg_next(a->table.sgl))) {
			ret = -EINVAL;
			continue;
		}

		ion_sgl_sync_range(a->dev, a->table.sgl, a->table.nents, offset,
				   len, dir, start);
	}
	mutex_unlock(&buffer->lock);

	return ret;
}

static int ion_dma_buf_begin_cpu_access_partial(struct dma_buf *dmabuf,
						enum dma_data_direction dir,
						unsigned int offset,
						unsigned int len)
{
	return ion_dma_buf_cpu_access_partial(dmabuf, dir, offset, len, true);
}

static int ion_dma_buf_end_cpu_access_partial(struct dma_buf *dmabuf,
					      enum dma_data_direction dir,
					      unsigned int offset,
					      unsigned int len)
{
	return ion_dma_buf_cpu_access_partial(dmabuf, dir, offset, len, false);
}

static int ion_dma_buf_get_flags(struct dma_buf *dmabuf, unsigned long *flags)
{
	struct ion_buffer *buffer = ion_dma_buf_to_buffer(dmabuf);

	*flags = buffer->flags;
	return 0;
}

static const struct dma_buf_ops ion_dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.attach = ion_dma_buf_attach,
	.detach = ion_dma_buf_detach,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.begin_cpu_access_partial = ion_dma_buf_begin_cpu_access_partial,
	.end_cpu_access_partial = ion_dma_buf_end_cpu_access_partial,
	.map = ion_dma_buf_kmap,
	.unmap = ion_dma_buf_kunmap,
	.vmap = ion_dma_buf_vmap,
	.vunmap = ion_dma_buf_vunmap,
	.get_flags = ion_dma_buf_get_flags
};

static bool ion_camera_heap_present(void)
{
	struct ion_heap *heap;

	plist_for_each_entry(heap, &ion_dev.heaps, node)
		if (heap->id == ION_CAMERA_HEAP_ID)
			return true;

	return false;
}

/*
 * Xiaomi's camera HAL allocates from the system heap and relies on the kernel
 * to redirect those buffers to the dedicated camera heap.
 */
static unsigned int ion_redirect_camera_heap(unsigned int heap_id_mask)
{
	char task_comm[TASK_COMM_LEN];

	if (heap_id_mask != ION_HEAP(ION_SYSTEM_HEAP_ID) &&
	    heap_id_mask != (ION_HEAP(ION_SYSTEM_HEAP_ID) |
			     ION_HEAP(ION_CAMERA_HEAP_ID)))
		return heap_id_mask;

	get_task_comm(task_comm, current->group_leader);
	if (!strstr(task_comm, "provider@") &&
	    !strstr(task_comm, ".android.camera"))
		return heap_id_mask;

	if (!ion_camera_heap_present())
		return heap_id_mask;

	return ION_HEAP(ION_CAMERA_HEAP_ID);
}

struct dma_buf *ion_alloc_dmabuf(size_t len, unsigned int heap_id_mask,
				 unsigned int flags)
{
	struct ion_device *idev = &ion_dev;
	struct dma_buf_export_info exp_info;
	struct ion_buffer *buffer = NULL;
	struct dma_buf *dmabuf;
	struct ion_heap *heap;

	if (len > SIZE_MAX - (PAGE_SIZE - 1))
		return ERR_PTR(-EINVAL);

	len = PAGE_ALIGN(len);
	if (!len)
		return ERR_PTR(-EINVAL);

	heap_id_mask = ion_redirect_camera_heap(heap_id_mask);

	plist_for_each_entry(heap, &idev->heaps, node) {
		if (BIT(heap->id) & heap_id_mask) {
			buffer = ion_buffer_create(heap, len, flags);
			if (!IS_ERR(buffer) || PTR_ERR(buffer) == -EINTR)
				break;
		}
	}

	if (!buffer)
		return ERR_PTR(-ENODEV);

	if (IS_ERR(buffer))
		return ERR_CAST(buffer);

	exp_info = (typeof(exp_info)){
		.ops = &ion_dma_buf_ops,
		.size = buffer->size,
		.flags = O_RDWR,
		.priv = &buffer->iommu_data
	};

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf))
		ion_buffer_free_work(&buffer->free);

	return dmabuf;
}

static int ion_alloc_fd(struct ion_allocation_data *a, __u32 __user *user_fd)
{
	struct dma_buf *dmabuf;
	int fd, ret;

	dmabuf = ion_alloc_dmabuf(a->len, a->heap_id_mask, a->flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto put_dmabuf;
	}

	/* Publish the file only after userspace has received the descriptor. */
	if (put_user(fd, user_fd)) {
		put_unused_fd(fd);
		ret = -EFAULT;
		goto put_dmabuf;
	}

	fd_install(fd, dmabuf->file);
	return 0;

put_dmabuf:
	dma_buf_put(dmabuf);
	return ret;
}

void ion_add_heap(struct ion_device *idev, struct ion_heap *heap)
{
	struct ion_heap_data *hdata = &idev->heap_data[idev->heap_count];

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE) {
		/*
		 * max_active is a global limit for an unbound workqueue, not a
		 * per-CPU one: on this non-NUMA system the workqueue owns a
		 * single pool_workqueue, so a max_active of 1 serialises the
		 * frees for the whole system and a burst of releases keeps its
		 * pages away from the allocator far longer than it has to.
		 *
		 * The ceiling is still the number of CPUs an unbound worker can
		 * actually run on.  workqueue_init_early() seeds
		 * wq_unbound_cpumask from cpu_lp_mask, which this kernel sets to
		 * the little cluster (CONFIG_LITTLE_CPU_MASK), so the big and
		 * prime cores never execute an unbound worker at all.  Asking
		 * for more than that would only queue work up inside the pool.
		 */
		heap->wq = alloc_workqueue("%s", WQ_UNBOUND | WQ_MEM_RECLAIM,
					   cpumask_weight(cpu_lp_mask),
					   heap->name);
		BUG_ON(!heap->wq);
	}

	if (heap->ops->shrink)
		ion_heap_init_shrinker(heap);

	plist_node_init(&heap->node, -heap->id);
	plist_add(&heap->node, &idev->heaps);

	strlcpy(hdata->name, heap->name, sizeof(hdata->name));
	hdata->type = heap->type;
	hdata->heap_id = heap->id;
	idev->heap_count++;
}

static int ion_walk_heaps(int heap_id, int type, void *data,
			  int (*f)(struct ion_heap *heap, void *data))
{
	struct ion_device *idev = &ion_dev;
	struct ion_heap *heap;
	int ret = 0;

	plist_for_each_entry(heap, &idev->heaps, node) {
		if (heap->type == type && ION_HEAP(heap->id) == heap_id) {
			ret = f(heap, data);
			break;
		}
	}

	return ret;
}

static int ion_query_heaps(struct ion_heap_query *query)
{
	struct ion_device *idev = &ion_dev;

	if (!query->cnt)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(query->heaps), idev->heap_data,
			 min(query->cnt, idev->heap_count) *
			 sizeof(*idev->heap_data)))
		return -EFAULT;

	return 0;
}

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ion_device *idev = &ion_dev;
	union {
		struct ion_allocation_data allocation;
		struct ion_prefetch_data prefetch;
		struct ion_heap_query query;
	} data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case ION_IOC_ALLOC:
		if (copy_from_user(&data, argp,
				   sizeof(struct ion_allocation_data)))
			return -EFAULT;

		return ion_alloc_fd(&data.allocation, argp +
				    offsetof(struct ion_allocation_data, fd));
	case ION_IOC_HEAP_QUERY:
		/* The data used in ion_heap_query ends at `heaps` */
		if (copy_from_user(&data, argp,
				   offsetof(struct ion_heap_query, heaps) +
				   sizeof(data.query.heaps)))
			return -EFAULT;

		if (data.query.heaps)
			return ion_query_heaps(&data.query);

		return put_user(idev->heap_count, (__u32 __user *)argp);
	case ION_IOC_PREFETCH:
	case ION_IOC_DRAIN:
		/* The data used in ion_prefetch_data begins at `regions` */
		if (copy_from_user(&data.prefetch.regions,
				   argp +
				   offsetof(struct ion_prefetch_data, regions),
				   sizeof(struct ion_prefetch_data) -
				   offsetof(struct ion_prefetch_data, regions)))
			return -EFAULT;

		return ion_walk_heaps(data.prefetch.heap_id,
				      ION_HEAP_TYPE_SYSTEM_SECURE,
				      &data.prefetch,
				      cmd == ION_IOC_PREFETCH ?
				      ion_system_secure_heap_prefetch :
				      ion_system_secure_heap_drain);
	default:
		return -ENOTTY;
	}
}

struct ion_device *ion_device_create(struct ion_heap_data *heap_data)
{
	struct ion_device *idev = &ion_dev;
	int ret;

	idev->heap_data = heap_data;
	ret = misc_register(&idev->dev);
	if (ret)
		return ERR_PTR(ret);

	return idev;
}
