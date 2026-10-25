// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
/*
 * Copyright (C) 2011 - 2016, Yann Collet.
 * Copyright (C) 2016, Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * LZ4 HC compressor -- kernel entry points
 *
 * The compressor is upstream's, in the verbatim lz4hc.c included below.
 * LZ4_compress_HC() takes the kernel's trailing wrkmem argument and forwards
 * to LZ4_compress_HC_extStateHC(); the level-taking entry points clamp the
 * level below LZ4HC_CLEVEL_OPT_MIN (see lz4hc_clamp_level()).
 */

#include "lz4_deps.h"

/* lz4hc.c includes lz4.c with LZ4_COMMONDEFS_ONLY, which stops short of
 * LZ4_compressBound().  Supply it for lz4hc.c's internal use.
 */
static int LZ4_compressBound(int isize)
{
	return LZ4_COMPRESSBOUND(isize);
}

#include "upstream/lz4hc.c"

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
static_assert(sizeof(LZ4_streamHC_t) == LZ4_STREAMHC_MINSIZE);
static_assert(LZ4_STREAMHC_MINSIZE == LZ4HC_MEM_COMPRESS);

/* Levels >= LZ4HC_CLEVEL_OPT_MIN (10) reach the optimal parser, whose ~64K
 * opt[] does not fit a kernel stack, so clamp them to 9.  Clamp rather than
 * reject so existing f2fs and zram settings keep working.  Levels 3..9 are
 * untouched; 1 and 2 are upstream's lz4mid, faster and weaker than the
 * shallow hash chain the fork used for them.
 */
static_assert(LZ4HC_CLAMP_CLEVEL == LZ4HC_CLEVEL_OPT_MIN);

static int lz4hc_clamp_level(int compressionLevel)
{
	if (compressionLevel >= LZ4HC_CLAMP_CLEVEL)
		return LZ4HC_CLAMP_CLEVEL - 1;

	return compressionLevel;
}

int LZ4_compress_HC(const char *src, char *dst, int srcSize, int dstCapacity,
		    int compressionLevel, void *wrkmem)
{
	return LZ4_compress_HC_extStateHC(wrkmem, src, dst, srcSize,
					  dstCapacity,
					  lz4hc_clamp_level(compressionLevel));
}
EXPORT_SYMBOL(LZ4_compress_HC);

void LZ4_resetStreamHC(LZ4_streamHC_t *streamHCPtr, int compressionLevel)
{
	__lz4_resetStreamHC(streamHCPtr, lz4hc_clamp_level(compressionLevel));
}
EXPORT_SYMBOL(LZ4_resetStreamHC);

int LZ4_loadDictHC(LZ4_streamHC_t *streamHCPtr, const char *dictionary,
		   int dictSize)
{
	return __lz4_loadDictHC(streamHCPtr, dictionary, dictSize);
}
EXPORT_SYMBOL(LZ4_loadDictHC);

int LZ4_compress_HC_continue(LZ4_streamHC_t *streamHCPtr, const char *src,
			     char *dst, int srcSize, int maxDstSize)
{
	return __lz4_compress_HC_continue(streamHCPtr, src, dst, srcSize,
					  maxDstSize);
}
EXPORT_SYMBOL(LZ4_compress_HC_continue);

int LZ4_saveDictHC(LZ4_streamHC_t *streamHCPtr, char *safeBuffer,
		   int maxDictSize)
{
	return __lz4_saveDictHC(streamHCPtr, safeBuffer, maxDictSize);
}
EXPORT_SYMBOL(LZ4_saveDictHC);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 HC compressor");
