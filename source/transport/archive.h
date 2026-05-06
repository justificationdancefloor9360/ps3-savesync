/*
 * archive.h — ZIP pack/unpack API for savesync.
 *
 * Wraps libzip (portlibs -lzip -lz).  All functions return 0 on success,
 * negative on failure.  Paths use SAVESYNC_PATH_LEN for sizing.
 */

#ifndef SAVESYNC_ARCHIVE_H
#define SAVESYNC_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-entry progress callback.  Called once per file plus optionally at
 * chunk boundaries.  Return value is ignored — there is no abort path.
 *
 *  current_entry  — basename of the file currently being processed
 *  bytes_done     — cumulative bytes processed across all entries so far
 *  bytes_total    — estimated total bytes (0 if unknown)
 *  user           — caller-supplied opaque pointer
 */
typedef int (*savesync_archive_progress_cb)(const char *current_entry,
                                            uint64_t bytes_done,
                                            uint64_t bytes_total,
                                            void *user);

/*
 * savesync_archive_zip_dir
 *
 * Pack all regular files directly under src_dir into out_zip_path.
 * Files are stored with flat names (no directory prefix) so that
 * unzipping produces a flat directory matching the save layout.
 *
 * PARAM.PFD is EXCLUDED (caller handles PFD stripping separately).
 * Any file named "PARAM.PFD" is silently skipped.
 */
int savesync_archive_zip_dir(const char *src_dir,
                             const char *out_zip_path,
                             savesync_archive_progress_cb cb,
                             void *user);

/*
 * savesync_archive_unzip_to
 *
 * Extract all entries from in_zip_path into dest_dir.
 * dest_dir is created if it does not exist.
 * Directory entries in the zip are recreated; file data is written flat
 * under dest_dir/<entry_name> (stripping any leading path components from
 * the save root — callers should detect the root first via
 * savesync_archive_find_save_root and strip it before extraction, or
 * pass a dest_dir that already accounts for the nesting).
 *
 * Files are extracted with their full in-zip path relative to dest_dir.
 */
int savesync_archive_unzip_to(const char *in_zip_path,
                              const char *dest_dir,
                              savesync_archive_progress_cb cb,
                              void *user);

/*
 * savesync_archive_find_save_root
 *
 * Inspect the zip to locate the directory that directly contains PARAM.SFO.
 * Writes that in-zip path prefix (e.g. "BLES01807-SAVE00/") into out_root_in_zip.
 * If PARAM.SFO is at the zip root, writes "" (empty string).
 *
 * Returns 0 if PARAM.SFO was found, -1 if not found or zip unreadable.
 */
int savesync_archive_find_save_root(const char *in_zip_path,
                                    char *out_root_in_zip,
                                    size_t out_size);

/*
 * savesync_archive_extract_one
 *
 * Extract a single entry from a zip archive to out_path.
 * entry_name must match the in-zip path exactly (including any save-root
 * prefix returned by savesync_archive_find_save_root).
 *
 * Used by the inspect endpoint to peek PARAM.SFO without unzipping the
 * whole archive (we re-extract during the actual import job).
 *
 * Returns 0 on success, -1 if entry not found or write failed.
 */
int savesync_archive_extract_one(const char *in_zip_path,
                                 const char *entry_name,
                                 const char *out_path);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_ARCHIVE_H */
