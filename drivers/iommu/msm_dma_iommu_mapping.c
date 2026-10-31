// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2020 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_map {
	struct device *dev;
	struct msm_iommu_data *data;
	struct list_head data_node;
	struct list_head dev_node;
	struct scatterlist *sgl;
	enum dma_data_direction dir;
	unsigned long attrs;
	int nents;
	int refcount;
};

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_data *data,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &data->map_list, data_node) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static void msm_iommu_map_free(struct msm_iommu_map *map)
{
	struct sg_table table = {
		.sgl = map->sgl,
		.nents = map->nents,
		.orig_nents = map->nents
	};

	dma_unmap_sg_attrs(map->dev, map->sgl, map->nents, map->dir,
			   map->attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(&table);
	list_del(&map->data_node);
	list_del(&map->dev_node);
	kfree(map);
}

static struct scatterlist *clone_sgl(struct scatterlist *sgl, int nents)
{
	struct scatterlist *d, *s;
	struct sg_table table;

	sg_alloc_table(&table, nents, GFP_KERNEL | __GFP_NOFAIL);
	for (d = table.sgl, s = sgl;
	     nents > SG_MAX_SINGLE_ALLOC; nents -= SG_MAX_SINGLE_ALLOC - 1,
	     d = sg_chain_ptr(&d[SG_MAX_SINGLE_ALLOC - 1]),
	     s = sg_chain_ptr(&s[SG_MAX_SINGLE_ALLOC - 1]))
		memcpy(d, s, (SG_MAX_SINGLE_ALLOC - 1) * sizeof(*d));

	if (nents)
		memcpy(d, s, nents * sizeof(*d));

	return table.sgl;
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
			 enum dma_data_direction dir, struct dma_buf *dmabuf,
			 unsigned long attrs)
{
	struct msm_iommu_data *data = dmabuf->priv;
	struct msm_iommu_map *map;

	/*
	 * A mapping is identified by (buffer, device), so data->lock -- the
	 * per-buffer lock -- is what has to serialise this.  dev->iommu_map_lock
	 * used to be held across dma_map_sg_attrs() as well, which made every
	 * mapping on the device wait for every other one: mapping a 20MB video
	 * buffer (thousands of page table entries) blocked a 64KB one behind it
	 * for the whole walk.
	 */
	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (map) {
		struct scatterlist *d = sgl, *s = map->sgl;

		map->refcount++;
		do {
			d->dma_address = s->dma_address;
			d->dma_length = s->dma_length;
		} while ((s = sg_next(s)) && s->dma_length && (d = sg_next(d)));
		if (is_device_dma_coherent(dev))
			dmb(ish);

		mutex_unlock(&data->lock);
		return nents;
	}

	/*
	 * Slow path.  Nobody else can map this buffer while we hold data->lock,
	 * so dev->iommu_map_lock is only needed to publish the bookkeeping, and
	 * it is taken once the mapping exists rather than around it.
	 */
	if (!dma_map_sg_attrs(dev, sgl, nents, dir, attrs)) {
		mutex_unlock(&data->lock);
		return 0;
	}

	map = kmalloc(sizeof(*map), GFP_KERNEL | __GFP_NOFAIL);
	map->sgl = clone_sgl(sgl, nents);
	map->data = data;
	map->dev = dev;
	map->dir = dir;
	map->nents = nents;
	map->refcount = 2;
	map->attrs = attrs;

	mutex_lock(&dev->iommu_map_lock);
	list_add(&map->data_node, &data->map_list);
	list_add(&map->dev_node, &dev->iommu_map_list);
	mutex_unlock(&dev->iommu_map_lock);

	mutex_unlock(&data->lock);

	return nents;
}

void msm_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    struct dma_buf *dmabuf, unsigned long attrs)
{
	struct msm_iommu_data *data = dmabuf->priv;
	struct msm_iommu_map *map;

	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (map && !--map->refcount) {
		mutex_lock(&dev->iommu_map_lock);
		msm_iommu_map_free(map);
		mutex_unlock(&dev->iommu_map_lock);
	}
	mutex_unlock(&data->lock);
}

void msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_map *map, *tmp;
	bool pending;

	/*
	 * Every other path takes data->lock first and dev->iommu_map_lock second,
	 * so blocking on data->lock here would invert that order and deadlock.
	 * Retry a buffer that is busy instead; data->lock is only ever held across
	 * one page table update, so the next sweep gets it.
	 */
	do {
		pending = false;
		mutex_lock(&dev->iommu_map_lock);
		list_for_each_entry_safe(map, tmp, &dev->iommu_map_list,
					 dev_node) {
			struct msm_iommu_data *data = map->data;

			if (!mutex_trylock(&data->lock)) {
				pending = true;
				continue;
			}

			msm_iommu_map_free(map);
			mutex_unlock(&data->lock);
		}
		mutex_unlock(&dev->iommu_map_lock);

		if (pending)
			cond_resched();
	} while (pending);
}

void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	struct msm_iommu_map *map, *tmp;

	mutex_lock(&data->lock);
	list_for_each_entry_safe(map, tmp, &data->map_list, data_node) {
		struct device *dev = map->dev;

		mutex_lock(&dev->iommu_map_lock);
		msm_iommu_map_free(map);
		mutex_unlock(&dev->iommu_map_lock);
	}
	mutex_unlock(&data->lock);
}
