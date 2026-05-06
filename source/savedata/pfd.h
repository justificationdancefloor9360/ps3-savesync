/*
 * pfd.h — Public API for PARAM.PFD signing/parsing (savesync).
 *
 * Self-contained: does not depend on apollo's headers.  Implementation
 * is in pfd.c using polarssl (via polarssl_shim.h).
 */

#ifndef SAVESYNC_PFD_H
#define SAVESYNC_PFD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Options passed to build/resign operations.
 * Use savesync_pfd_default_options() to get a portable zero-initialised
 * starting point, then override fields as needed.
 * --------------------------------------------------------------------- */
typedef struct savesync_pfd_options {
    /*
     * If non-zero, emit all-zero console_id / auth_id so the PFD can be
     * loaded on any console (cross-account / portable mode).
     */
    int cross_account;

    /*
     * 8 ASCII digit user-id string, NUL-terminated (e.g. "00000001").
     * Empty string ("") means all-zeros — portable across accounts.
     * Used for hash variant PFD_ENTRY_HASH_FILE_AID_UID on PARAM.SFO.
     */
    char user_id[9];

    /*
     * 16-byte IDPS (console ID).
     * All-zeros → portable mode (no console binding).
     * Used for hash variant PFD_ENTRY_HASH_FILE_CID / DHK_CID2 on PARAM.SFO.
     */
    uint8_t console_id[16];

    /*
     * 8-byte authentication ID.
     * Zero → use apollo's well-known default (XOR-descrambled at runtime).
     * Leave zeroed unless you want to override.
     */
    uint8_t auth_id[8];
} savesync_pfd_options_t;

/*
 * Fill *out with safe portable defaults:
 *   cross_account = 1, user_id = "", console_id = {0}, auth_id = {0}
 */
void savesync_pfd_default_options(savesync_pfd_options_t *out);

/* -----------------------------------------------------------------------
 * Core operations
 * --------------------------------------------------------------------- */

/*
 * savesync_pfd_strip — Remove PARAM.PFD from a save directory.
 *
 * Idempotent: if PARAM.PFD does not exist, returns 0.
 * Used when converting PS3 → RPCS3 (RPCS3 does not use PFD).
 *
 * Returns 0 on success, negative on I/O error.
 */
int savesync_pfd_strip(const char *save_dir);

/*
 * savesync_pfd_build — Create a fresh PARAM.PFD covering all regular files
 * currently present in save_dir.
 *
 * Scans save_dir, enumerates every regular file (excluding PARAM.PFD itself
 * since it is being generated), builds the full PFD structure (header +
 * hash table + entry table + entry signature table), signs all four hash
 * chains, and writes PARAM.PFD into save_dir.
 *
 * Overwrites any existing PARAM.PFD.
 *
 * Returns 0 on success, negative on failure.
 */
int savesync_pfd_build(const char *save_dir, const savesync_pfd_options_t *opts);

/*
 * savesync_pfd_resign — Re-sign an existing PARAM.PFD.
 *
 * Reads the existing PARAM.PFD, reconciles its entry list with the files
 * currently on disk (adds new files, removes missing ones, updates file
 * sizes), recomputes all hashes, and writes back.
 *
 * Returns 0 on success, negative on failure.
 */
int savesync_pfd_resign(const char *save_dir, const savesync_pfd_options_t *opts);

/*
 * savesync_pfd_validate — Verify the existing PARAM.PFD structure hash
 * chains against current files.
 *
 * Returns  0 if top + bottom hashes are consistent.
 * Returns -1 if PFD is missing or malformed.
 * Returns -2 if the top hash does not match.
 * Returns -3 if the bottom hash does not match.
 *
 * This does not verify per-file HMAC correctness (that would require the
 * game's secure_file_ids); it validates only the PFD's internal integrity.
 */
int savesync_pfd_validate(const char *save_dir);

/* -----------------------------------------------------------------------
 * Smoke-test (optional, not exported in shipping builds)
 * --------------------------------------------------------------------- */

/*
 * savesync_pfd_self_test — Runs key-setup and one HMAC-SHA1 known-answer
 * test.  Returns 0 on pass, 1 on fail.  Suitable for a boot-time sanity
 * check before touching real save data.
 */
int savesync_pfd_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_PFD_H */
