/*
 * polarssl_shim.h — Thin aliases from mbedtls_* to polarssl_* naming.
 *
 * The PS3 portlib ships polarssl (pre-mbedtls rename).  Apollo's algorithm
 * code uses mbedtls names.  This shim lets algorithm comments and logic stay
 * readable without pulling in apollo's tree.
 *
 * Rules:
 *  - Include this BEFORE any algorithm code that uses mbedtls_* names.
 *  - Do NOT include both this header and a real mbedtls header in the same TU.
 *  - Only the functions actually used by pfd.c are shimmed here.
 */

#ifndef SAVESYNC_POLARSSL_SHIM_H
#define SAVESYNC_POLARSSL_SHIM_H

#include <polarssl/aes.h>
#include <polarssl/md.h>

/* -----------------------------------------------------------------------
 * AES
 * polarssl: aes_context (no init/free needed — plain struct)
 * mbedtls:  mbedtls_aes_context, mbedtls_aes_init, mbedtls_aes_free
 * --------------------------------------------------------------------- */
typedef aes_context mbedtls_aes_context;

/* polarssl has no init/free for aes_context — use memset(0) instead.
 * The macros silently drop these calls.  Callers must zero the struct
 * themselves or rely on stack zero-init. */
#define mbedtls_aes_init(ctx)   memset((ctx), 0, sizeof(*(ctx)))
#define mbedtls_aes_free(ctx)   ((void)(ctx))

#define mbedtls_aes_setkey_enc  aes_setkey_enc
#define mbedtls_aes_setkey_dec  aes_setkey_dec
#define mbedtls_aes_crypt_ecb   aes_crypt_ecb
/* CBC: mbedtls allows input==output (in-place).  polarssl's signature also
 * declares separate in/out, but in practice shares them when aliased.
 * Our usage in pfd.c always passes the same buffer for in+out. */
#define mbedtls_aes_crypt_cbc   aes_crypt_cbc

#define MBEDTLS_AES_ENCRYPT     AES_ENCRYPT
#define MBEDTLS_AES_DECRYPT     AES_DECRYPT

/* -----------------------------------------------------------------------
 * MD / HMAC
 * polarssl: md_context_t, md_init_ctx(ctx, info), md_free_ctx(ctx)
 * mbedtls:  mbedtls_md_context_t, mbedtls_md_init(ctx),
 *           mbedtls_md_setup(ctx, info, hmac), mbedtls_md_free(ctx)
 *
 * The setup difference is the biggest divergence: mbedtls splits init and
 * setup; polarssl combines them in md_init_ctx.  We handle this by making
 * mbedtls_md_init a no-op and mbedtls_md_setup call md_init_ctx.
 * --------------------------------------------------------------------- */
typedef md_context_t mbedtls_md_context_t;
typedef md_type_t    mbedtls_md_type_t;

#define mbedtls_md_init(ctx)                    memset((ctx), 0, sizeof(*(ctx)))
#define mbedtls_md_free(ctx)                    md_free_ctx(ctx)
/* mbedtls_md_setup(ctx, info, use_hmac) — the 'use_hmac' arg is ignored;
 * polarssl's md_init_ctx always supports HMAC. */
#define mbedtls_md_setup(ctx, info, use_hmac)   md_init_ctx((ctx), (info))

#define mbedtls_md_info_from_type   md_info_from_type
#define MBEDTLS_MD_SHA1             POLARSSL_MD_SHA1

#define mbedtls_md_hmac_starts      md_hmac_starts
#define mbedtls_md_hmac_update      md_hmac_update
#define mbedtls_md_hmac_finish      md_hmac_finish
#define mbedtls_md_hmac             md_hmac

#endif /* SAVESYNC_POLARSSL_SHIM_H */
