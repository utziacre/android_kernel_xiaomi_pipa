// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
/*
 * Copyright (C) 2011 - 2016, Yann Collet.
 * Copyright (C) 2016, Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * LZ4 compressor -- kernel entry points
 *
 * The compressor is upstream's, in the verbatim lz4.c included below.  The
 * kernel has no heap at these call sites, so LZ4_compress_default(),
 * LZ4_compress_fast() and LZ4_compress_destSize() take a trailing wrkmem
 * argument and forward to upstream's *_extState() entry points.
 */

#include "lz4_deps.h"
#include "upstream/lz4.c"

/* Upstream's short names for these clash with <linux/minmax.h>. */
#undef MIN
#undef MAX
#undef KB
#undef MB
#undef GB

#include "lz4_kernel_api.h"
#include <linux/export.h>
#include <linux/module.h>

/* Catch any divergence from upstream's layout at build time. */
static_assert(sizeof(LZ4_stream_t) == LZ4_STREAM_MINSIZE);
static_assert(LZ4_STREAM_MINSIZE == LZ4_MEM_COMPRESS);

int LZ4_compress_fast(const char *source, char *dest, int inputSize,
		      int maxOutputSize, int acceleration, void *wrkmem)
{
	return LZ4_compress_fast_extState(wrkmem, source, dest, inputSize,
					  maxOutputSize, acceleration);
}
EXPORT_SYMBOL(LZ4_compress_fast);

int LZ4_compress_default(const char *source, char *dest, int inputSize,
			 int maxOutputSize, void *wrkmem)
{
	return LZ4_compress_fast(source, dest, inputSize, maxOutputSize,
				 LZ4_ACCELERATION_DEFAULT, wrkmem);
}
EXPORT_SYMBOL(LZ4_compress_default);

int LZ4_compress_destSize(const char *source, char *dest, int *sourceSizePtr,
			  int targetDestSize, void *wrkmem)
{
	return LZ4_compress_destSize_extState(wrkmem, source, dest,
					      sourceSizePtr, targetDestSize,
					      LZ4_ACCELERATION_DEFAULT);
}
EXPORT_SYMBOL(LZ4_compress_destSize);

void LZ4_resetStream(LZ4_stream_t *LZ4_stream)
{
	__lz4_resetStream(LZ4_stream);
}

int LZ4_loadDict(LZ4_stream_t *streamPtr, const char *dictionary, int dictSize)
{
	return __lz4_loadDict(streamPtr, dictionary, dictSize);
}
EXPORT_SYMBOL(LZ4_loadDict);

int LZ4_saveDict(LZ4_stream_t *streamPtr, char *safeBuffer, int dictSize)
{
	return __lz4_saveDict(streamPtr, safeBuffer, dictSize);
}
EXPORT_SYMBOL(LZ4_saveDict);

int LZ4_compress_fast_continue(LZ4_stream_t *streamPtr, const char *src,
			       char *dst, int srcSize, int maxDstSize,
			       int acceleration)
{
	return __lz4_compress_fast_continue(streamPtr, src, dst, srcSize,
					    maxDstSize, acceleration);
}
EXPORT_SYMBOL(LZ4_compress_fast_continue);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 compressor");
