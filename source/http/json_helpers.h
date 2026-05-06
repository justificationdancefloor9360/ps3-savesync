/*
 * json_helpers.h — json-c wrappers for serializing savesync structures.
 *
 * All functions return a malloc'd NUL-terminated JSON string.
 * The caller must free() the returned pointer.
 * Returns NULL on allocation failure.
 */

#ifndef SAVESYNC_JSON_HELPERS_H
#define SAVESYNC_JSON_HELPERS_H

#include <stddef.h>

#include "savedata/savedata.h"
#include "state/jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize a savesync_save_t to a JSON object string.
 * Fields match the GET /api/saves contract in the SPA.
 */
char *savesync_json_save(const savesync_save_t *save);

/*
 * Serialize an array of savesync_save_t to a JSON array string.
 */
char *savesync_json_save_array(const savesync_save_t *saves, size_t count);

/*
 * Group an array of savesync_save_t by title_id and emit a JSON array of
 * { title_id, title, has_icon0, account_id_hex, slot_count, slots: [...] }.
 * Each slot is the same object shape as savesync_json_save.
 *
 * The "title" picked for each game is the first non-empty SFO TITLE among
 * its slots; "has_icon0" / "account_id_hex" come from the first slot.
 */
char *savesync_json_game_array(const savesync_save_t *saves, size_t count);

/*
 * Build the POST /api/inspect response body:
 *   { stage_id, title_id, title, subtitle, suggested_dir_name, account_id_hex }
 */
char *savesync_json_inspect_result(const char *stage_id,
                                   const char *title_id,
                                   const char *title,
                                   const char *subtitle,
                                   const char *suggested_dir_name,
                                   const char *account_id_hex);

/*
 * Serialize a savesync_job_t to a JSON object string.
 * Fields match the GET /api/jobs contract in the SPA.
 */
char *savesync_json_job(const savesync_job_t *job);

/*
 * Serialize an array of savesync_job_t to a JSON array string.
 */
char *savesync_json_job_array(const savesync_job_t *jobs, size_t count);

/*
 * Build the GET /api/status response body.
 * version:          SAVESYNC_VERSION string
 * console_id_short: empty string for v1
 */
char *savesync_json_status(void);

/*
 * Build a { "job_id": "<id>" } response for POST /api/upload.
 */
char *savesync_json_job_id(const char *job_id);

/*
 * Build a { "error": "<msg>" } error response body.
 */
char *savesync_json_error(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_JSON_HELPERS_H */
