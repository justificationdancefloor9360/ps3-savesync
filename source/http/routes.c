/*
 * routes.c — savesync HTTP route handlers, built on ps3http.
 *
 * Static asset routes (/, /app.js) ship the embedded SPA. /api/saves and
 * /api/jobs are JSON CRUD over the in-memory job state. The two routes
 * that actually move bytes are:
 *
 *   GET /api/jobs/<id>/download — streams the export zip from disk via
 *     ps3http_response_send_file (64 KB chunks, no full-body malloc).
 *   POST /api/upload — the multipart helper streams the upload payload
 *     directly to /dev_hdd0/tmp/savesync/upload.zip while parsing.
 *
 * Both used to read/write the entire payload through a single contiguous
 * malloc, which fragmented the heap on saves > ~10 MB.
 */

#include "routes.h"
#include "ps3http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/sysfs.h>
#include <json-c/json.h>

#include "embedded_assets.h"
#include "json_helpers.h"
#include "savesync.h"
#include "savedata/savedata.h"
#include "savedata/convert.h"
#include "savedata/sfo.h"
#include "transport/archive.h"
#include "state/jobs.h"

#include <sys/systime.h>

/* Defined in main.cpp; flipped to 1 by /api/quit. */
extern volatile int savesync_should_quit;

/* ----- Small helpers ----------------------------------------------------- */

static int send_json(ps3http_response_t *res, int code, const char *reason,
                     const char *body)
{
    ps3http_response_set_status(res, code, reason);
    return ps3http_response_send_text(res, "application/json", body ? body : "");
}

static int send_404(ps3http_response_t *res) {
    return send_json(res, 404, "Not Found", "{\"error\":\"not found\"}");
}
static int send_405(ps3http_response_t *res) {
    return send_json(res, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
}
static int send_400(ps3http_response_t *res, const char *msg) {
    char *body = savesync_json_error(msg);
    int rc = send_json(res, 400, "Bad Request",
                        body ? body : "{\"error\":\"bad request\"}");
    free(body);
    return rc;
}
static int send_500(ps3http_response_t *res) {
    return send_json(res, 500, "Internal Server Error", "{\"error\":\"server error\"}");
}

/* Match a path of the shape "/prefix/<token>/suffix". On match, copies the
 * <token> into out and returns 1; returns 0 otherwise. */
static int path_match_two(const char *path, const char *prefix, const char *suffix,
                          char *out, size_t out_cap)
{
    size_t plen = strlen(prefix);
    size_t slen = strlen(suffix);
    if (strncmp(path, prefix, plen) != 0) return 0;
    const char *after = path + plen;
    size_t alen = strlen(after);
    if (alen <= slen) return 0;
    if (strcmp(after + alen - slen, suffix) != 0) return 0;
    size_t idlen = alen - slen;
    if (idlen == 0 || idlen >= out_cap) return 0;
    memcpy(out, after, idlen);
    out[idlen] = '\0';
    return 1;
}

/* ----- Static SPA assets ------------------------------------------------- */

static int handle_get_root(ps3http_response_t *res) {
    return ps3http_response_send(res, "text/html; charset=utf-8",
                                  embedded_index_html, embedded_index_html_len);
}
static int handle_get_appjs(ps3http_response_t *res) {
    return ps3http_response_send(res, "application/javascript",
                                  embedded_app_js, embedded_app_js_len);
}

/* ----- /api/status ------------------------------------------------------- */

static int handle_get_status(ps3http_response_t *res) {
    char *body = savesync_json_status();
    if (!body) return send_500(res);
    int rc = ps3http_response_send_text(res, "application/json", body);
    free(body);
    return rc;
}

/* ----- /api/saves -------------------------------------------------------- */

static int handle_get_saves(ps3http_response_t *res) {
    savesync_save_list_t list;
    savesync_save_list_init(&list);
    savesync_scan_hdd(&list);
    savesync_scan_usb(&list);

    char *body = savesync_json_save_array(list.items, list.count);
    savesync_save_list_free(&list);
    if (!body) return send_500(res);

    int rc = ps3http_response_send_text(res, "application/json", body);
    free(body);
    return rc;
}

/* ----- /api/games -------------------------------------------------------- */

/* Same data as /api/saves but bucketed by title_id. The web UI uses this to
 * render game cards with their slots inside; the slot picker on import reads
 * existing_slots[] from the matching game by title_id. */
static int handle_get_games(ps3http_response_t *res) {
    savesync_save_list_t list;
    savesync_save_list_init(&list);
    savesync_scan_hdd(&list);
    savesync_scan_usb(&list);

    char *body = savesync_json_game_array(list.items, list.count);
    savesync_save_list_free(&list);
    if (!body) return send_500(res);

    int rc = ps3http_response_send_text(res, "application/json", body);
    free(body);
    return rc;
}

static int handle_get_save_icon(ps3http_response_t *res, const char *save_id)
{
    savesync_save_list_t list;
    savesync_save_list_init(&list);
    savesync_scan_hdd(&list);
    savesync_scan_usb(&list);

    char icon_path[SAVESYNC_PATH_LEN + 16] = "";
    int found = 0;
    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].dir_name, save_id) == 0) {
            if (list.items[i].has_icon0) {
                snprintf(icon_path, sizeof(icon_path),
                         "%s/ICON0.PNG", list.items[i].path);
                found = 1;
            }
            break;
        }
    }
    savesync_save_list_free(&list);

    if (!found) return send_404(res);
    if (ps3http_response_send_file(res, icon_path, "image/png") != 0)
        return send_404(res);
    return 0;
}

/* ----- /api/jobs --------------------------------------------------------- */

static int handle_get_jobs(ps3http_response_t *res) {
    savesync_job_t jobs[SAVESYNC_MAX_JOBS];
    size_t count = savesync_jobs_snapshot(jobs, SAVESYNC_MAX_JOBS);
    char *body = savesync_json_job_array(jobs, count);
    if (!body) return send_500(res);
    int rc = ps3http_response_send_text(res, "application/json", body);
    free(body);
    return rc;
}

/* Resolve a stage_id from /api/inspect to its on-disk staged zip path.
 * stage_id is the random suffix, NOT a path — we never accept caller-supplied
 * paths here, only an opaque token that we ourselves minted. Returns 0 on
 * success. */
static int resolve_stage_path(const char *stage_id, char *out, size_t outsz)
{
    if (!stage_id || !stage_id[0]) return -1;
    /* Reject anything that looks like path traversal. */
    for (const char *c = stage_id; *c; c++) {
        if (*c == '/' || *c == '\\' || *c == '.') return -1;
    }
    snprintf(out, outsz, "%s/staged-%s.zip", SAVESYNC_TMP_DIR, stage_id);
    return 0;
}

static int handle_post_jobs(ps3http_request_t *req, ps3http_response_t *res)
{
    const void *body_buf = NULL;
    size_t      body_len = 0;
    if (ps3http_request_read_all_body(req, &body_buf, &body_len) != 0)
        return send_400(res, "could not read body");
    if (body_len == 0) return send_400(res, "empty body");

    char *json_str = (char *)malloc(body_len + 1);
    if (!json_str) return send_500(res);
    memcpy(json_str, body_buf, body_len);
    json_str[body_len] = '\0';

    struct json_object *root = json_tokener_parse(json_str);
    free(json_str);
    if (!root) return send_400(res, "invalid JSON");

    struct json_object *kind_obj = NULL;
    json_object_object_get_ex(root, "kind", &kind_obj);
    if (!kind_obj) {
        json_object_put(root);
        return send_400(res, "missing kind");
    }
    const char *kind_str = json_object_get_string(kind_obj);
    if (!kind_str) {
        json_object_put(root);
        return send_400(res, "null kind");
    }

    /* Import kind: drives the slot-picker flow. The zip lives at the
     * stage_id returned by /api/inspect. target_dir_name selects an existing
     * slot or names a new one; overwrite/backup decide what happens when the
     * slot already exists. */
    if (strcmp(kind_str, "import") == 0) {
        struct json_object *stage_obj = NULL, *dir_obj = NULL,
                           *over_obj  = NULL, *bak_obj = NULL,
                           *user_obj  = NULL;
        json_object_object_get_ex(root, "stage_id",        &stage_obj);
        json_object_object_get_ex(root, "target_dir_name", &dir_obj);
        json_object_object_get_ex(root, "overwrite",       &over_obj);
        json_object_object_get_ex(root, "backup",          &bak_obj);
        json_object_object_get_ex(root, "target_user_id",  &user_obj);

        const char *stage_id = stage_obj ? json_object_get_string(stage_obj) : NULL;
        if (!stage_id || !stage_id[0]) {
            json_object_put(root);
            return send_400(res, "missing stage_id");
        }
        char zip_path[SAVESYNC_PATH_LEN];
        if (resolve_stage_path(stage_id, zip_path, sizeof(zip_path)) != 0) {
            json_object_put(root);
            return send_400(res, "invalid stage_id");
        }

        savesync_convert_options_t opts;
        savesync_convert_default_options(&opts);
        const char *dir_name = dir_obj ? json_object_get_string(dir_obj) : NULL;
        if (dir_name && dir_name[0]) {
            snprintf(opts.target_dir_name, sizeof(opts.target_dir_name),
                     "%s", dir_name);
        }
        opts.overwrite = over_obj ? json_object_get_boolean(over_obj) : 0;
        opts.backup    = bak_obj  ? json_object_get_boolean(bak_obj)  : 0;
        const char *uid = user_obj ? json_object_get_string(user_obj) : NULL;
        if (uid && strlen(uid) == 8) {
            memcpy(opts.target_user_id, uid, 8);
            opts.target_user_id[8] = '\0';
        }

        char job_id[SAVESYNC_JOB_ID_LEN];
        char label [SAVESYNC_JOB_LABEL_LEN];
        snprintf(label, sizeof(label), "Import %s",
                 opts.target_dir_name[0] ? opts.target_dir_name : stage_id);
        int enq = savesync_jobs_enqueue_import(zip_path, &opts, label, job_id);
        json_object_put(root);
        if (enq != 0)
            return send_json(res, 503, "Service Unavailable",
                              "{\"error\":\"job queue full\"}");

        char *body = savesync_json_job_id(job_id);
        if (!body) return send_500(res);
        int rc = send_json(res, 202, "Accepted", body);
        free(body);
        return rc;
    }

    /* export / convert kinds: act on an existing on-disk save. */
    struct json_object *save_id_obj = NULL, *direction_obj = NULL;
    json_object_object_get_ex(root, "save_id",   &save_id_obj);
    json_object_object_get_ex(root, "direction", &direction_obj);
    if (!save_id_obj) {
        json_object_put(root);
        return send_400(res, "missing save_id");
    }
    const char *save_id   = json_object_get_string(save_id_obj);
    const char *direction = direction_obj ? json_object_get_string(direction_obj) : NULL;
    if (!save_id) {
        json_object_put(root);
        return send_400(res, "null save_id");
    }

    savesync_save_list_t list;
    savesync_save_list_init(&list);
    savesync_scan_hdd(&list);
    savesync_scan_usb(&list);

    const savesync_save_t *found = NULL;
    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].dir_name, save_id) == 0) {
            found = &list.items[i];
            break;
        }
    }
    if (!found) {
        savesync_save_list_free(&list);
        json_object_put(root);
        return send_404(res);
    }

    char job_id[SAVESYNC_JOB_ID_LEN];
    char label [SAVESYNC_JOB_LABEL_LEN];
    int  enq = -1;
    if (strcmp(kind_str, "export") == 0) {
        snprintf(label, sizeof(label), "Export %s", found->dir_name);
        enq = savesync_jobs_enqueue_export(found->path, label, job_id);
    } else if (strcmp(kind_str, "convert") == 0) {
        savesync_convert_options_t opts;
        savesync_convert_default_options(&opts);
        opts.direction = (direction && strcmp(direction, "ps3-to-rpcs3") == 0)
            ? SAVESYNC_DIR_PS3_TO_RPCS3 : SAVESYNC_DIR_RPCS3_TO_PS3;
        snprintf(label, sizeof(label), "Convert %s", found->dir_name);
        enq = savesync_jobs_enqueue_convert(found->path, &opts, label, job_id);
    } else {
        savesync_save_list_free(&list);
        json_object_put(root);
        return send_400(res, "unknown kind");
    }
    savesync_save_list_free(&list);
    json_object_put(root);

    if (enq != 0)
        return send_json(res, 503, "Service Unavailable",
                          "{\"error\":\"job queue full\"}");

    char *body = savesync_json_job_id(job_id);
    if (!body) return send_500(res);
    int rc = send_json(res, 202, "Accepted", body);
    free(body);
    return rc;
}

static int handle_delete_job(ps3http_response_t *res, const char *job_id) {
    savesync_jobs_cancel(job_id);
    if (savesync_jobs_remove(job_id) != 0)
        return send_json(res, 404, "Not Found", "{\"error\":\"not found\"}");
    return send_json(res, 200, "OK", "{\"ok\":true}");
}

/* The headline fix: stream the zip from disk in 64 KB chunks, never holding
 * the whole file in memory. Old path malloc'd zip_size + (zip_size + 512)
 * back-to-back and fragmented the heap on the first multi-MB save export. */
static int handle_get_job_download(ps3http_response_t *res, const char *job_id)
{
    savesync_job_t job;
    if (savesync_jobs_get(job_id, &job) != 0) return send_404(res);
    if (job.state != SAVESYNC_JOB_DONE || job.zip_path[0] == '\0')
        return send_json(res, 404, "Not Found",
                          "{\"error\":\"no download available\"}");

    /* Filename: prefer the save dir basename (e.g. "BCUS98103_NDI...") so the
     * downloaded file is identifiable; fall back to job id only if missing. */
    char base[SAVESYNC_PATH_LEN];
    const char *slash = strrchr(job.source_dir, '/');
    const char *src   = slash ? slash + 1 : job.source_dir;
    if (src[0] == '\0') src = job_id;
    /* Sanitize: replace anything outside [A-Za-z0-9._-] with '_' so the
     * Content-Disposition value can't be truncated by a stray quote/CRLF. */
    size_t i = 0;
    for (; src[i] && i < sizeof(base) - 1; i++) {
        char c = src[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        base[i] = ok ? c : '_';
    }
    base[i] = '\0';

    char disp[256];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"%s.zip\"", base);
    ps3http_response_set_header(res, "Content-Disposition", disp);

    if (ps3http_response_send_file(res, job.zip_path, "application/zip") != 0)
        return send_404(res);
    return 0;
}

/* ----- /api/inspect ------------------------------------------------------ */

/* POST /api/inspect — multipart upload of an RPCS3-flavored zip. Stages it
 * to /dev_hdd0/tmp/savesync/staged-<id>.zip, peeks PARAM.SFO, and returns
 * { stage_id, title_id, title, subtitle, suggested_dir_name, account_id_hex }.
 * The client uses suggested_dir_name to seed the slot picker, then submits
 * POST /api/jobs { kind:"import", stage_id, target_dir_name, overwrite, backup }
 * to actually run the import. */
static int handle_post_inspect(ps3http_request_t *req, ps3http_response_t *res)
{
    const char *ct = ps3http_request_header(req, "Content-Type");
    if (!ct || !strstr(ct, "multipart/form-data"))
        return send_400(res, "expected multipart/form-data");

    sysFsMkdir(SAVESYNC_TMP_DIR, 0755);

    /* Stage id: bottom 32 bits of system-time microseconds, hex-encoded.
     * Good enough for identifying one upload across the lifetime of an app
     * run; the staged zip is consumed when the import job runs. */
    char stage_id[16];
    snprintf(stage_id, sizeof(stage_id), "%08lx",
             (unsigned long)(sysGetSystemTime() & 0xFFFFFFFFu));

    char zip_path[SAVESYNC_PATH_LEN];
    snprintf(zip_path, sizeof(zip_path), "%s/staged-%s.zip",
             SAVESYNC_TMP_DIR, stage_id);

    char filename[PS3HTTP_MULTIPART_FILENAME_MAX];
    if (ps3http_multipart_save_to_disk(req, "file", zip_path, filename) != 0)
        return send_400(res, "multipart parse failed");

    /* Find PARAM.SFO inside the staged zip and extract just it. */
    char root_in_zip[SAVESYNC_PATH_LEN];
    if (savesync_archive_find_save_root(zip_path, root_in_zip,
                                         sizeof(root_in_zip)) != 0) {
        remove(zip_path);
        return send_400(res, "no PARAM.SFO found in zip");
    }

    char sfo_entry[SAVESYNC_PATH_LEN];
    snprintf(sfo_entry, sizeof(sfo_entry), "%sPARAM.SFO", root_in_zip);

    char sfo_tmp[SAVESYNC_PATH_LEN];
    snprintf(sfo_tmp, sizeof(sfo_tmp), "%s/staged-%s.sfo",
             SAVESYNC_TMP_DIR, stage_id);

    if (savesync_archive_extract_one(zip_path, sfo_entry, sfo_tmp) != 0) {
        remove(zip_path);
        return send_400(res, "could not extract PARAM.SFO");
    }

    sfo_t sfo;
    sfo_init(&sfo);
    if (sfo_load(&sfo, sfo_tmp) != 0) {
        sfo_free(&sfo);
        remove(sfo_tmp);
        remove(zip_path);
        return send_400(res, "could not parse PARAM.SFO");
    }

    const char *title       = sfo_get_str(&sfo, "TITLE");
    const char *subtitle    = sfo_get_str(&sfo, "SUB_TITLE");
    const char *sfo_dir     = sfo_get_str(&sfo, "SAVEDATA_DIRECTORY");
    /* TITLE_ID is sometimes absent — fall back to deriving from SAVEDATA_DIRECTORY
     * (PS3 dir-name pattern: BLES01807-MYSAVE00 → first 9 chars). */
    const char *title_id    = sfo_get_str(&sfo, "TITLE_ID");
    char tid_buf[SAVESYNC_TITLE_ID_LEN];
    if ((!title_id || !title_id[0]) && sfo_dir && sfo_dir[0]) {
        size_t i = 0;
        for (; i < SAVESYNC_TITLE_ID_LEN - 1 && sfo_dir[i] && sfo_dir[i] != '-'; i++)
            tid_buf[i] = sfo_dir[i];
        tid_buf[i] = '\0';
        title_id = tid_buf;
    }

    char acct[17] = "";
    sfo_get_account_id(&sfo, acct);

    char *body = savesync_json_inspect_result(stage_id,
                                              title_id ? title_id : "",
                                              title ? title : "",
                                              subtitle ? subtitle : "",
                                              sfo_dir ? sfo_dir : "",
                                              acct);
    sfo_free(&sfo);
    remove(sfo_tmp);

    if (!body) { remove(zip_path); return send_500(res); }
    int rc = send_json(res, 200, "OK", body);
    free(body);
    return rc;
}

/* ----- /api/upload ------------------------------------------------------- */

static int handle_post_upload(ps3http_request_t *req, ps3http_response_t *res)
{
    const char *ct = ps3http_request_header(req, "Content-Type");
    if (!ct || !strstr(ct, "multipart/form-data"))
        return send_400(res, "expected multipart/form-data");

    sysFsMkdir(SAVESYNC_TMP_DIR, 0755);
    char upload_path[SAVESYNC_PATH_LEN];
    snprintf(upload_path, sizeof(upload_path), "%s/upload.zip", SAVESYNC_TMP_DIR);

    char filename[PS3HTTP_MULTIPART_FILENAME_MAX];
    if (ps3http_multipart_save_to_disk(req, "file", upload_path, filename) != 0)
        return send_400(res, "multipart parse failed");

    savesync_convert_options_t opts;
    savesync_convert_default_options(&opts);
    char label[SAVESYNC_JOB_LABEL_LEN];
    snprintf(label, sizeof(label), "Import %s",
             filename[0] ? filename : "upload.zip");

    char job_id[SAVESYNC_JOB_ID_LEN];
    if (savesync_jobs_enqueue_import(upload_path, &opts, label, job_id) != 0)
        return send_json(res, 503, "Service Unavailable",
                          "{\"error\":\"job queue full\"}");

    char *body = savesync_json_job_id(job_id);
    if (!body) return send_500(res);
    int rc = send_json(res, 202, "Accepted", body);
    free(body);
    return rc;
}

/* ----- Dispatcher -------------------------------------------------------- */

void savesync_routes_dispatch(ps3http_request_t *req,
                              ps3http_response_t *res,
                              void *user)
{
    (void)user;

    const char *method = ps3http_request_method(req);
    const char *path   = ps3http_request_path(req);
    if (!ps3http_path_is_safe(path)) { send_404(res); return; }

    /* Static SPA */
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        if (strcmp(method, "GET") != 0) { send_405(res); return; }
        handle_get_root(res); return;
    }
    if (strcmp(path, "/app.js") == 0) {
        if (strcmp(method, "GET") != 0) { send_405(res); return; }
        handle_get_appjs(res); return;
    }

    /* /api/status */
    if (strcmp(path, "/api/status") == 0) {
        if (strcmp(method, "GET") != 0) { send_405(res); return; }
        handle_get_status(res); return;
    }

    /* /api/quit — flag flips before send so even a hung response unblocks main loop. */
    if (strcmp(path, "/api/quit") == 0) {
        savesync_should_quit = 1;
        send_json(res, 200, "OK", "{\"quitting\":true}");
        return;
    }

    /* /api/saves [/<id>/icon] */
    if (strcmp(path, "/api/saves") == 0) {
        if (strcmp(method, "GET") != 0) { send_405(res); return; }
        handle_get_saves(res); return;
    }
    {
        char save_id[256];
        if (path_match_two(path, "/api/saves/", "/icon", save_id, sizeof(save_id))) {
            if (strcmp(method, "GET") != 0) { send_405(res); return; }
            handle_get_save_icon(res, save_id); return;
        }
    }

    /* /api/games — same data as /api/saves grouped by title_id */
    if (strcmp(path, "/api/games") == 0) {
        if (strcmp(method, "GET") != 0) { send_405(res); return; }
        handle_get_games(res); return;
    }

    /* /api/inspect — stage an upload and report SFO metadata */
    if (strcmp(path, "/api/inspect") == 0) {
        if (strcmp(method, "POST") != 0) { send_405(res); return; }
        handle_post_inspect(req, res); return;
    }

    /* /api/jobs [/<id>[/download]] */
    if (strcmp(path, "/api/jobs") == 0) {
        if (strcmp(method, "GET")  == 0) { handle_get_jobs(res);          return; }
        if (strcmp(method, "POST") == 0) { handle_post_jobs(req, res);    return; }
        send_405(res); return;
    }
    {
        char job_id[SAVESYNC_JOB_ID_LEN];
        if (path_match_two(path, "/api/jobs/", "/download", job_id, sizeof(job_id))) {
            if (strcmp(method, "GET") != 0) { send_405(res); return; }
            handle_get_job_download(res, job_id); return;
        }
        if (strncmp(path, "/api/jobs/", 10) == 0 && strchr(path + 10, '/') == NULL) {
            const char *id = path + 10;
            if (strcmp(method, "DELETE") != 0) { send_405(res); return; }
            handle_delete_job(res, id); return;
        }
    }

    /* /api/upload */
    if (strcmp(path, "/api/upload") == 0) {
        if (strcmp(method, "POST") != 0) { send_405(res); return; }
        handle_post_upload(req, res); return;
    }

    send_404(res);
}
