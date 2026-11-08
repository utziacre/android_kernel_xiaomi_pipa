/*
 * Copyright (c) 2019. Xiaomi Technology Co.
 *
 * All Rights Reserved.
 *
 * Confidential and Proprietary - Xiaomi Technology Co.
 */

#ifndef _ION_CAMERA_HEAP_H
#define _ION_CAMERA_HEAP_H

#include "ion.h"

#ifndef CONFIG_ALLOC_BUFFERS_IN_4K_CHUNKS
#if defined(CONFIG_IOMMU_IO_PGTABLE_ARMV7S)
static const unsigned int orders[] = {8, 4, 0};
static const unsigned int cam_reserved_counts[] = {1024, 2048, 3096};
#else
static const unsigned int orders[] = {8, 4, 0};
static const unsigned int cam_reserved_counts[] = {20, 1600, 35840};
#endif
#else
static const unsigned int orders[] = {0};
static const unsigned int cam_reserved_counts[] = {0};
#endif

#define NUM_ORDERS ARRAY_SIZE(orders)

struct ion_camera_heap {
	struct ion_heap heap;
	struct ion_page_pool *uncached_pools[NUM_ORDERS];
	struct ion_page_pool *cached_pools[NUM_ORDERS];
	struct ion_page_pool *reserved_pools[NUM_ORDERS];
	struct ion_page_pool_worker kworker[2];
};

#endif /* _ION_CAMERA_HEAP_H */
