/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * Freestanding <string.h> for the vendored upstream LZ4 sources; lz4.c
 * includes it for memcpy()/memset(), which must resolve under -nostdinc.
 */
#ifndef __LZ4_FREESTANDING_STRING_H__
#define __LZ4_FREESTANDING_STRING_H__

#include <linux/string.h>

#endif
