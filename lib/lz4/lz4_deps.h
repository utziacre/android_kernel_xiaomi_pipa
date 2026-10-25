/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * Build environment for the vendored upstream LZ4 sources.
 *
 * The four files under upstream/ are copied verbatim; do not edit them.  The
 * three entry point files include this header before them.
 */

#ifndef __LZ4_DEPS_H__
#define __LZ4_DEPS_H__

#include <linux/build_bug.h>	/* static_assert */
#include <linux/compiler.h>	/* __maybe_unused */
#include <linux/string.h>
#include <linux/types.h>

/* lz4.c uses "current" as a local; <asm/current.h> breaks the build. */
#undef current

/* Static so an object carries only the codec it forwards; __maybe_unused
 * for the ones it does not, which the pre-boot decompressors compile
 * without lib/lz4's ccflags.
 */
#define LZ4LIB_VISIBILITY static __maybe_unused

/* Route upstream's "static linking only" entry points through
 * LZ4LIB_VISIBILITY too.
 */
#define LZ4_PUBLISH_STATIC_FUNCTIONS

/* Heap mode only: the stack mode puts LZ4HC's ~64K opt[] in a frame.
 * Allocation goes through the failing stubs below; upstream handles
 * NULL by returning 0.
 */
#define LZ4_USER_MEMORY_FUNCTIONS
#define LZ4_HEAPMODE 1
#define LZ4HC_HEAPMODE 1

/* Failing stubs; nothing in the kernel reaches these. */
static void *LZ4_malloc(size_t s) { return NULL; }
static void *LZ4_calloc(size_t n, size_t s) { return NULL; }
static void LZ4_free(void *p) { }

/* LZ4_decompress_fast() and LZ4_resetStream() are still kernel API. */
#define LZ4_DISABLE_DEPRECATE_WARNINGS 1

/* Rename upstream's entry points out of the way; four of them take a
 * trailing wrkmem argument in the kernel.
 */
#define LZ4_compress_fast		__lz4_compress_fast
#define LZ4_compress_default		__lz4_compress_default
#define LZ4_compress_destSize		__lz4_compress_destSize
#define LZ4_resetStream			__lz4_resetStream
#define LZ4_loadDict			__lz4_loadDict
#define LZ4_saveDict			__lz4_saveDict
#define LZ4_compress_fast_continue	__lz4_compress_fast_continue

#define LZ4_decompress_safe		__lz4_decompress_safe
#define LZ4_decompress_safe_partial	__lz4_decompress_safe_partial
#define LZ4_decompress_fast		__lz4_decompress_fast
#define LZ4_setStreamDecode		__lz4_setStreamDecode
#define LZ4_decompress_safe_continue	__lz4_decompress_safe_continue
#define LZ4_decompress_fast_continue	__lz4_decompress_fast_continue
#define LZ4_decompress_safe_usingDict	__lz4_decompress_safe_usingDict
#define LZ4_decompress_fast_usingDict	__lz4_decompress_fast_usingDict

#define LZ4_compress_HC			__lz4_compress_HC
#define LZ4_resetStreamHC		__lz4_resetStreamHC
#define LZ4_loadDictHC			__lz4_loadDictHC
#define LZ4_compress_HC_continue	__lz4_compress_HC_continue
#define LZ4_saveDictHC			__lz4_saveDictHC

/* Upstream declares these bare, so both entry point objects would define
 * them.  Declaring them static first gives them internal linkage (C11
 * 6.2.2p4): this one in lz4.h, the three below in lz4.c.
 */
static __maybe_unused int LZ4_compress_destSize_extState(void *state, const char *src,
		char *dst, int *srcSizePtr, int targetDstSize,
		int acceleration);

#include "upstream/lz4.h"

static __maybe_unused int LZ4_compress_forceExtDict(LZ4_stream_t *LZ4_dict,
		const char *source, char *dest, int srcSize);
static __maybe_unused int LZ4_decompress_safe_forceExtDict(const char *source,
		char *dest,
		int compressedSize, int maxOutputSize,
		const void *dictStart, size_t dictSize);
static __maybe_unused int LZ4_decompress_safe_partial_forceExtDict(const char *source,
		char *dest, int compressedSize, int targetOutputSize,
		int dstCapacity, const void *dictStart, size_t dictSize);

#endif /* __LZ4_DEPS_H__ */
