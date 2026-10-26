/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/crypto.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/lzo.h>
#include <linux/lz4.h>
#if IS_ENABLED(CONFIG_ZSTD_COMPRESS)
#include <linux/zstd.h>
#endif
#if IS_ENABLED(CONFIG_ZLIB_DEFLATE)
#include <linux/zlib.h>
#endif

#include "zcomp.h"

static const char * const backends[] = {
	"lzo",
	"lzo-rle",
#if IS_ENABLED(CONFIG_CRYPTO_LZ4)
	"lz4",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4HC)
	"lz4hc",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_842)
	"842",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_ZSTD)
	"zstd",
#endif
	NULL
};

struct zcomp_ops {
	const char *name;
	/* Decompression context is stateless and can be shared. */
	bool shared_decomp;
	void *(*create_cctx)(const char *name);
	void *(*create_dctx)(const char *name);
	void (*destroy_ctx)(void *ctx);
	int (*compress)(void *ctx, const void *src, unsigned int slen,
			void *dst, unsigned int *dlen);
	int (*decompress)(void *ctx, const void *src, unsigned int slen,
			  void *dst, unsigned int *dlen);
};

static void kvfree_ctx(void *ctx)
{
	kvfree(ctx);
}

#if IS_ENABLED(CONFIG_LZ4_COMPRESS)
static unsigned int zram_lz4_acceleration = 1;
module_param(zram_lz4_acceleration, uint, 0644);
MODULE_PARM_DESC(zram_lz4_acceleration,
		 "zram LZ4 acceleration factor (1 = default)");

static void *lz4_create_cctx(const char *name)
{
	return kvmalloc(LZ4_MEM_COMPRESS, GFP_KERNEL);
}

static int lz4_compress(void *ctx, const void *src, unsigned int slen,
			void *dst, unsigned int *dlen)
{
	int out_len;
	unsigned int acceleration = zram_lz4_acceleration;

	if (!acceleration)
		acceleration = 1;

	out_len = LZ4_compress_fast(src, dst, slen, *dlen,
				    acceleration, ctx);
	if (!out_len)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4_decompress(void *ctx, const void *src, unsigned int slen,
			  void *dst, unsigned int *dlen)
{
	int out_len;

	out_len = LZ4_decompress_safe(src, dst, slen, *dlen);
	if (out_len < 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static const struct zcomp_ops lz4_ops = {
	.name		= "lz4",
	.shared_decomp	= true,
	.create_cctx	= lz4_create_cctx,
	.destroy_ctx	= kvfree_ctx,
	.compress	= lz4_compress,
	.decompress	= lz4_decompress,
};
#endif

#if IS_ENABLED(CONFIG_LZ4HC_COMPRESS)
static int zram_lz4hc_level = LZ4HC_DEFAULT_CLEVEL;
module_param(zram_lz4hc_level, int, 0644);
MODULE_PARM_DESC(zram_lz4hc_level, "zram LZ4HC level (3..9)");

static void *lz4hc_create_cctx(const char *name)
{
	return kvmalloc(LZ4HC_MEM_COMPRESS, GFP_KERNEL);
}

static int lz4hc_compress(void *ctx, const void *src, unsigned int slen,
			  void *dst, unsigned int *dlen)
{
	int level = zram_lz4hc_level;
	int out_len;

	if (level < LZ4HC_MIN_CLEVEL || level >= LZ4HC_CLAMP_CLEVEL)
		level = LZ4HC_DEFAULT_CLEVEL;

	out_len = LZ4_compress_HC(src, dst, slen, *dlen, level, ctx);
	if (!out_len)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4hc_decompress(void *ctx, const void *src, unsigned int slen,
			    void *dst, unsigned int *dlen)
{
	int out_len;

	out_len = LZ4_decompress_safe(src, dst, slen, *dlen);
	if (out_len < 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static const struct zcomp_ops lz4hc_ops = {
	.name		= "lz4hc",
	.shared_decomp	= true,
	.create_cctx	= lz4hc_create_cctx,
	.destroy_ctx	= kvfree_ctx,
	.compress	= lz4hc_compress,
	.decompress	= lz4hc_decompress,
};
#endif

#if IS_ENABLED(CONFIG_LZO_COMPRESS)
static void *lzo_create_cctx(const char *name)
{
	return kvmalloc(LZO1X_MEM_COMPRESS, GFP_KERNEL);
}

static int lzo_compress(void *ctx, const void *src, unsigned int slen,
			void *dst, unsigned int *dlen)
{
	size_t dst_len = *dlen;
	int err;

	err = lzo1x_1_compress_safe(src, slen, dst, &dst_len, ctx);
	if (err != LZO_E_OK)
		return -EINVAL;

	*dlen = dst_len;
	return 0;
}

static int lzo_decompress(void *ctx, const void *src, unsigned int slen,
			  void *dst, unsigned int *dlen)
{
	size_t dst_len = *dlen;
	int err;

	err = lzo1x_decompress_safe(src, slen, dst, &dst_len);
	if (err != LZO_E_OK)
		return -EINVAL;

	*dlen = dst_len;
	return 0;
}

static const struct zcomp_ops lzo_ops = {
	.name		= "lzo",
	.shared_decomp	= true,
	.create_cctx	= lzo_create_cctx,
	.destroy_ctx	= kvfree_ctx,
	.compress	= lzo_compress,
	.decompress	= lzo_decompress,
};

static int lzorle_compress(void *ctx, const void *src, unsigned int slen,
			   void *dst, unsigned int *dlen)
{
	size_t dst_len = *dlen;
	int err;

	err = lzorle1x_1_compress_safe(src, slen, dst, &dst_len, ctx);
	if (err != LZO_E_OK)
		return -EINVAL;

	*dlen = dst_len;
	return 0;
}

static const struct zcomp_ops lzorle_ops = {
	.name		= "lzo-rle",
	.shared_decomp	= true,
	.create_cctx	= lzo_create_cctx,
	.destroy_ctx	= kvfree_ctx,
	.compress	= lzorle_compress,
	.decompress	= lzo_decompress,
};
#endif

#if IS_ENABLED(CONFIG_ZSTD_COMPRESS)
static int zram_zstd_level = 3;
module_param(zram_zstd_level, int, 0644);
MODULE_PARM_DESC(zram_zstd_level, "zram zstd level (1..22)");

struct zstd_ctx {
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
	void *cwksp;
	void *dwksp;
};

static ZSTD_parameters zram_zstd_params(void)
{
	int level = zram_zstd_level;

	if (level < 1 || level > ZSTD_maxCLevel())
		level = 3;
	return ZSTD_getParams(level, 0, 0);
}

static void *zstd_create_cctx(const char *name)
{
	struct zstd_ctx *zctx;
	ZSTD_parameters params = zram_zstd_params();
	size_t wksp_size = ZSTD_CCtxWorkspaceBound(params.cParams);

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return NULL;

	zctx->cwksp = kvzalloc(wksp_size, GFP_KERNEL);
	if (!zctx->cwksp)
		goto error;

	zctx->cctx = ZSTD_initCCtx(zctx->cwksp, wksp_size);
	if (!zctx->cctx)
		goto error;

	return zctx;

error:
	kvfree(zctx->cwksp);
	kfree(zctx);
	return NULL;
}

static void *zstd_create_dctx(const char *name)
{
	struct zstd_ctx *zctx;
	size_t wksp_size = ZSTD_DCtxWorkspaceBound();

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return NULL;

	zctx->dwksp = kvzalloc(wksp_size, GFP_KERNEL);
	if (!zctx->dwksp)
		goto error;

	zctx->dctx = ZSTD_initDCtx(zctx->dwksp, wksp_size);
	if (!zctx->dctx)
		goto error;

	return zctx;

error:
	kvfree(zctx->dwksp);
	kfree(zctx);
	return NULL;
}

static void zstd_destroy_ctx(void *ctx)
{
	struct zstd_ctx *zctx = ctx;

	kvfree(zctx->cwksp);
	kvfree(zctx->dwksp);
	kfree(zctx);
}

static int zstd_compress(void *ctx, const void *src, unsigned int slen,
			 void *dst, unsigned int *dlen)
{
	struct zstd_ctx *zctx = ctx;
	ZSTD_parameters params = zram_zstd_params();
	size_t out_len;

	out_len = ZSTD_compressCCtx(zctx->cctx, dst, *dlen, src, slen,
				    params);
	if (ZSTD_isError(out_len))
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int zstd_decompress(void *ctx, const void *src, unsigned int slen,
			   void *dst, unsigned int *dlen)
{
	struct zstd_ctx *zctx = ctx;
	size_t out_len;

	out_len = ZSTD_decompressDCtx(zctx->dctx, dst, *dlen, src, slen);
	if (ZSTD_isError(out_len))
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static const struct zcomp_ops zstd_ops = {
	.name		= "zstd",
	.shared_decomp	= true,
	.create_cctx	= zstd_create_cctx,
	.create_dctx	= zstd_create_dctx,
	.destroy_ctx	= zstd_destroy_ctx,
	.compress	= zstd_compress,
	.decompress	= zstd_decompress,
};
#endif

#if IS_ENABLED(CONFIG_ZLIB_DEFLATE)
static int zram_deflate_level = Z_DEFAULT_COMPRESSION;
module_param(zram_deflate_level, int, 0644);
MODULE_PARM_DESC(zram_deflate_level, "zram deflate level (-1..9)");

struct deflate_ctx {
	struct z_stream_s stream;
	void *workspace;
	bool decompress;
};

static void *deflate_create_cctx(const char *name)
{
	struct deflate_ctx *ctx;
	int level = zram_deflate_level;

	if (level < Z_DEFAULT_COMPRESSION || level > 9)
		level = Z_DEFAULT_COMPRESSION;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->workspace = kvzalloc(zlib_deflate_workspacesize(MAX_WBITS,
							    MAX_MEM_LEVEL),
				  GFP_KERNEL);
	if (!ctx->workspace)
		goto error;

	if (zlib_deflateInit2(&ctx->stream, level, Z_DEFLATED, -11,
			      MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
		goto error;

	return ctx;

error:
	kvfree(ctx->workspace);
	kfree(ctx);
	return NULL;
}

static void *deflate_create_dctx(const char *name)
{
	struct deflate_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->workspace = kvzalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	if (!ctx->workspace)
		goto error;

	if (zlib_inflateInit2(&ctx->stream, -11) != Z_OK)
		goto error;

	ctx->decompress = true;
	return ctx;

error:
	kvfree(ctx->workspace);
	kfree(ctx);
	return NULL;
}

static void deflate_destroy_ctx(void *data)
{
	struct deflate_ctx *ctx = data;

	if (ctx->decompress)
		zlib_inflateEnd(&ctx->stream);
	else
		zlib_deflateEnd(&ctx->stream);
	kvfree(ctx->workspace);
	kfree(ctx);
}

static int deflate_compress(void *data, const void *src, unsigned int slen,
			    void *dst, unsigned int *dlen)
{
	struct deflate_ctx *ctx = data;
	struct z_stream_s *stream = &ctx->stream;
	int ret;

	ret = zlib_deflateReset(stream);
	if (ret != Z_OK)
		return -EINVAL;

	stream->next_in = (u8 *)src;
	stream->avail_in = slen;
	stream->next_out = dst;
	stream->avail_out = *dlen;

	ret = zlib_deflate(stream, Z_FINISH);
	if (ret != Z_STREAM_END)
		return -EINVAL;

	*dlen = stream->total_out;
	return 0;
}

static int deflate_decompress(void *data, const void *src, unsigned int slen,
			      void *dst, unsigned int *dlen)
{
	struct deflate_ctx *ctx = data;
	struct z_stream_s *stream = &ctx->stream;
	int ret;

	ret = zlib_inflateReset(stream);
	if (ret != Z_OK)
		return -EINVAL;

	stream->next_in = (u8 *)src;
	stream->avail_in = slen;
	stream->next_out = dst;
	stream->avail_out = *dlen;

	ret = zlib_inflate(stream, Z_FINISH);
	if (ret != Z_STREAM_END)
		return -EINVAL;

	*dlen = stream->total_out;
	return 0;
}

static const struct zcomp_ops deflate_ops = {
	.name		= "deflate",
	.shared_decomp	= true,
	.create_cctx	= deflate_create_cctx,
	.create_dctx	= deflate_create_dctx,
	.destroy_ctx	= deflate_destroy_ctx,
	.compress	= deflate_compress,
	.decompress	= deflate_decompress,
};
#endif

/* Fallback for backends which do not have a direct implementation yet. */
static void *crypto_create_ctx(const char *name)
{
	return crypto_alloc_comp(name, 0, 0);
}

static void crypto_destroy_ctx(void *ctx)
{
	if (ctx)
		crypto_free_comp(ctx);
}

static int crypto_compress(void *ctx, const void *src, unsigned int slen,
			   void *dst, unsigned int *dlen)
{
	return crypto_comp_compress(ctx, src, slen, dst, dlen);
}

static int crypto_decompress(void *ctx, const void *src, unsigned int slen,
			     void *dst, unsigned int *dlen)
{
	return crypto_comp_decompress(ctx, src, slen, dst, dlen);
}

static const struct zcomp_ops crypto_ops = {
	.name		= NULL,
	.shared_decomp	= false,
	.create_cctx	= crypto_create_ctx,
	.create_dctx	= crypto_create_ctx,
	.destroy_ctx	= crypto_destroy_ctx,
	.compress	= crypto_compress,
	.decompress	= crypto_decompress,
};

static const struct zcomp_ops *zcomp_lookup_ops(const char *name)
{
#if IS_ENABLED(CONFIG_LZO_COMPRESS)
	if (!strcmp(name, "lzo"))
		return &lzo_ops;
	if (!strcmp(name, "lzo-rle"))
		return &lzorle_ops;
#endif
#if IS_ENABLED(CONFIG_LZ4_COMPRESS)
	if (!strcmp(name, "lz4"))
		return &lz4_ops;
#endif
#if IS_ENABLED(CONFIG_LZ4HC_COMPRESS)
	if (!strcmp(name, "lz4hc"))
		return &lz4hc_ops;
#endif
#if IS_ENABLED(CONFIG_ZSTD_COMPRESS)
	if (!strcmp(name, "zstd"))
		return &zstd_ops;
#endif
#if IS_ENABLED(CONFIG_ZLIB_DEFLATE)
	if (!strcmp(name, "deflate"))
		return &deflate_ops;
#endif
	return NULL;
}

static void zcomp_strm_free(struct zcomp_strm *zstrm)
{
	if (zstrm->ops && zstrm->ops->destroy_ctx && zstrm->ctx)
		zstrm->ops->destroy_ctx(zstrm->ctx);
	if (zstrm->buffer)
		free_pages((unsigned long)zstrm->buffer, 1);
	kfree(zstrm);
}

/*
 * Allocate one compression or decompression stream.  Compression streams
 * always need a backend context; stateless decompression streams do not.
 */
static struct zcomp_strm *zcomp_strm_alloc(struct zcomp *comp, bool decompress)
{
	struct zcomp_strm *zstrm;

	zstrm = kmalloc(sizeof(*zstrm), GFP_KERNEL);
	if (!zstrm)
		return NULL;

	zstrm->ops = comp->ops;
	zstrm->ctx = NULL;
	zstrm->buffer = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (!zstrm->buffer)
		goto error;

	if (decompress) {
		if (comp->ops->create_dctx) {
			zstrm->ctx = comp->ops->create_dctx(comp->name);
			if (IS_ERR_OR_NULL(zstrm->ctx)) {
				zstrm->ctx = NULL;
				goto error;
			}
		}
	} else {
		zstrm->ctx = comp->ops->create_cctx(comp->name);
		if (IS_ERR_OR_NULL(zstrm->ctx)) {
			zstrm->ctx = NULL;
			goto error;
		}
	}

	spin_lock_init(&zstrm->zcomp_lock);
	return zstrm;

error:
	zcomp_strm_free(zstrm);
	return NULL;
}

bool zcomp_available_algorithm(const char *comp)
{
	int i;

	if (zcomp_lookup_ops(comp))
		return true;

	i = __sysfs_match_string(backends, -1, comp);
	if (i >= 0)
		return true;

	/*
	 * Crypto does not ignore a trailing new line symbol,
	 * so make sure you don't supply a string containing
	 * one.
	 * This also means that we permit zcomp initialisation
	 * with any compressing algorithm known to crypto api.
	 */
	return crypto_has_comp(comp, 0, 0) == 1;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	bool known_algorithm = false;
	ssize_t sz = 0;
	int i = 0;

	for (; backends[i]; i++) {
		if (!strcmp(comp, backends[i])) {
			known_algorithm = true;
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]);
		}
	}

	/*
	 * Out-of-tree module known to crypto api or a missing
	 * entry in `backends'.
	 */
	if (!known_algorithm && crypto_has_comp(comp, 0, 0) == 1)
		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
				"[%s] ", comp);

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	zstrm = *get_local_ptr(comp->stream);
	spin_lock(&zstrm->zcomp_lock);
	return zstrm;
}

void zcomp_stream_put(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	zstrm = *this_cpu_ptr(comp->stream);
	spin_unlock(&zstrm->zcomp_lock);
	put_local_ptr(zstrm);
}

struct zcomp_strm *zcomp_dstream_get(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	if (comp->ops->shared_decomp) {
		if (comp->ops->create_dctx)
			spin_lock(&comp->dstrm_lock);
		return comp->dstrm;
	}

	zstrm = *get_local_ptr(comp->dstream);
	spin_lock(&zstrm->zcomp_lock);
	return zstrm;
}

void zcomp_dstream_put(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	if (comp->ops->shared_decomp) {
		if (comp->ops->create_dctx)
			spin_unlock(&comp->dstrm_lock);
		return;
	}

	zstrm = *this_cpu_ptr(comp->dstream);
	spin_unlock(&zstrm->zcomp_lock);
	put_local_ptr(zstrm);
}

int zcomp_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int *dst_len)
{
	/*
	 * zstrm->buffer is 2 pages: one page for normal compressed data and
	 * one extra page for backends which can produce a slightly larger
	 * output (e.g. 842 padding).
	 */
	*dst_len = PAGE_SIZE * 2;
	return zstrm->ops->compress(zstrm->ctx, src, PAGE_SIZE,
				    zstrm->buffer, dst_len);
}

int zcomp_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst)
{
	unsigned int dst_len = PAGE_SIZE;

	return zstrm->ops->decompress(zstrm->ctx, src, src_len,
				      dst, &dst_len);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm, *dstrm = NULL;

	if (WARN_ON(*per_cpu_ptr(comp->stream, cpu)))
		return 0;
	if (!comp->ops->shared_decomp &&
	    WARN_ON(*per_cpu_ptr(comp->dstream, cpu)))
		return 0;

	zstrm = zcomp_strm_alloc(comp, false);
	if (!zstrm) {
		pr_err("Can't allocate a compression stream\n");
		return -ENOMEM;
	}

	if (!comp->ops->shared_decomp) {
		dstrm = zcomp_strm_alloc(comp, true);
		if (!dstrm) {
			pr_err("Can't allocate a decompression stream\n");
			zcomp_strm_free(zstrm);
			return -ENOMEM;
		}
	}

	*per_cpu_ptr(comp->stream, cpu) = zstrm;
	if (!comp->ops->shared_decomp)
		*per_cpu_ptr(comp->dstream, cpu) = dstrm;
	return 0;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm, *dstrm;

	zstrm = *per_cpu_ptr(comp->stream, cpu);
	if (zstrm)
		zcomp_strm_free(zstrm);
	*per_cpu_ptr(comp->stream, cpu) = NULL;

	if (!comp->ops->shared_decomp) {
		dstrm = *per_cpu_ptr(comp->dstream, cpu);
		if (dstrm)
			zcomp_strm_free(dstrm);
		*per_cpu_ptr(comp->dstream, cpu) = NULL;
	}
	return 0;
}

static int zcomp_init(struct zcomp *comp)
{
	int ret;

	comp->stream = alloc_percpu(struct zcomp_strm *);
	if (!comp->stream)
		return -ENOMEM;

	if (comp->ops->shared_decomp) {
		comp->dstrm = zcomp_strm_alloc(comp, true);
		if (!comp->dstrm) {
			free_percpu(comp->stream);
			comp->stream = NULL;
			return -ENOMEM;
		}
		spin_lock_init(&comp->dstrm_lock);
	} else {
		comp->dstream = alloc_percpu(struct zcomp_strm *);
		if (!comp->dstream) {
			free_percpu(comp->stream);
			comp->stream = NULL;
			return -ENOMEM;
		}
	}

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;
	return 0;

cleanup:
	if (comp->dstrm) {
		zcomp_strm_free(comp->dstrm);
		comp->dstrm = NULL;
	}
	if (comp->dstream) {
		free_percpu(comp->dstream);
		comp->dstream = NULL;
	}
	free_percpu(comp->stream);
	comp->stream = NULL;
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);

	if (comp->dstrm)
		zcomp_strm_free(comp->dstrm);
	if (comp->dstream)
		free_percpu(comp->dstream);
	free_percpu(comp->stream);
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by zcomp_init().
 */
struct zcomp *zcomp_create(const char *compress)
{
	const struct zcomp_ops *ops;
	struct zcomp *comp;
	int error;

	if (!zcomp_available_algorithm(compress))
		return ERR_PTR(-EINVAL);

	ops = zcomp_lookup_ops(compress);
	if (!ops)
		ops = &crypto_ops;

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->name = compress;
	comp->ops = ops;
	error = zcomp_init(comp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
