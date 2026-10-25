/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * lz4_kernel_api.h -- hand the LZ4 names back to the kernel
 *
 * Undo lz4_deps.h's renaming and pull in <linux/lz4.h>.  Include after
 * upstream/lz4.c or upstream/lz4hc.c.
 */

#undef LZ4_compress_fast
#undef LZ4_compress_default
#undef LZ4_compress_destSize
#undef LZ4_resetStream
#undef LZ4_loadDict
#undef LZ4_saveDict
#undef LZ4_compress_fast_continue

#undef LZ4_decompress_safe
#undef LZ4_decompress_safe_partial
#undef LZ4_decompress_fast
#undef LZ4_setStreamDecode
#undef LZ4_decompress_safe_continue
#undef LZ4_decompress_fast_continue
#undef LZ4_decompress_safe_usingDict
#undef LZ4_decompress_fast_usingDict

#undef LZ4_compress_HC
#undef LZ4_resetStreamHC
#undef LZ4_loadDictHC
#undef LZ4_compress_HC_continue
#undef LZ4_saveDictHC

#include <linux/lz4.h>
