// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2020 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_map {
	struct device *dev;
	struct list_head data_node;
	struct list_head dev_node;
	struct sg_table table;
	/* Protects DMA state; never acquire the device list lock while held. */
	struct mutex lock;
	/* One buffer reference, plus one while registered on the device. */
	refcount_t refs;
	enum dma_data_direction dir;
	unsigned long attrs;
	bool mapped;
};

static unsigned long msm_iommu_map_attrs(unsigned long attrs)
{
	/* CPU ownership is per call, independent of the cached translation. */
	return attrs & ~(DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_DELAYED_UNMAP);
}

static struct msm_iommu_map *
msm_iommu_map_lookup(struct msm_iommu_data *data, struct device *dev,
		     int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &data->map_list, data_node) {
		if (map->dev == dev && map->dir == dir &&
		    map->table.orig_nents == nents &&
		    msm_iommu_map_attrs(map->attrs) == msm_iommu_map_attrs(attrs)) {
			if (data->map_list.next != &map->data_node)
				list_move(&map->data_node, &data->map_list);
			return map;
		}
	}
	return NULL;
}

static void msm_iommu_map_put(struct msm_iommu_map *map)
{
	if (refcount_dec_and_test(&map->refs)) {
		struct device *dev = map->dev;

		sg_free_table(&map->table);
		kfree(map);
		put_device(dev);
	}
}

static void msm_iommu_map_unmap(struct msm_iommu_map *map)
{
	if (!map->mapped)
		return;

	dma_unmap_sg_attrs(map->dev, map->table.sgl, map->table.orig_nents,
			   map->dir, map->attrs | DMA_ATTR_SKIP_CPU_SYNC);
	map->mapped = false;
}

static void msm_iommu_map_unregister(struct msm_iommu_map *map)
{
	struct device *dev = map->dev;
	bool registered;

	mutex_lock(&dev->iommu_map_lock);
	registered = !list_empty(&map->dev_node);
	list_del_init(&map->dev_node);
	mutex_unlock(&dev->iommu_map_lock);
	if (registered)
		msm_iommu_map_put(map);
}

static struct msm_iommu_map *
msm_iommu_map_alloc(struct device *dev, struct scatterlist *sgl, int nents,
		    enum dma_data_direction dir, unsigned long attrs)
{
	struct msm_iommu_map *map;
	struct scatterlist *s, *d;
	int i;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return NULL;
	if (sg_alloc_table(&map->table, nents, GFP_KERNEL)) {
		kfree(map);
		return NULL;
	}

	d = map->table.sgl;
	for_each_sg(sgl, s, nents, i) {
		sg_set_page(d, sg_page(s), s->length, s->offset);
		d = sg_next(d);
	}
	map->dev = get_device(dev);
	map->dir = dir;
	map->attrs = attrs;
	mutex_init(&map->lock);
	refcount_set(&map->refs, 1);
	INIT_LIST_HEAD(&map->dev_node);
	return map;
}

static void msm_iommu_map_copy(struct msm_iommu_map *map,
			       struct scatterlist *sgl)
{
	struct scatterlist *s = map->table.sgl, *d = sgl;
	unsigned int i;

	for (i = 0; i < map->table.nents; i++) {
		sg_dma_address(d) = sg_dma_address(s);
		sg_dma_len(d) = sg_dma_len(s);
		d = sg_next(d);
		s = sg_next(s);
	}
	/* DMA segments form a packed prefix. Clear a previous, longer prefix. */
	for (; i < map->table.orig_nents && sg_dma_len(d); i++) {
		sg_dma_address(d) = 0;
		sg_dma_len(d) = 0;
		d = sg_next(d);
	}
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
			 enum dma_data_direction dir, struct dma_buf *dmabuf,
			 unsigned long attrs)
{
	struct msm_iommu_data *data;
	struct msm_iommu_map *map;
	bool new_map = false;
	bool register_map = false;
	int count = 0;

	if (!dev || !dmabuf || !sgl || nents <= 0 || !valid_dma_direction(dir))
		return 0;
	data = dmabuf->priv;

	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev, nents, dir, attrs);
	if (!map) {
		/* Allocate bookkeeping before installing any IOMMU translations. */
		map = msm_iommu_map_alloc(dev, sgl, nents, dir, attrs);
		if (!map)
			goto unlock_data;
		new_map = true;
	}

	mutex_lock(&map->lock);
	if (!map->mapped) {
		count = dma_map_sg_attrs(dev, map->table.sgl, nents, dir, attrs);
		if (count <= 0)
			goto unlock_map;
		map->table.nents = count;
		map->mapped = true;
		register_map = true;
	} else {
		if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
			dma_sync_sg_for_device(dev, map->table.sgl, nents, dir);
		if (is_device_dma_coherent(dev) ||
		    (attrs & DMA_ATTR_FORCE_COHERENT))
			dmb(ish);
		count = map->table.nents;
	}
	msm_iommu_map_copy(map, sgl);
unlock_map:
	mutex_unlock(&map->lock);

	if (count > 0) {
		if (new_map)
			list_add(&map->data_node, &data->map_list);
		if (register_map) {
			mutex_lock(&dev->iommu_map_lock);
			if (list_empty(&map->dev_node)) {
				refcount_inc(&map->refs);
				list_add(&map->dev_node, &dev->iommu_map_list);
			}
			mutex_unlock(&dev->iommu_map_lock);
		}
	} else if (new_map) {
		msm_iommu_map_put(map);
	}
unlock_data:
	mutex_unlock(&data->lock);
	return count > 0 ? count : 0;
}

void msm_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    struct dma_buf *dmabuf, unsigned long attrs)
{
	struct msm_iommu_data *data;
	struct msm_iommu_map *map;

	if (!dev || !dmabuf || !sgl || nents <= 0 || !valid_dma_direction(dir))
		return;
	data = dmabuf->priv;

	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev, nents, dir, attrs);
	if (map) {
		mutex_lock(&map->lock);
		if (map->mapped && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
			dma_sync_sg_for_cpu(dev, map->table.sgl, nents, dir);
		mutex_unlock(&map->lock);
	}
	/* The buffer, rather than each map/unmap pair, owns the cache entry. */
	mutex_unlock(&data->lock);
}

void msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_map *map;

	for (;;) {
		mutex_lock(&dev->iommu_map_lock);
		if (list_empty(&dev->iommu_map_list)) {
			mutex_unlock(&dev->iommu_map_lock);
			break;
		}
		map = list_first_entry(&dev->iommu_map_list,
				       struct msm_iommu_map, dev_node);
		refcount_inc(&map->refs);
		mutex_unlock(&dev->iommu_map_lock);

		/*
		 * Keep the entry registered until unmap completes. Other drainers
		 * must wait for it before detaching the IOMMU domain as well.
		 */
		mutex_lock(&map->lock);
		msm_iommu_map_unmap(map);
		mutex_unlock(&map->lock);
		msm_iommu_map_unregister(map);
		msm_iommu_map_put(map);
		cond_resched();
	}
}

void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	struct msm_iommu_map *map, *next;
	LIST_HEAD(maps);

	mutex_lock(&data->lock);
	list_splice_init(&data->map_list, &maps);
	mutex_unlock(&data->lock);

	list_for_each_entry_safe(map, next, &maps, data_node) {
		list_del(&map->data_node);

		mutex_lock(&map->lock);
		msm_iommu_map_unmap(map);
		mutex_unlock(&map->lock);
		msm_iommu_map_unregister(map);
		msm_iommu_map_put(map);
		cond_resched();
	}
}
