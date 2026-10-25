// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
/*
 * Copyright (C) 2011 - 2016, Yann Collet.
 * Copyright (C) 2016, Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * LZ4 decompressor -- kernel entry points
 *
 * The decompressor is upstream's, in the verbatim lz4.c included below.  The
 * API is signature-compatible, so every export is a plain forwarder.  This
 * file is also included by lib/decompress_unlz4.c for the pre-boot
 * decompressor, which defines STATIC.
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

#ifndef STATIC
#include <linux/export.h>
#include <linux/module.h>
#endif

static_assert(sizeof(LZ4_streamDecode_t) == LZ4_STREAMDECODE_MINSIZE);
static_assert(LZ4_STREAMDECODE_MINSIZE == LZ4_MEM_DECOMPRESS);

int LZ4_decompress_safe(const char *source, char *dest, int compressedSize,
			int maxDecompressedSize)
{
	return __lz4_decompress_safe(source, dest, compressedSize,
				     maxDecompressedSize);
}

int LZ4_decompress_safe_partial(const char *source, char *dest,
				int compressedSize, int targetOutputSize,
				int maxDecompressedSize)
{
	return __lz4_decompress_safe_partial(source, dest, compressedSize,
					     targetOutputSize,
					     maxDecompressedSize);
}

int LZ4_decompress_fast(const char *source, char *dest, int originalSize)
{
	return __lz4_decompress_fast(source, dest, originalSize);
}

int LZ4_setStreamDecode(LZ4_streamDecode_t *LZ4_streamDecode,
			const char *dictionary, int dictSize)
{
	return __lz4_setStreamDecode(LZ4_streamDecode, dictionary, dictSize);
}

int LZ4_decompress_safe_continue(LZ4_streamDecode_t *LZ4_streamDecode,
				 const char *source, char *dest,
				 int compressedSize, int maxDecompressedSize)
{
	return __lz4_decompress_safe_continue(LZ4_streamDecode, source, dest,
					      compressedSize,
					      maxDecompressedSize);
}

int LZ4_decompress_fast_continue(LZ4_streamDecode_t *LZ4_streamDecode,
				 const char *source, char *dest,
				 int originalSize)
{
	return __lz4_decompress_fast_continue(LZ4_streamDecode, source, dest,
					      originalSize);
}

int LZ4_decompress_safe_usingDict(const char *source, char *dest,
				  int compressedSize, int maxDecompressedSize,
				  const char *dictStart, int dictSize)
{
	return __lz4_decompress_safe_usingDict(source, dest, compressedSize,
					       maxDecompressedSize, dictStart,
					       dictSize);
}

int LZ4_decompress_fast_usingDict(const char *source, char *dest,
				  int originalSize, const char *dictStart,
				  int dictSize)
{
	return __lz4_decompress_fast_usingDict(source, dest, originalSize,
					       dictStart, dictSize);
}

#ifndef STATIC
EXPORT_SYMBOL(LZ4_decompress_safe);
EXPORT_SYMBOL(LZ4_decompress_safe_partial);
EXPORT_SYMBOL(LZ4_decompress_fast);
EXPORT_SYMBOL(LZ4_setStreamDecode);
EXPORT_SYMBOL(LZ4_decompress_safe_continue);
EXPORT_SYMBOL(LZ4_decompress_fast_continue);
EXPORT_SYMBOL(LZ4_decompress_safe_usingDict);
EXPORT_SYMBOL(LZ4_decompress_fast_usingDict);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 decompressor");
#endif
