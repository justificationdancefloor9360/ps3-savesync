/*
 * pfd_keys.h — Static keys for PARAM.PFD signing (savesync).
 *
 * All constants are well-known reverse-engineered public values, originally
 * documented by flatz and carried verbatim from apollo-ps3/source/pfd_util.c.
 *
 * The bytes stored here are XOR-scrambled with xor_key (see PFD_XOR_KEY_*).
 * Call pfd_keys_setup() once before using any key — it XORs them in-place
 * into their true values.  The function is idempotent via a guard flag.
 *
 * Do NOT include this header from more than one .c translation unit.  It
 * contains mutable static data.
 */

#ifndef SAVESYNC_PFD_KEYS_H
#define SAVESYNC_PFD_KEYS_H

#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Sizes — mirrors pfd.h from apollo (kept here so this header is
 * self-contained and pfd.c does not need apollo's pfd.h).
 * --------------------------------------------------------------------- */
#define PFD_AUTHENTICATION_ID_SIZE  8
#define PFD_SYSCON_MANAGER_KEY_SIZE 16
#define PFD_DISC_HASH_KEY_SIZE      16
#define PFD_KEYGEN_KEY_SIZE         20
#define PFD_PARAM_SFO_KEY_SIZE      20
#define PFD_SECURE_FILE_ID_SIZE     16
#define PFD_HASH_KEY_SIZE           20   /* HMAC-SHA1 output / real_hash_key */

/* -----------------------------------------------------------------------
 * XOR descrambling key (8 bytes, repeating).
 * --------------------------------------------------------------------- */
#define PFD_XOR_KEY_LEN 8
static const uint8_t k_pfd_xor_key[PFD_XOR_KEY_LEN] = {
    0xD4, 0xD1, 0x6B, 0x0C, 0x5D, 0xB0, 0x87, 0x91
};

/* -----------------------------------------------------------------------
 * Scrambled key data.  These are the raw bytes from apollo's pfd_util.c
 * config initializer.  They are NOT the true keys until XOR-descrambled.
 * --------------------------------------------------------------------- */
static uint8_t k_authentication_id[PFD_AUTHENTICATION_ID_SIZE] = {
    0xC4, 0xC1, 0x6B, 0x0C, 0x5C, 0xB0, 0x87, 0x92,
};

static uint8_t k_syscon_manager_key[PFD_SYSCON_MANAGER_KEY_SIZE] = {
    0x00, 0xC2, 0xD3, 0x9A, 0x3E, 0x51, 0x79, 0x0E,
    0xA1, 0xC5, 0x56, 0x37, 0xE9, 0xE6, 0xD5, 0xE5,
};

static uint8_t k_fallback_disc_hash_key[PFD_DISC_HASH_KEY_SIZE] = {
    0x05, 0x10, 0x8A, 0x07, 0xC1, 0xE4, 0xF9, 0xF9,
    0x4F, 0x51, 0x36, 0xC1, 0xCA, 0xA0, 0x49, 0x1C,
};

static uint8_t k_keygen_key[PFD_KEYGEN_KEY_SIZE] = {
    0xBF, 0xCB, 0xA5, 0xAE, 0x1B, 0x07, 0xC2, 0x6C,
    0x5B, 0x42, 0x1D, 0x37, 0xCF, 0xB5, 0x13, 0x5C,
    0x87, 0x99, 0x50, 0x8E,
};

static uint8_t k_savegame_param_sfo_key[PFD_PARAM_SFO_KEY_SIZE] = {
    0xD8, 0xD9, 0x6B, 0x02, 0x54, 0xB5, 0x83, 0x95,
    0xD9, 0xD0, 0x64, 0x0C, 0x59, 0xB6, 0x85, 0x93,
    0xDD, 0xD7, 0x66, 0x0F,
};

/* -----------------------------------------------------------------------
 * Setup state and function.
 * --------------------------------------------------------------------- */
static int s_pfd_keys_ready = 0;

static void pfd_keys_setup(void) {
    int i;
    if (s_pfd_keys_ready)
        return;

    for (i = 0; i < PFD_AUTHENTICATION_ID_SIZE; i++)
        k_authentication_id[i] ^= k_pfd_xor_key[i % PFD_XOR_KEY_LEN];

    for (i = 0; i < PFD_SYSCON_MANAGER_KEY_SIZE; i++)
        k_syscon_manager_key[i] ^= k_pfd_xor_key[i % PFD_XOR_KEY_LEN];

    for (i = 0; i < PFD_DISC_HASH_KEY_SIZE; i++)
        k_fallback_disc_hash_key[i] ^= k_pfd_xor_key[i % PFD_XOR_KEY_LEN];

    for (i = 0; i < PFD_KEYGEN_KEY_SIZE; i++)
        k_keygen_key[i] ^= k_pfd_xor_key[i % PFD_XOR_KEY_LEN];

    for (i = 0; i < PFD_PARAM_SFO_KEY_SIZE; i++)
        k_savegame_param_sfo_key[i] ^= k_pfd_xor_key[i % PFD_XOR_KEY_LEN];

    s_pfd_keys_ready = 1;
}

#endif /* SAVESYNC_PFD_KEYS_H */
