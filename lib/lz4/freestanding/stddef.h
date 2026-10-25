/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * Freestanding <stddef.h> for the vendored upstream LZ4 sources; -nostdinc
 * drops the compiler's copy, and lz4.h needs size_t and NULL.
 */
#ifndef __LZ4_FREESTANDING_STDDEF_H__
#define __LZ4_FREESTANDING_STDDEF_H__

#include <linux/stddef.h>	/* NULL, offsetof */
#include <linux/types.h>	/* size_t, ptrdiff_t */

#endif
