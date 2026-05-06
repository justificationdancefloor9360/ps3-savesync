/*
 * convert.h — Save conversion orchestration API for savesync.
 *
 * Coordinates SFO patching, PFD signing, and zip archive operations.
 * All functions return 0 on success, negative on failure.
 */

#ifndef SAVESYNC_CONVERT_H
#define SAVESYNC_CONVERT_H

#include <stdint.h>

#include "savedata.h" /* SAVESYNC_DIR_NAME_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Direction
 * --------------------------------------------------------------------- */
typedef enum {
    SAVESYNC_DIR_PS3_TO_RPCS3 = 0,
    SAVESYNC_DIR_RPCS3_TO_PS3 = 1,
} savesync_direction_t;

/* -----------------------------------------------------------------------
 * Phase — reported via progress callback
 * --------------------------------------------------------------------- */
typedef enum {
    SAVESYNC_PHASE_PREPARE = 0,
    SAVESYNC_PHASE_COPY,
    SAVESYNC_PHASE_TRANSFORM_SFO,
    SAVESYNC_PHASE_SIGN_PFD,
    SAVESYNC_PHASE_ARCHIVE,
    SAVESYNC_PHASE_DONE,
    SAVESYNC_PHASE_FAILED,
} savesync_phase_t;

/* -----------------------------------------------------------------------
 * Progress report — passed to the callback on every notable event.
 * --------------------------------------------------------------------- */
typedef struct {
    savesync_phase_t phase;
    char     current_file[128];
    uint32_t files_done;
    uint32_t files_total;
    uint64_t bytes_done;
    uint64_t bytes_total;
    int      error_code;          /* 0 while in progress or on success */
    char     error_message[160];
} savesync_progress_t;

typedef void (*savesync_progress_cb)(const savesync_progress_t *p, void *user);

/* -----------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------- */
typedef struct {
    savesync_direction_t direction;

    /*
     * 1 = portable output — zeros console_id and ACCOUNT_ID so the save
     * loads on any console/account.  Recommended for all RPCS3-origin saves.
     */
    int cross_account;

    /*
     * 1 = strip any "[RPCS3]" style prefix from SUB_TITLE in PARAM.SFO.
     * Currently a placeholder — no known RPCS3 emulator actually adds this.
     */
    int clear_subtitle_marker;

    /*
     * 8-digit user-id string for RPCS3→PS3 installs (e.g. "00000001").
     * "" means auto-detect: scan /dev_hdd0/home/ and pick the
     * highest-numbered user directory.
     */
    char target_user_id[9];

    /*
     * Slot-picker fields for RPCS3→PS3 import (see savesync_import_zip).
     *
     * target_dir_name: when non-empty, overrides the dir_name derived from
     *   PARAM.SFO's SAVEDATA_DIRECTORY. Used by the web UI to direct an
     *   import into a specific existing slot ("BLES01807-AUTOSAVE") or a
     *   user-typed new slot name. Empty = derive from SFO (default).
     *
     * overwrite: 1 = if target_dir already exists at the final HDD path,
     *   wipe its contents before installing. 0 = fail with -6 if target
     *   exists (default — refuses to clobber).
     *
     * backup: 1 = if target_dir exists, rename it to <dir>.bak.<unix_ts>
     *   before installing. Implies (does not require) overwrite. 0 = no
     *   backup (default).
     */
    char target_dir_name[SAVESYNC_DIR_NAME_LEN];
    int  overwrite;
    int  backup;
} savesync_convert_options_t;

/* Fill *out with portable defaults (cross_account=1, direction=RPCS3_TO_PS3). */
void savesync_convert_default_options(savesync_convert_options_t *out);

/* -----------------------------------------------------------------------
 * Core operations
 * --------------------------------------------------------------------- */

/*
 * savesync_convert_dir — in-place transform of save_dir.
 *
 * PS3→RPCS3: strips PARAM.PFD, clears ACCOUNT_ID in SFO.
 * RPCS3→PS3: patches SFO (clears ACCOUNT_ID if cross_account), builds
 *            a fresh PARAM.PFD via savesync_pfd_build().
 *
 * The directory must already be at its final destination.
 * Progress callback fired at each phase boundary and per file during COPY.
 */
int savesync_convert_dir(const char *save_dir,
                         const savesync_convert_options_t *opts,
                         savesync_progress_cb cb, void *user);

/*
 * savesync_export_zip — pack save_dir into a .zip at out_zip_path.
 *
 * PARAM.PFD is NOT included in the zip (RPCS3 does not use it).
 * A savesync.json manifest is prepended at the zip root for traceability.
 * Progress callback receives ARCHIVE phase updates.
 */
int savesync_export_zip(const char *save_dir,
                        const char *out_zip_path,
                        savesync_progress_cb cb, void *user);

/*
 * savesync_import_zip — extract zip and install to HDD, then sign for PS3.
 *
 * 1. Extracts to /dev_hdd0/tmp/savesync/staging/<hex>/
 * 2. Detects save root inside zip via savesync_archive_find_save_root().
 * 3. Determines final install path under
 *    /dev_hdd0/home/<target_user_id>/savedata/<dir_name>/
 * 4. Copies staging → final path.
 * 5. Runs savesync_convert_dir with RPCS3_TO_PS3.
 *
 * target_user_id from opts: "" triggers auto-detect of highest user dir.
 */
int savesync_import_zip(const char *in_zip_path,
                        const savesync_convert_options_t *opts,
                        savesync_progress_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_CONVERT_H */
