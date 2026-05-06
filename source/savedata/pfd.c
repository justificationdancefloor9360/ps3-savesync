/*
 * pfd.c — PARAM.PFD signing/parsing for savesync.
 *
 * Algorithm credit: flatz (original pfdtool), apollo-ps3 (open-source
 * reimplementation).  This file ports only the hash/sign operations needed
 * for PS3 <-> RPCS3 conversion; file content encryption is intentionally
 * omitted.
 *
 * Crypto backend: polarssl (via polarssl_shim.h aliasing mbedtls_* names).
 */

#include "pfd.h"
#include "pfd_keys.h"
#include "polarssl_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * PFD binary format constants
 * Source: pfd_internal.h from apollo-ps3 (read-only reference).
 * --------------------------------------------------------------------- */
#define PFD_MAGIC                   0x50464442ULL
#define PFD_VERSION_V3              0x3ULL
#define PFD_VERSION_V4              0x4ULL

#define PFD_ENTRY_NAME_SIZE         65
#define PFD_KEY_SIZE                16
#define PFD_HASH_SIZE               20
#define PFD_ENTRY_KEY_SIZE          64
#define PFD_ENTRY_DATA_SIZE         192    /* file_name(65)+pad(7)+key(64)+... see entry layout */
#define PFD_ENTRY_SIZE              272
#define PFD_FILE_SIZE_ALIGNMENT     16
#define PFD_MAX_FILE_SIZE           32768  /* maximum PARAM.PFD file size */

/* Offsets within the PFD binary */
#define PFD_HEADER_OFFSET           0
#define PFD_HEADER_SIZE             16
#define PFD_HEADER_KEY_OFFSET       (PFD_HEADER_OFFSET + PFD_HEADER_SIZE)
#define PFD_HEADER_KEY_SIZE         16
#define PFD_SIGNATURE_OFFSET        (PFD_HEADER_KEY_OFFSET + PFD_HEADER_KEY_SIZE)
#define PFD_SIGNATURE_SIZE          64
#define PFD_HASH_TABLE_OFFSET       (PFD_SIGNATURE_OFFSET + PFD_SIGNATURE_SIZE)
#define PFD_HASH_TABLE_HEADER_SIZE  24     /* capacity(8) + num_reserved(8) + num_used(8) */
#define PFD_ENTRY_INDEX_SENTINEL    0xFFFFFFFFFFFFFFFFULL

/* Entry hash slot indices (index into entry->file_hashes[4]) */
#define PFD_ENTRY_HASH_FILE         0
#define PFD_ENTRY_HASH_FILE_CID     1
#define PFD_ENTRY_HASH_FILE_DHK_CID2 2
#define PFD_ENTRY_HASH_FILE_AID_UID 3
#define PFD_NUM_FILE_HASHES         4

/* -----------------------------------------------------------------------
 * On-disk structures  (packed — matches PS3 big-endian layout).
 *
 * NOTE: The PS3 stores all multi-byte integers in big-endian order.  On PPU
 * (also big-endian) no byte-swapping is needed.  On x86 (for unit tests /
 * RPCS3) you would need ES64() etc.  savesync runs on PPU only, so we
 * access fields directly.
 * --------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct {
    uint64_t magic;
    uint64_t version;
} pfd_header_t;

/* The 16-byte AES IV stored immediately after the header. */
typedef uint8_t pfd_header_key_t[PFD_HEADER_KEY_SIZE];

/* 64-byte signature block: bottom_hash(20) + top_hash(20) + hash_key(20) + pad(4). */
typedef struct {
    uint8_t bottom_hash[PFD_HASH_SIZE];
    uint8_t top_hash[PFD_HASH_SIZE];
    uint8_t hash_key[PFD_HASH_KEY_SIZE];
    uint8_t _pad[4];
} pfd_signature_t;

/* Hash table header — variable-length array of entry indices follows. */
typedef struct {
    uint64_t capacity;
    uint64_t num_reserved;
    uint64_t num_used;
    /* uint64_t entries[capacity] follows in memory */
} pfd_hash_table_hdr_t;

/* One entry in the per-file table. */
typedef struct {
    uint64_t additional_index;          /* chaining: next entry with same hash slot */
    char     file_name[PFD_ENTRY_NAME_SIZE];
    uint8_t  _pad0[7];
    uint8_t  key[PFD_ENTRY_KEY_SIZE];   /* AES-encrypted per-file key (unused for RPCS3→PS3) */
    uint8_t  file_hashes[PFD_NUM_FILE_HASHES][PFD_HASH_SIZE];
    uint8_t  _pad1[40];
    uint64_t file_size;
} pfd_entry_t;

#pragma pack(pop)

/* Static assert to catch struct size drift at compile time. */
typedef char _pfd_entry_size_check[sizeof(pfd_entry_t) == PFD_ENTRY_SIZE ? 1 : -1];

/* -----------------------------------------------------------------------
 * Internal context — heap-allocated, freed before every public function
 * returns.
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t  *data;              /* raw PFD bytes (PFD_MAX_FILE_SIZE) */
    uint8_t  *tmp;               /* scratch buffer for CBC encrypt/decrypt */
    uint64_t  data_size;

    pfd_header_t          *header;
    pfd_header_key_t      *header_key;
    pfd_signature_t       *signature;
    pfd_hash_table_hdr_t  *hash_table_hdr;
    uint64_t              *hash_table_entries;  /* capacity entries */
    pfd_entry_t           *entry_table;         /* num_reserved entries */
    uint8_t               *entry_sig_table;     /* capacity * PFD_HASH_SIZE */

    uint8_t  real_hash_key[PFD_HASH_KEY_SIZE];
    uint8_t  console_id[16];
    uint8_t  auth_id[PFD_AUTHENTICATION_ID_SIZE];
    uint8_t  user_id[8];
} pfd_ctx_t;

/* -----------------------------------------------------------------------
 * Helpers: path, file I/O
 * --------------------------------------------------------------------- */
#define PFD_PATH_MAX 512

static void build_path(char *out, size_t out_size,
                       const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/')
        snprintf(out, out_size, "%s%s", dir, name);
    else
        snprintf(out, out_size, "%s/%s", dir, name);
}

/* Read up to `max_len` bytes from `path` into pre-zeroed `buf`.
 * Returns number of bytes read, or -1 on error. */
static long read_file_partial(const char *path, uint8_t *buf,
                              size_t max_len, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, max_len, f);
    fclose(f);
    *out_size = n;
    return (long)n;
}

/* Write exactly `len` bytes from `buf` to `path`.
 * Returns 0 on success, -1 on error. */
static int write_file_exact(const char *path,
                            const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

/* Return file size via stat, or 0 on error. */
static uint64_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

/* -----------------------------------------------------------------------
 * Helper: next power of two >= n (minimum 4)
 * --------------------------------------------------------------------- */
static uint64_t next_pow2(uint64_t n) {
    if (n < 4) return 4;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/* -----------------------------------------------------------------------
 * AES-CBC encrypt/decrypt using syscon_manager_key.
 *
 * polarssl's aes_crypt_cbc requires separate in/out buffers.  We use the
 * ctx->tmp scratch buffer so we never alias input==output at the C-API
 * boundary, even though it would likely work in practice.
 * --------------------------------------------------------------------- */
static int pfd_cbc_encrypt(pfd_ctx_t *ctx,
                           const uint8_t iv_src[PFD_KEY_SIZE],
                           uint8_t *data, uint32_t data_size) {
    uint8_t iv[PFD_KEY_SIZE];
    aes_context aes;

    memset(&aes, 0, sizeof(aes));
    memcpy(iv, iv_src, PFD_KEY_SIZE);
    memcpy(ctx->tmp, data, data_size);

    aes_setkey_enc(&aes, k_syscon_manager_key, 128);
    if (aes_crypt_cbc(&aes, AES_ENCRYPT, data_size, iv, ctx->tmp, data) != 0)
        return -1;
    return 0;
}

static int pfd_cbc_decrypt(pfd_ctx_t *ctx,
                           const uint8_t iv_src[PFD_KEY_SIZE],
                           uint8_t *data, uint32_t data_size) {
    uint8_t iv[PFD_KEY_SIZE];
    aes_context aes;

    memset(&aes, 0, sizeof(aes));
    memcpy(iv, iv_src, PFD_KEY_SIZE);
    memcpy(ctx->tmp, data, data_size);

    aes_setkey_dec(&aes, k_syscon_manager_key, 128);
    if (aes_crypt_cbc(&aes, AES_DECRYPT, data_size, iv, ctx->tmp, data) != 0)
        return -1;
    return 0;
}

/* -----------------------------------------------------------------------
 * HMAC-SHA1 over an in-memory buffer.  Returns 0 on success.
 * --------------------------------------------------------------------- */
static int hmac_sha1_buf(const uint8_t *key, size_t key_len,
                         const uint8_t *input, size_t input_len,
                         uint8_t out[PFD_HASH_SIZE]) {
    return md_hmac(md_info_from_type(POLARSSL_MD_SHA1),
                   key, key_len, input, input_len, out);
}

/* -----------------------------------------------------------------------
 * HMAC-SHA1 over a file's contents.  Returns 0 on success.
 * --------------------------------------------------------------------- */
static int hmac_sha1_file(const char *path,
                          const uint8_t *key, size_t key_len,
                          uint8_t out[PFD_HASH_SIZE]) {
    md_context_t ctx;
    uint8_t buf[4096];
    size_t n;
    FILE *f;

    memset(&ctx, 0, sizeof(ctx));
    if (md_init_ctx(&ctx, md_info_from_type(POLARSSL_MD_SHA1)) != 0)
        return -1;

    if (md_hmac_starts(&ctx, key, key_len) != 0) {
        md_free_ctx(&ctx);
        return -1;
    }

    f = fopen(path, "rb");
    if (!f) {
        md_free_ctx(&ctx);
        return -1;
    }

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (md_hmac_update(&ctx, buf, n) != 0) {
            fclose(f);
            md_free_ctx(&ctx);
            return -1;
        }
    }
    fclose(f);

    if (md_hmac_finish(&ctx, out) != 0) {
        md_free_ctx(&ctx);
        return -1;
    }

    md_free_ctx(&ctx);
    return 0;
}

/* -----------------------------------------------------------------------
 * Context allocation / free
 * --------------------------------------------------------------------- */
static pfd_ctx_t *pfd_ctx_alloc(void) {
    pfd_ctx_t *c = (pfd_ctx_t *)malloc(sizeof(pfd_ctx_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(pfd_ctx_t));

    c->data_size = PFD_MAX_FILE_SIZE;
    c->data = (uint8_t *)malloc(c->data_size);
    if (!c->data) { free(c); return NULL; }

    c->tmp = (uint8_t *)malloc(c->data_size);
    if (!c->tmp) { free(c->data); free(c); return NULL; }

    memset(c->data, 0, c->data_size);
    memset(c->tmp, 0, c->data_size);
    return c;
}

static void pfd_ctx_free(pfd_ctx_t *c) {
    if (!c) return;
    free(c->data);
    free(c->tmp);
    free(c);
}

/* -----------------------------------------------------------------------
 * Wire up the pointer overlay onto ctx->data after data is loaded.
 * --------------------------------------------------------------------- */
static void pfd_ctx_wire(pfd_ctx_t *c) {
    uint64_t cap;

    c->header         = (pfd_header_t *)(c->data + PFD_HEADER_OFFSET);
    c->header_key     = (pfd_header_key_t *)(c->data + PFD_HEADER_KEY_OFFSET);
    c->signature      = (pfd_signature_t *)(c->data + PFD_SIGNATURE_OFFSET);
    c->hash_table_hdr = (pfd_hash_table_hdr_t *)(c->data + PFD_HASH_TABLE_OFFSET);

    cap = c->hash_table_hdr->capacity;
    c->hash_table_entries = (uint64_t *)(c->data + PFD_HASH_TABLE_OFFSET
                                         + PFD_HASH_TABLE_HEADER_SIZE);

    c->entry_table    = (pfd_entry_t *)(c->data + PFD_HASH_TABLE_OFFSET
                                        + PFD_HASH_TABLE_HEADER_SIZE
                                        + cap * sizeof(uint64_t));

    c->entry_sig_table = (uint8_t *)(c->data + PFD_HASH_TABLE_OFFSET
                                     + PFD_HASH_TABLE_HEADER_SIZE
                                     + cap * sizeof(uint64_t)
                                     + c->hash_table_hdr->num_reserved
                                       * PFD_ENTRY_SIZE);
}

/* -----------------------------------------------------------------------
 * Hash table entry index computation (djb2 mod capacity).
 * --------------------------------------------------------------------- */
static uint64_t pfd_hash_slot(uint64_t capacity, const char *name) {
    uint64_t h = 0;
    const uint8_t *p = (const uint8_t *)name;
    while (*p)
        h = (h << 5) - h + (uint64_t)(*p++);
    return h % capacity;
}

/* -----------------------------------------------------------------------
 * Derive real_hash_key from the (decrypted) signature's hash_key.
 *
 * V4: real_hash_key = HMAC-SHA1(keygen_key, signature.hash_key)
 * V3: real_hash_key = signature.hash_key
 * --------------------------------------------------------------------- */
static int pfd_derive_real_hash_key(pfd_ctx_t *c) {
    if (c->header->version == PFD_VERSION_V4) {
        return hmac_sha1_buf(k_keygen_key, PFD_KEYGEN_KEY_SIZE,
                             c->signature->hash_key, PFD_HASH_KEY_SIZE,
                             c->real_hash_key);
    }
    memcpy(c->real_hash_key, c->signature->hash_key, PFD_HASH_KEY_SIZE);
    return 0;
}

/* -----------------------------------------------------------------------
 * Derive the HMAC key used for a given entry's file hash slot.
 *
 * For PARAM.SFO:
 *   slot 0 → savegame_param_sfo_key (20 bytes)
 *   slot 1 → console_id             (16 bytes)
 *   slot 2 → disc_hash_key          (16 bytes)
 *   slot 3 → auth_id                (8 bytes)
 * For all other files:
 *   slot 0 → key derived from secure_file_id via the scramble below.
 *             In portable mode we use a zero secure_file_id.
 *   slots 1,2,3 are not computed (PFD_UPDATE_FAILURE_NO_DATA).
 *
 * out_key receives the derived key bytes; *out_len receives its length.
 * Returns 0 on success, -1 if the slot is not applicable.
 * --------------------------------------------------------------------- */
static int pfd_derive_entry_hash_key(pfd_ctx_t *c,
                                     const char *file_name,
                                     int slot,
                                     uint8_t out_key[PFD_HASH_KEY_SIZE],
                                     uint32_t *out_len) {
    int is_param_sfo = (strncasecmp(file_name, "PARAM.SFO",
                                    PFD_ENTRY_NAME_SIZE) == 0);
    int i, j;

    memset(out_key, 0, PFD_HASH_KEY_SIZE);

    if (is_param_sfo) {
        switch (slot) {
        case PFD_ENTRY_HASH_FILE:
            memcpy(out_key, k_savegame_param_sfo_key, PFD_PARAM_SFO_KEY_SIZE);
            *out_len = PFD_PARAM_SFO_KEY_SIZE;
            return 0;
        case PFD_ENTRY_HASH_FILE_CID:
            memcpy(out_key, c->console_id, 16);
            *out_len = 16;
            return 0;
        case PFD_ENTRY_HASH_FILE_DHK_CID2:
            memcpy(out_key, k_fallback_disc_hash_key, PFD_DISC_HASH_KEY_SIZE);
            *out_len = PFD_DISC_HASH_KEY_SIZE;
            return 0;
        case PFD_ENTRY_HASH_FILE_AID_UID:
            memcpy(out_key, c->auth_id, PFD_AUTHENTICATION_ID_SIZE);
            *out_len = PFD_AUTHENTICATION_ID_SIZE;
            return 0;
        default:
            return -1;
        }
    }

    /* Non-PARAM.SFO files: only slot 0 is computed.
     * Key derivation from a 16-byte secure_file_id (zeros in portable mode):
     * Builds a 20-byte key by inserting sentinel bytes at positions
     * 1, 2, 5, 8 and filling the rest from the secure_file_id. */
    if (slot != PFD_ENTRY_HASH_FILE)
        return -1;

    {
        /* zero secure_file_id for portable mode */
        uint8_t sfid[PFD_SECURE_FILE_ID_SIZE];
        memset(sfid, 0, PFD_SECURE_FILE_ID_SIZE);

        for (i = 0, j = 0; i < PFD_HASH_KEY_SIZE; i++) {
            switch (i) {
            case 1:  out_key[i] = 11; break;
            case 2:  out_key[i] = 15; break;
            case 5:  out_key[i] = 14; break;
            case 8:  out_key[i] = 10; break;
            default: out_key[i] = sfid[j++]; break;
            }
        }
        *out_len = PFD_HASH_KEY_SIZE;
        return 0;
    }
}

/* -----------------------------------------------------------------------
 * Compute the HMAC hash for one entry row (covers file_name + entry data
 * in all chained entries under that hash slot).
 * --------------------------------------------------------------------- */
static int pfd_calc_entry_hash(pfd_ctx_t *c, const char *file_name,
                               uint8_t out[PFD_HASH_SIZE]) {
    uint64_t slot = pfd_hash_slot(c->hash_table_hdr->capacity, file_name);
    uint64_t idx  = c->hash_table_entries[slot];
    uint64_t rsrv = c->hash_table_hdr->num_reserved;
    md_context_t sha1;

    if (idx >= rsrv)
        return -1;

    memset(&sha1, 0, sizeof(sha1));
    if (md_init_ctx(&sha1, md_info_from_type(POLARSSL_MD_SHA1)) != 0)
        return -1;

    if (md_hmac_starts(&sha1, c->real_hash_key, PFD_HASH_KEY_SIZE) != 0) {
        md_free_ctx(&sha1);
        return -1;
    }

    while (idx < rsrv) {
        pfd_entry_t *e = &c->entry_table[idx];
        md_hmac_update(&sha1, (const uint8_t *)e->file_name, PFD_ENTRY_NAME_SIZE);
        /* entry->key (64) + file_hashes (80) + _pad1 (40) = 184 bytes after file_name+pad */
        md_hmac_update(&sha1, e->key, PFD_ENTRY_DATA_SIZE);
        idx = e->additional_index;
    }

    if (md_hmac_finish(&sha1, out) != 0) {
        md_free_ctx(&sha1);
        return -1;
    }

    md_free_ctx(&sha1);
    return 0;
}

/* Compute top hash = HMAC(real_hash_key, hash_table_raw_bytes) */
static int pfd_calc_top_hash(pfd_ctx_t *c, uint8_t out[PFD_HASH_SIZE]) {
    size_t ht_size = PFD_HASH_TABLE_HEADER_SIZE
                     + (size_t)c->hash_table_hdr->capacity * sizeof(uint64_t);
    return hmac_sha1_buf(c->real_hash_key, PFD_HASH_KEY_SIZE,
                         (const uint8_t *)c->hash_table_hdr, ht_size, out);
}

/* Compute bottom hash = HMAC(real_hash_key, entry_sig_table_raw_bytes) */
static int pfd_calc_bottom_hash(pfd_ctx_t *c, uint8_t out[PFD_HASH_SIZE]) {
    size_t est_size = (size_t)c->hash_table_hdr->capacity * PFD_HASH_SIZE;
    return hmac_sha1_buf(c->real_hash_key, PFD_HASH_KEY_SIZE,
                         c->entry_sig_table, est_size, out);
}

/* Compute default hash = HMAC(real_hash_key, empty) — used for empty slots */
static int pfd_calc_default_hash(pfd_ctx_t *c, uint8_t out[PFD_HASH_SIZE]) {
    return hmac_sha1_buf(c->real_hash_key, PFD_HASH_KEY_SIZE,
                         NULL, 0, out);
}

/* -----------------------------------------------------------------------
 * Update all four file hash slots for all entries in the PFD.
 * Slot 0 is always computed.  Slots 1-3 are PARAM.SFO-only.
 * Missing keys produce PFD_UPDATE_FAILURE_NO_DATA but are non-fatal.
 * --------------------------------------------------------------------- */
static void pfd_update_file_hashes(pfd_ctx_t *c, const char *dir) {
    uint64_t n = c->hash_table_hdr->num_used;
    uint64_t i;
    int slot;

    for (i = 0; i < n; i++) {
        pfd_entry_t *e = &c->entry_table[i];
        char path[PFD_PATH_MAX];
        build_path(path, sizeof(path), dir, e->file_name);

        for (slot = 0; slot < PFD_NUM_FILE_HASHES; slot++) {
            uint8_t  hkey[PFD_HASH_KEY_SIZE];
            uint32_t hlen;

            if (pfd_derive_entry_hash_key(c, e->file_name, slot,
                                          hkey, &hlen) < 0)
                continue;  /* not applicable for this file/slot */

            /* Ignore error: file may not exist (will hash as if empty). */
            hmac_sha1_file(path, hkey, hlen, e->file_hashes[slot]);
        }
    }
}

/* -----------------------------------------------------------------------
 * Recompute the entry signature table and top/bottom hashes.
 * Must be called after pfd_update_file_hashes().
 * --------------------------------------------------------------------- */
static int pfd_update_structure_hashes(pfd_ctx_t *c) {
    uint64_t cap  = c->hash_table_hdr->capacity;
    uint64_t used = c->hash_table_hdr->num_used;
    uint64_t i;
    uint8_t  dflt[PFD_HASH_SIZE];
    uint8_t  hash[PFD_HASH_SIZE];

    /* Fill all empty slots with the default hash */
    if (pfd_calc_default_hash(c, dflt) < 0)
        return -1;

    for (i = 0; i < cap; i++) {
        if (c->hash_table_entries[i] >= cap)
            memcpy(c->entry_sig_table + i * PFD_HASH_SIZE, dflt, PFD_HASH_SIZE);
    }

    /* Fill occupied slots with the per-entry hash */
    for (i = 0; i < used; i++) {
        pfd_entry_t *e = &c->entry_table[i];
        uint64_t slot  = pfd_hash_slot(cap, e->file_name);

        if (pfd_calc_entry_hash(c, e->file_name, hash) < 0)
            return -1;
        memcpy(c->entry_sig_table + slot * PFD_HASH_SIZE, hash, PFD_HASH_SIZE);
    }

    /* Bottom hash over entry_sig_table */
    if (pfd_calc_bottom_hash(c, hash) < 0)
        return -1;
    memcpy(c->signature->bottom_hash, hash, PFD_HASH_SIZE);

    /* Top hash over hash_table (header + entries array) */
    if (pfd_calc_top_hash(c, hash) < 0)
        return -1;
    memcpy(c->signature->top_hash, hash, PFD_HASH_SIZE);

    return 0;
}

/* -----------------------------------------------------------------------
 * Apply opts → populate ctx fields (console_id, auth_id, user_id).
 * --------------------------------------------------------------------- */
static void pfd_ctx_apply_opts(pfd_ctx_t *c,
                               const savesync_pfd_options_t *opts) {
    if (!opts) {
        /* portable defaults */
        memset(c->console_id, 0, sizeof(c->console_id));
        memcpy(c->auth_id, k_authentication_id, PFD_AUTHENTICATION_ID_SIZE);
        memset(c->user_id, 0, sizeof(c->user_id));
        return;
    }

    if (opts->cross_account) {
        memset(c->console_id, 0, sizeof(c->console_id));
        memset(c->auth_id, 0, sizeof(c->auth_id));
        memset(c->user_id, 0, sizeof(c->user_id));
    } else {
        memcpy(c->console_id, opts->console_id, 16);

        /* auth_id: if all-zero in opts, use the default key */
        {
            int is_zero = 1;
            int k;
            for (k = 0; k < PFD_AUTHENTICATION_ID_SIZE; k++) {
                if (opts->auth_id[k]) { is_zero = 0; break; }
            }
            if (is_zero)
                memcpy(c->auth_id, k_authentication_id,
                       PFD_AUTHENTICATION_ID_SIZE);
            else
                memcpy(c->auth_id, opts->auth_id,
                       PFD_AUTHENTICATION_ID_SIZE);
        }

        /* user_id: 8 bytes from the 8-char ASCII string */
        memset(c->user_id, 0, sizeof(c->user_id));
        if (opts->user_id[0])
            memcpy(c->user_id, opts->user_id,
                   sizeof(c->user_id)); /* no NUL — raw bytes */
    }
}

/* -----------------------------------------------------------------------
 * savesync_pfd_default_options
 * --------------------------------------------------------------------- */
void savesync_pfd_default_options(savesync_pfd_options_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cross_account = 1;
}

/* -----------------------------------------------------------------------
 * savesync_pfd_strip
 * --------------------------------------------------------------------- */
int savesync_pfd_strip(const char *save_dir) {
    char path[PFD_PATH_MAX];
    build_path(path, sizeof(path), save_dir, "PARAM.PFD");
    if (access(path, F_OK) != 0)
        return 0;   /* already absent — idempotent */
    if (unlink(path) != 0)
        return -1;
    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_pfd_build
 *
 * Enumerate regular files in save_dir → build a fresh PFD.
 *
 * Layout:
 *   capacity  = next_pow2(max(2 * file_count, 4))
 *   num_reserved = capacity   (every hash-table slot can hold one entry,
 *                              collision chains use reserved overflow slots)
 *
 * The entry key (64 bytes) is left all-zeros — this is correct for portable
 * mode because file content is not encrypted.
 * --------------------------------------------------------------------- */
int savesync_pfd_build(const char *save_dir,
                       const savesync_pfd_options_t *opts) {
    /* Collect file names from directory */
    char names[128][PFD_ENTRY_NAME_SIZE];
    int  n_files = 0;
    DIR  *d;
    struct dirent *de;

    pfd_keys_setup();

    d = opendir(save_dir);
    if (!d) return -1;

    while ((de = readdir(d)) != NULL && n_files < 128) {
        char fpath[PFD_PATH_MAX];
        struct stat st;

        if (de->d_name[0] == '.') continue;
        if (strcasecmp(de->d_name, "PARAM.PFD") == 0) continue;

        build_path(fpath, sizeof(fpath), save_dir, de->d_name);
        if (stat(fpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        strncpy(names[n_files], de->d_name, PFD_ENTRY_NAME_SIZE - 1);
        names[n_files][PFD_ENTRY_NAME_SIZE - 1] = '\0';
        n_files++;
    }
    closedir(d);

    if (n_files == 0)
        return -1;  /* nothing to sign */

    /* Allocate context */
    pfd_ctx_t *c = pfd_ctx_alloc();
    if (!c) return -1;

    pfd_ctx_apply_opts(c, opts);

    /* Determine dimensions */
    uint64_t capacity = next_pow2((uint64_t)n_files * 2);
    uint64_t num_rsvd = capacity;  /* we pre-allocate capacity entry rows */

    /* Check it fits */
    size_t required = PFD_HASH_TABLE_OFFSET
                      + PFD_HASH_TABLE_HEADER_SIZE
                      + (size_t)capacity * sizeof(uint64_t)
                      + (size_t)num_rsvd * PFD_ENTRY_SIZE
                      + (size_t)capacity * PFD_HASH_SIZE;
    if (required > PFD_MAX_FILE_SIZE) {
        pfd_ctx_free(c);
        return -1;
    }

    /* Zero the data buffer and wire pointers */
    memset(c->data, 0, c->data_size);
    c->hash_table_hdr = (pfd_hash_table_hdr_t *)(c->data + PFD_HASH_TABLE_OFFSET);
    c->hash_table_hdr->capacity    = capacity;
    c->hash_table_hdr->num_reserved = num_rsvd;
    c->hash_table_hdr->num_used    = 0;
    pfd_ctx_wire(c);  /* re-wire after setting capacity */

    /* Fill hash table sentinel values */
    {
        uint64_t i;
        for (i = 0; i < capacity; i++)
            c->hash_table_entries[i] = PFD_ENTRY_INDEX_SENTINEL;
    }

    /* Write header */
    c->header->magic   = PFD_MAGIC;
    c->header->version = PFD_VERSION_V4;

    /* Generate a random-ish hash_key (use zero for portable mode; the
     * keygen_key round ensures uniqueness per-PFD on real hardware).
     * We use zeros — acceptable for portable RPCS3-origin saves. */
    memset(c->signature->hash_key, 0, PFD_HASH_KEY_SIZE);

    /* Derive real_hash_key */
    if (pfd_derive_real_hash_key(c) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    /* Insert entries */
    {
        int fi;
        for (fi = 0; fi < n_files; fi++) {
            uint64_t used = c->hash_table_hdr->num_used;
            pfd_entry_t *e = &c->entry_table[used];
            uint64_t slot  = pfd_hash_slot(capacity, names[fi]);
            char     fpath[PFD_PATH_MAX];

            memset(e, 0, PFD_ENTRY_SIZE);
            strncpy(e->file_name, names[fi], PFD_ENTRY_NAME_SIZE - 1);
            e->additional_index = PFD_ENTRY_INDEX_SENTINEL;

            build_path(fpath, sizeof(fpath), save_dir, names[fi]);
            e->file_size = get_file_size(fpath);

            /* Insert into hash table (open addressing with chaining) */
            if (c->hash_table_entries[slot] >= num_rsvd) {
                /* Empty slot — direct link */
                c->hash_table_entries[slot] = used;
            } else {
                /* Walk chain to tail and append */
                uint64_t prev = c->hash_table_entries[slot];
                while (c->entry_table[prev].additional_index < num_rsvd)
                    prev = c->entry_table[prev].additional_index;
                c->entry_table[prev].additional_index = used;
            }

            c->hash_table_hdr->num_used++;
        }
    }

    /* Compute file hashes for all entries */
    pfd_update_file_hashes(c, save_dir);

    /* Compute structure hashes (entry_sig_table, top, bottom) */
    if (pfd_update_structure_hashes(c) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    /* Encrypt signature block with CBC (IV = header_key) */
    {
        /* header_key is all zeros (set during memset above) */
        uint8_t *sig_bytes = c->data + PFD_SIGNATURE_OFFSET;
        if (pfd_cbc_encrypt(c, *c->header_key, sig_bytes,
                            PFD_SIGNATURE_SIZE) < 0) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    /* Write PARAM.PFD */
    {
        char out_path[PFD_PATH_MAX];
        build_path(out_path, sizeof(out_path), save_dir, "PARAM.PFD");
        if (write_file_exact(out_path, c->data, required) < 0) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    pfd_ctx_free(c);
    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_pfd_resign
 *
 * Load existing PFD, reconcile entries with current files, resign.
 * --------------------------------------------------------------------- */
int savesync_pfd_resign(const char *save_dir,
                        const savesync_pfd_options_t *opts) {
    char pfd_path[PFD_PATH_MAX];
    size_t  file_sz;
    pfd_ctx_t *c;

    pfd_keys_setup();

    build_path(pfd_path, sizeof(pfd_path), save_dir, "PARAM.PFD");

    c = pfd_ctx_alloc();
    if (!c) return -1;

    /* Read existing PFD */
    {
        long r = read_file_partial(pfd_path, c->data, c->data_size, &file_sz);
        if (r < 0 || file_sz < (PFD_HASH_TABLE_OFFSET + PFD_HASH_TABLE_HEADER_SIZE)) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    /* Validate magic */
    {
        pfd_header_t *h = (pfd_header_t *)(c->data + PFD_HEADER_OFFSET);
        if (h->magic != PFD_MAGIC ||
            (h->version != PFD_VERSION_V3 && h->version != PFD_VERSION_V4)) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    /* Wire pointers (requires capacity to already be in data) */
    c->hash_table_hdr = (pfd_hash_table_hdr_t *)(c->data + PFD_HASH_TABLE_OFFSET);
    pfd_ctx_wire(c);

    /* Decrypt signature block so we can read hash_key */
    {
        uint8_t *sig_bytes = c->data + PFD_SIGNATURE_OFFSET;
        if (pfd_cbc_decrypt(c, *c->header_key, sig_bytes,
                            PFD_SIGNATURE_SIZE) < 0) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    if (pfd_derive_real_hash_key(c) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    pfd_ctx_apply_opts(c, opts);

    /* Refresh file sizes (RPCS3 may have changed them) */
    {
        uint64_t i;
        for (i = 0; i < c->hash_table_hdr->num_used; i++) {
            pfd_entry_t *e = &c->entry_table[i];
            char fpath[PFD_PATH_MAX];
            build_path(fpath, sizeof(fpath), save_dir, e->file_name);
            uint64_t sz = get_file_size(fpath);
            if (sz > 0)
                e->file_size = sz;
        }
    }

    /* Recompute all hashes */
    pfd_update_file_hashes(c, save_dir);

    if (pfd_update_structure_hashes(c) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    /* Re-encrypt signature block */
    {
        uint8_t *sig_bytes = c->data + PFD_SIGNATURE_OFFSET;
        if (pfd_cbc_encrypt(c, *c->header_key, sig_bytes,
                            PFD_SIGNATURE_SIZE) < 0) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    /* Write back */
    if (write_file_exact(pfd_path, c->data, file_sz) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    pfd_ctx_free(c);
    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_pfd_validate
 * --------------------------------------------------------------------- */
int savesync_pfd_validate(const char *save_dir) {
    char pfd_path[PFD_PATH_MAX];
    size_t file_sz;
    pfd_ctx_t *c;
    uint8_t expected[PFD_HASH_SIZE];
    uint8_t computed[PFD_HASH_SIZE];
    int ret = 0;

    pfd_keys_setup();

    build_path(pfd_path, sizeof(pfd_path), save_dir, "PARAM.PFD");

    c = pfd_ctx_alloc();
    if (!c) return -1;

    {
        long r = read_file_partial(pfd_path, c->data, c->data_size, &file_sz);
        if (r < 0 || file_sz < (PFD_HASH_TABLE_OFFSET + PFD_HASH_TABLE_HEADER_SIZE)) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    {
        pfd_header_t *h = (pfd_header_t *)(c->data + PFD_HEADER_OFFSET);
        if (h->magic != PFD_MAGIC ||
            (h->version != PFD_VERSION_V3 && h->version != PFD_VERSION_V4)) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    c->hash_table_hdr = (pfd_hash_table_hdr_t *)(c->data + PFD_HASH_TABLE_OFFSET);
    pfd_ctx_wire(c);

    /* Decrypt signature */
    {
        uint8_t *sig_bytes = c->data + PFD_SIGNATURE_OFFSET;
        if (pfd_cbc_decrypt(c, *c->header_key, sig_bytes,
                            PFD_SIGNATURE_SIZE) < 0) {
            pfd_ctx_free(c);
            return -1;
        }
    }

    if (pfd_derive_real_hash_key(c) < 0) {
        pfd_ctx_free(c);
        return -1;
    }

    /* Validate top hash */
    memcpy(expected, c->signature->top_hash, PFD_HASH_SIZE);
    if (pfd_calc_top_hash(c, computed) < 0) {
        pfd_ctx_free(c);
        return -1;
    }
    if (memcmp(expected, computed, PFD_HASH_SIZE) != 0) {
        ret = -2;
        goto done;
    }

    /* Validate bottom hash */
    memcpy(expected, c->signature->bottom_hash, PFD_HASH_SIZE);
    if (pfd_calc_bottom_hash(c, computed) < 0) {
        pfd_ctx_free(c);
        return -1;
    }
    if (memcmp(expected, computed, PFD_HASH_SIZE) != 0) {
        ret = -3;
        goto done;
    }

done:
    pfd_ctx_free(c);
    return ret;
}

/* -----------------------------------------------------------------------
 * savesync_pfd_self_test
 *
 * Known-answer test: HMAC-SHA1("key", "The quick brown fox...") =
 *   DE 7C 9B 85 B8 B7 8A A6 BC 8A 7A 36 F7 0A 90 70 1C 9D B4 D9
 * (from RFC 2202 test case 2 — key = "Jefe", data = "what do ya want for nothing?",
 *  but we use a simpler well-known vector instead.)
 *
 * We use the NIST FIPS 198a example:
 *   key  = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
 *   data = "Hi There"
 *   expected HMAC-SHA1 = b617318655057264e28bc0b6fb378c8ef146be00
 * --------------------------------------------------------------------- */
int savesync_pfd_self_test(void) {
    static const uint8_t key[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
    };
    static const uint8_t data[]  = "Hi There";
    static const uint8_t expect[PFD_HASH_SIZE] = {
        0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,
        0xe2,0x8b,0xc0,0xb6,0xfb,0x37,0x8c,0x8e,
        0xf1,0x46,0xbe,0x00
    };
    uint8_t got[PFD_HASH_SIZE];

    pfd_keys_setup();

    if (hmac_sha1_buf(key, sizeof(key), data, sizeof(data) - 1, got) != 0)
        return 1;

    return (memcmp(got, expect, PFD_HASH_SIZE) == 0) ? 0 : 1;
}
