/*
 * ChaCha and XChaCha stream ciphers, including ChaCha20 (RFC7539)
 *
 * Copyright (C) 2015 Martin Willi
 * Copyright (C) 2018 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/unaligned.h>
#include <crypto/algapi.h>
#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <crypto/chacha.h>

static void chacha_docrypt(u32 *state, u8 *dst, const u8 *src,
			   unsigned int bytes, int nrounds)
{
	/* aligned to potentially speed up crypto_xor() */
	u8 stream[CHACHA_BLOCK_SIZE] __aligned(sizeof(long));

	while (bytes >= CHACHA_BLOCK_SIZE) {
		chacha_block(state, stream, nrounds);
		crypto_xor_cpy(dst, src, stream, CHACHA_BLOCK_SIZE);
		bytes -= CHACHA_BLOCK_SIZE;
		dst += CHACHA_BLOCK_SIZE;
		src += CHACHA_BLOCK_SIZE;
	}
	if (bytes) {
		chacha_block(state, stream, nrounds);
		crypto_xor_cpy(dst, src, stream, bytes);
	}
}

static int chacha_stream_xor(struct blkcipher_desc *desc, struct scatterlist *dst,
			     struct scatterlist *src, unsigned int nbytes,
			     struct chacha_ctx *ctx, u8 *iv)
{
	struct blkcipher_walk walk;
	u32 state[16];
	int err;

	blkcipher_walk_init(&walk, dst, src, nbytes);
	err = blkcipher_walk_virt_block(desc, &walk, CHACHA_BLOCK_SIZE);

	crypto_chacha_init(state, ctx, iv);

	while (walk.nbytes >= CHACHA_BLOCK_SIZE) {
		chacha_docrypt(state, walk.dst.virt.addr, walk.src.virt.addr,
			       rounddown(walk.nbytes, CHACHA_BLOCK_SIZE),
			       ctx->nrounds);
		err = blkcipher_walk_done(desc, &walk,
					  walk.nbytes % CHACHA_BLOCK_SIZE);
	}

	if (walk.nbytes) {
		chacha_docrypt(state, walk.dst.virt.addr, walk.src.virt.addr,
			       walk.nbytes, ctx->nrounds);
		err = blkcipher_walk_done(desc, &walk, 0);
	}

	return err;
}

void crypto_chacha_init(u32 *state, struct chacha_ctx *ctx, u8 *iv)
{
	state[0]  = 0x61707865; /* "expa" */
	state[1]  = 0x3320646e; /* "nd 3" */
	state[2]  = 0x79622d32; /* "2-by" */
	state[3]  = 0x6b206574; /* "te k" */
	state[4]  = ctx->key[0];
	state[5]  = ctx->key[1];
	state[6]  = ctx->key[2];
	state[7]  = ctx->key[3];
	state[8]  = ctx->key[4];
	state[9]  = ctx->key[5];
	state[10] = ctx->key[6];
	state[11] = ctx->key[7];
	state[12] = get_unaligned_le32(iv +  0);
	state[13] = get_unaligned_le32(iv +  4);
	state[14] = get_unaligned_le32(iv +  8);
	state[15] = get_unaligned_le32(iv + 12);
}
EXPORT_SYMBOL_GPL(crypto_chacha_init);

static int chacha_setkey(struct crypto_tfm *tfm, const u8 *key,
			 unsigned int keysize, int nrounds)
{
	struct chacha_ctx *ctx = crypto_tfm_ctx(tfm);
	int i;

	if (keysize != CHACHA_KEY_SIZE)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ctx->key); i++)
		ctx->key[i] = get_unaligned_le32(key + i * sizeof(u32));

	ctx->nrounds = nrounds;
	return 0;
}

int crypto_chacha20_setkey(struct crypto_tfm *tfm, const u8 *key,
			   unsigned int keysize)
{
	return chacha_setkey(tfm, key, keysize, 20);
}
EXPORT_SYMBOL_GPL(crypto_chacha20_setkey);

int crypto_chacha12_setkey(struct crypto_tfm *tfm, const u8 *key,
			   unsigned int keysize)
{
	return chacha_setkey(tfm, key, keysize, 12);
}
EXPORT_SYMBOL_GPL(crypto_chacha12_setkey);

int crypto_chacha_crypt(struct blkcipher_desc *desc, struct scatterlist *dst,
			struct scatterlist *src, unsigned int nbytes)
{
	struct chacha_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	u8 *iv = desc->info;

	return chacha_stream_xor(desc, dst, src, nbytes, ctx, iv);
}
EXPORT_SYMBOL_GPL(crypto_chacha_crypt);

int crypto_xchacha_crypt(struct blkcipher_desc *desc, struct scatterlist *dst,
			 struct scatterlist *src, unsigned int nbytes)
{
	struct chacha_ctx *ctx = crypto_blkcipher_ctx(desc->tfm);
	u8 *iv = desc->info;
	struct chacha_ctx subctx;
	u32 state[16];
	u8 real_iv[16];

	/* Compute the subkey given the original key and first 128 nonce bits */
	crypto_chacha_init(state, ctx, iv);
	hchacha_block(state, subctx.key, ctx->nrounds);
	subctx.nrounds = ctx->nrounds;

	/* Build the real IV */
	memcpy(&real_iv[0], iv + 24, 8); /* stream position */
	memcpy(&real_iv[8], iv + 16, 8); /* remaining 64 nonce bits */

	/* Generate the stream and XOR it with the data */
	return chacha_stream_xor(desc, dst, src, nbytes, &subctx, real_iv);
}
EXPORT_SYMBOL_GPL(crypto_xchacha_crypt);

static struct crypto_alg algs[] = {
	{
		.cra_name		= "chacha20",
		.cra_driver_name	= "chacha20-generic",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_BLKCIPHER,
		.cra_blocksize		= 1,
		.cra_type		= &crypto_blkcipher_type,
		.cra_ctxsize		= sizeof(struct chacha_ctx),
		.cra_alignmask		= sizeof(u32) - 1,
		.cra_module		= THIS_MODULE,
		.cra_u			= {
			.blkcipher = {
				.min_keysize	= CHACHA_KEY_SIZE,
				.max_keysize	= CHACHA_KEY_SIZE,
				.ivsize		= CHACHA_IV_SIZE,
				.geniv		= "seqiv",
				.setkey		= crypto_chacha20_setkey,
				.encrypt	= crypto_chacha_crypt,
				.decrypt	= crypto_chacha_crypt,
			},
		},
	}, {
		.cra_name		= "xchacha20",
		.cra_driver_name	= "xchacha20-generic",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_BLKCIPHER,
		.cra_blocksize		= 1,
		.cra_type		= &crypto_blkcipher_type,
		.cra_ctxsize		= sizeof(struct chacha_ctx),
		.cra_alignmask		= sizeof(u32) - 1,
		.cra_module		= THIS_MODULE,
		.cra_u			= {
			.blkcipher = {
				.min_keysize	= CHACHA_KEY_SIZE,
				.max_keysize	= CHACHA_KEY_SIZE,
				.ivsize		= XCHACHA_IV_SIZE,
				.geniv		= "seqiv",
				.setkey		= crypto_chacha20_setkey,
				.encrypt	= crypto_xchacha_crypt,
				.decrypt	= crypto_xchacha_crypt,
			},
		},
	}, {
		.cra_name		= "xchacha12",
		.cra_driver_name	= "xchacha12-generic",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_BLKCIPHER,
		.cra_blocksize		= 1,
		.cra_type		= &crypto_blkcipher_type,
		.cra_ctxsize		= sizeof(struct chacha_ctx),
		.cra_alignmask		= sizeof(u32) - 1,
		.cra_module		= THIS_MODULE,
		.cra_u			= {
			.blkcipher = {
				.min_keysize	= CHACHA_KEY_SIZE,
				.max_keysize	= CHACHA_KEY_SIZE,
				.ivsize		= XCHACHA_IV_SIZE,
				.geniv		= "seqiv",
				.setkey		= crypto_chacha12_setkey,
				.encrypt	= crypto_xchacha_crypt,
				.decrypt	= crypto_xchacha_crypt,
			},
		},
	},
};

static int __init chacha_generic_mod_init(void)
{
	return crypto_register_algs(algs, ARRAY_SIZE(algs));
}

static void __exit chacha_generic_mod_fini(void)
{
	crypto_unregister_algs(algs, ARRAY_SIZE(algs));
}

module_init(chacha_generic_mod_init);
module_exit(chacha_generic_mod_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martin Willi <martin@strongswan.org>");
MODULE_DESCRIPTION("ChaCha and XChaCha stream ciphers (generic)");
MODULE_ALIAS_CRYPTO("chacha20");
MODULE_ALIAS_CRYPTO("chacha20-generic");
MODULE_ALIAS_CRYPTO("xchacha20");
MODULE_ALIAS_CRYPTO("xchacha20-generic");
MODULE_ALIAS_CRYPTO("xchacha12");
MODULE_ALIAS_CRYPTO("xchacha12-generic");
