/*
 * json_helpers.c — json-c wrappers for serializing savesync structures.
 */

#include "json_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "savesync.h"
#include "savedata/savedata.h"
#include "savedata/convert.h"
#include "state/jobs.h"

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

/* Map flavor enum to the string the SPA expects. */
static const char *flavor_str_api(savesync_flavor_t f)
{
    switch (f) {
        case SAVESYNC_FLAVOR_PS3_SIGNED:   return "ps3-signed";
        case SAVESYNC_FLAVOR_PS3_UNSIGNED: return "ps3-unsigned";
        case SAVESYNC_FLAVOR_RPCS3:        return "rpcs3";
        default:                           return "unknown";
    }
}

/* Map location enum to the string the SPA expects. */
static const char *location_str_api(savesync_location_t l)
{
    switch (l) {
        case SAVESYNC_LOCATION_HDD:          return "hdd";
        case SAVESYNC_LOCATION_USB:          return "usb";
        case SAVESYNC_LOCATION_RPCS3_BUNDLE: return "rpcs3-bundle";
        default:                             return "hdd";
    }
}

/* Map job kind enum to string. */
static const char *job_kind_str(savesync_job_kind_t k)
{
    switch (k) {
        case SAVESYNC_JOB_KIND_EXPORT:  return "export";
        case SAVESYNC_JOB_KIND_IMPORT:  return "import";
        case SAVESYNC_JOB_KIND_CONVERT: return "convert";
        default:                        return "export";
    }
}

/* Map combined job state + phase to the SPA "phase" string.
 * The SPA treats "phase" as a combined state+phase indicator. */
static const char *job_phase_str(const savesync_job_t *job)
{
    switch (job->state) {
        case SAVESYNC_JOB_PENDING:   return "prepare";
        case SAVESYNC_JOB_DONE:      return "done";
        case SAVESYNC_JOB_FAILED:    return "failed";
        case SAVESYNC_JOB_CANCELLED: return "failed";
        case SAVESYNC_JOB_RUNNING:
            switch (job->progress.phase) {
                case SAVESYNC_PHASE_PREPARE:       return "prepare";
                case SAVESYNC_PHASE_COPY:          return "copy";
                case SAVESYNC_PHASE_TRANSFORM_SFO: return "transform_sfo";
                case SAVESYNC_PHASE_SIGN_PFD:      return "sign_pfd";
                case SAVESYNC_PHASE_ARCHIVE:       return "archive";
                case SAVESYNC_PHASE_DONE:          return "done";
                case SAVESYNC_PHASE_FAILED:        return "failed";
                default:                           return "running";
            }
        default:
            return "prepare";
    }
}

/* Serialize a single save to a json_object (caller takes ownership). */
static struct json_object *save_to_json(const savesync_save_t *save)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;

    json_object_object_add(obj, "id",
        json_object_new_string(save->dir_name));
    json_object_object_add(obj, "dir_name",
        json_object_new_string(save->dir_name));
    json_object_object_add(obj, "title_id",
        json_object_new_string(save->title_id));
    json_object_object_add(obj, "title",
        json_object_new_string(save->title));
    json_object_object_add(obj, "subtitle",
        json_object_new_string(save->subtitle));
    json_object_object_add(obj, "flavor",
        json_object_new_string(flavor_str_api(save->flavor)));
    json_object_object_add(obj, "location",
        json_object_new_string(location_str_api(save->location)));
    json_object_object_add(obj, "total_size_bytes",
        json_object_new_int64((int64_t)save->total_size_bytes));
    json_object_object_add(obj, "file_count",
        json_object_new_int((int)save->file_count));
    json_object_object_add(obj, "account_id_hex",
        json_object_new_string(save->account_id_hex));

    return obj;
}

/* Serialize a single job to a json_object (caller takes ownership). */
static struct json_object *job_to_json(const savesync_job_t *job)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;

    json_object_object_add(obj, "id",
        json_object_new_string(job->id));
    json_object_object_add(obj, "label",
        json_object_new_string(job->label));
    json_object_object_add(obj, "kind",
        json_object_new_string(job_kind_str(job->kind)));
    json_object_object_add(obj, "phase",
        json_object_new_string(job_phase_str(job)));
    json_object_object_add(obj, "current_file",
        json_object_new_string(job->progress.current_file));
    json_object_object_add(obj, "files_done",
        json_object_new_int((int)job->progress.files_done));
    json_object_object_add(obj, "files_total",
        json_object_new_int((int)job->progress.files_total));
    json_object_object_add(obj, "bytes_done",
        json_object_new_int64((int64_t)job->progress.bytes_done));
    json_object_object_add(obj, "bytes_total",
        json_object_new_int64((int64_t)job->progress.bytes_total));

    /* download_url is null unless job is done and has a url set. */
    if (job->state == SAVESYNC_JOB_DONE && job->download_url[0] != '\0') {
        json_object_object_add(obj, "download_url",
            json_object_new_string(job->download_url));
    } else {
        json_object_object_add(obj, "download_url",
            NULL);
    }

    return obj;
}

/* Stringify a json_object and return a malloc'd copy. Frees the object. */
static char *json_to_str(struct json_object *obj)
{
    if (!obj) return NULL;
    const char *tmp = json_object_to_json_string_ext(obj,
        JSON_C_TO_STRING_PLAIN);
    char *out = NULL;
    if (tmp) {
        out = strdup(tmp);
    }
    json_object_put(obj);
    return out;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

char *savesync_json_save(const savesync_save_t *save)
{
    return json_to_str(save_to_json(save));
}

char *savesync_json_save_array(const savesync_save_t *saves, size_t count)
{
    struct json_object *arr = json_object_new_array();
    if (!arr) return NULL;

    for (size_t i = 0; i < count; i++) {
        struct json_object *obj = save_to_json(&saves[i]);
        if (!obj) {
            json_object_put(arr);
            return NULL;
        }
        json_object_array_add(arr, obj);
    }
    return json_to_str(arr);
}

char *savesync_json_game_array(const savesync_save_t *saves, size_t count)
{
    /* Two-pass grouping by title_id. We don't try to be efficient — savesync
     * never lists more than a few hundred saves on a real PS3. The first pass
     * collects unique title_ids in encounter order; the second pass walks
     * them and builds a slots[] array per game. */
    struct json_object *out = json_object_new_array();
    if (!out) return NULL;

    /* Track which input indices have been emitted, to avoid O(n^2) lookups
     * dominating but keeping the code simple. */
    char *seen = (char *)calloc(count, 1);
    if (count > 0 && !seen) { json_object_put(out); return NULL; }

    for (size_t i = 0; i < count; i++) {
        if (seen[i]) continue;
        const savesync_save_t *anchor = &saves[i];
        const char *tid = anchor->title_id;

        struct json_object *game = json_object_new_object();
        struct json_object *slots = json_object_new_array();
        if (!game || !slots) {
            if (game) json_object_put(game);
            if (slots) json_object_put(slots);
            free(seen);
            json_object_put(out);
            return NULL;
        }

        const char *game_title = anchor->title;
        int        has_icon0   = anchor->has_icon0;
        const char *acct       = anchor->account_id_hex;

        /* Walk forward to gather slots for this title_id. */
        size_t slot_count = 0;
        for (size_t j = i; j < count; j++) {
            if (seen[j]) continue;
            if (strcmp(saves[j].title_id, tid) != 0) continue;
            seen[j] = 1;
            slot_count++;
            /* Prefer the first non-empty title for the game. */
            if (!game_title || !game_title[0]) game_title = saves[j].title;
            if (!has_icon0)                    has_icon0  = saves[j].has_icon0;
            if (!acct || !acct[0])             acct       = saves[j].account_id_hex;
            struct json_object *s = save_to_json(&saves[j]);
            if (!s) continue;
            json_object_array_add(slots, s);
        }

        json_object_object_add(game, "title_id",
            json_object_new_string(tid));
        json_object_object_add(game, "title",
            json_object_new_string(game_title ? game_title : ""));
        json_object_object_add(game, "has_icon0",
            json_object_new_boolean(has_icon0 ? 1 : 0));
        json_object_object_add(game, "account_id_hex",
            json_object_new_string(acct ? acct : ""));
        json_object_object_add(game, "slot_count",
            json_object_new_int((int)slot_count));
        json_object_object_add(game, "slots", slots);

        json_object_array_add(out, game);
    }

    free(seen);
    return json_to_str(out);
}

char *savesync_json_inspect_result(const char *stage_id,
                                   const char *title_id,
                                   const char *title,
                                   const char *subtitle,
                                   const char *suggested_dir_name,
                                   const char *account_id_hex)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;
    json_object_object_add(obj, "stage_id",
        json_object_new_string(stage_id ? stage_id : ""));
    json_object_object_add(obj, "title_id",
        json_object_new_string(title_id ? title_id : ""));
    json_object_object_add(obj, "title",
        json_object_new_string(title ? title : ""));
    json_object_object_add(obj, "subtitle",
        json_object_new_string(subtitle ? subtitle : ""));
    json_object_object_add(obj, "suggested_dir_name",
        json_object_new_string(suggested_dir_name ? suggested_dir_name : ""));
    json_object_object_add(obj, "account_id_hex",
        json_object_new_string(account_id_hex ? account_id_hex : ""));
    return json_to_str(obj);
}

char *savesync_json_job(const savesync_job_t *job)
{
    return json_to_str(job_to_json(job));
}

char *savesync_json_job_array(const savesync_job_t *jobs, size_t count)
{
    struct json_object *arr = json_object_new_array();
    if (!arr) return NULL;

    for (size_t i = 0; i < count; i++) {
        struct json_object *obj = job_to_json(&jobs[i]);
        if (!obj) {
            json_object_put(arr);
            return NULL;
        }
        json_object_array_add(arr, obj);
    }
    return json_to_str(arr);
}

char *savesync_json_status(void)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;
    json_object_object_add(obj, "version",
        json_object_new_string(SAVESYNC_VERSION));
    json_object_object_add(obj, "console_id_short",
        json_object_new_string(""));
    return json_to_str(obj);
}

char *savesync_json_job_id(const char *job_id)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;
    json_object_object_add(obj, "job_id",
        json_object_new_string(job_id ? job_id : ""));
    return json_to_str(obj);
}

char *savesync_json_error(const char *msg)
{
    struct json_object *obj = json_object_new_object();
    if (!obj) return NULL;
    json_object_object_add(obj, "error",
        json_object_new_string(msg ? msg : "internal error"));
    return json_to_str(obj);
}
