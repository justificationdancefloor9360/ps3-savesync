/*
 * convert.c — Save conversion orchestration for savesync.
 *
 * Coordinates: SFO patching → PFD signing → zip archive operations.
 * Error handling: cleanup-goto pattern; every function that allocates
 * or opens a resource has a single cleanup label at the bottom.
 */

#include "convert.h"
#include "savedata.h"
#include "sfo.h"
#include "pfd.h"
#include "archive.h"
#include "savesync.h"

#include <json-c/json.h>
#include <zip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

/* Join dir + "/" + name safely.  Both inputs clipped to PATH_HALF chars. */
#define PATH_HALF (SAVESYNC_PATH_LEN / 2 - 2)
static void pjoin(char *out, size_t outsz, const char *dir, const char *name) {
    char d[PATH_HALF + 1], n[PATH_HALF + 1];
    strncpy(d, dir,  PATH_HALF); d[PATH_HALF] = '\0';
    strncpy(n, name, PATH_HALF); n[PATH_HALF] = '\0';
    snprintf(out, outsz, "%s/%s", d, n);
}

/* Count regular files (excluding dotfiles and PARAM.PFD) and the sum of
 * their sizes — used to populate progress.{files,bytes}_total before any
 * work begins, so the UI's percent calculation has a denominator. */
static void count_save_dir(const char *dir,
                           uint64_t *out_files, uint64_t *out_bytes) {
    DIR *d = opendir(dir);
    struct dirent *de;
    *out_files = 0;
    *out_bytes = 0;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char p[SAVESYNC_PATH_LEN];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        if (strcasecmp(de->d_name, "PARAM.PFD") == 0) continue;
        pjoin(p, sizeof(p), dir, de->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        (*out_files)++;
        *out_bytes += (uint64_t)st.st_size;
    }
    closedir(d);
}

/* -----------------------------------------------------------------------
 * Internal progress helpers
 * --------------------------------------------------------------------- */
static void progress_phase(savesync_progress_cb cb, void *user,
                            savesync_progress_t *p,
                            savesync_phase_t phase,
                            const char *file) {
    if (!cb) return;
    p->phase = phase;
    if (file) {
        strncpy(p->current_file, file, sizeof(p->current_file) - 1);
        p->current_file[sizeof(p->current_file) - 1] = '\0';
    }
    cb(p, user);
}

static void progress_fail(savesync_progress_cb cb, void *user,
                          savesync_progress_t *p,
                          int code, const char *msg) {
    if (!cb) return;
    p->phase      = SAVESYNC_PHASE_FAILED;
    p->error_code = code;
    snprintf(p->error_message, sizeof(p->error_message), "%s", msg ? msg : "");
    cb(p, user);
}

/* -----------------------------------------------------------------------
 * Auto-detect the highest-numbered user directory under /dev_hdd0/home/.
 * Writes the 8-digit string (e.g. "00000001") into out (9 bytes).
 * Returns 0 on success, -1 if no user dirs found.
 * --------------------------------------------------------------------- */
static int find_highest_user(char out[9]) {
    DIR *d = opendir("/dev_hdd0/home");
    struct dirent *de;
    unsigned long best = 0;
    int found = 0;

    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        char *end;
        unsigned long v;
        int len;

        if (de->d_name[0] == '.') continue;
        len = (int)strlen(de->d_name);
        if (len != 8) continue;

        /* All 8 chars must be digits */
        {
            int ok = 1, k;
            for (k = 0; k < 8; k++) {
                if (de->d_name[k] < '0' || de->d_name[k] > '9') { ok = 0; break; }
            }
            if (!ok) continue;
        }

        v = strtoul(de->d_name, &end, 10);
        if (end == de->d_name) continue;

        if (!found || v > best) {
            best = v;
            found = 1;
        }
    }
    closedir(d);

    if (!found) return -1;
    snprintf(out, 9, "%08lu", best);
    return 0;
}

/* -----------------------------------------------------------------------
 * Wipe a flat save directory: unlink every regular file inside, then rmdir.
 * Saves are non-recursive (PS3 layout is flat per-dir), so a one-level
 * sweep is enough. On error, returns -1 but best-effort completes the rest.
 * --------------------------------------------------------------------- */
static int clear_save_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int err = 0;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char p[SAVESYNC_PATH_LEN];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        pjoin(p, sizeof(p), dir, de->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            if (unlink(p) != 0) err = -1;
        }
    }
    closedir(d);
    return err;
}

/* -----------------------------------------------------------------------
 * mkdir for a path, creating intermediate components.
 * --------------------------------------------------------------------- */
static void mkdirs(const char *path) {
    char tmp[SAVESYNC_PATH_LEN];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

/* -----------------------------------------------------------------------
 * Copy a single file src → dst.  Returns 0 on success.
 * --------------------------------------------------------------------- */
static int copy_file(const char *src, const char *dst) {
    uint8_t buf[8192];
    FILE *in  = fopen(src, "rb");
    FILE *out = NULL;
    int ret   = -1;
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) goto done;
    }
    ret = 0;

done:
    fclose(in);
    fclose(out);
    return ret;
}

/* -----------------------------------------------------------------------
 * Copy all regular files from src_dir into dst_dir.
 * Fires cb with COPY phase once per file.
 * Returns 0 on success, count of copy errors on partial failure.
 * --------------------------------------------------------------------- */
static int copy_dir_files(const char *src_dir, const char *dst_dir,
                          savesync_progress_cb cb, void *user,
                          savesync_progress_t *p) {
    DIR *d = opendir(src_dir);
    struct dirent *de;
    int errors = 0;

    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        char src[SAVESYNC_PATH_LEN], dst[SAVESYNC_PATH_LEN];
        struct stat st;

        if (de->d_name[0] == '.') continue;

        pjoin(src, sizeof(src), src_dir, de->d_name);
        pjoin(dst, sizeof(dst), dst_dir, de->d_name);

        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        progress_phase(cb, user, p, SAVESYNC_PHASE_COPY, de->d_name);

        if (copy_file(src, dst) < 0) errors++;
        else {
            p->files_done++;
            p->bytes_done += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return errors;
}

/* -----------------------------------------------------------------------
 * Write a savesync.json manifest entry into a zip archive.
 * Uses json-c to build the object; embeds it via zip_source_buffer.
 * The buffer must outlive zip_close() — we alloc and leak intentionally
 * (tiny, one-shot).
 * --------------------------------------------------------------------- */
static int zip_add_manifest(struct zip *za,
                             const char *title_id,
                             const char *title) {
    struct json_object *root;
    const char *json_str;
    char *buf;
    size_t len;
    struct zip_source *zs;
    time_t now = time(NULL);

    root = json_object_new_object();
    if (!root) return -1;

    json_object_object_add(root, "title_id",
                           json_object_new_string(title_id ? title_id : ""));
    json_object_object_add(root, "title",
                           json_object_new_string(title ? title : ""));
    json_object_object_add(root, "exported_from",
                           json_object_new_string("ps3-savesync v" SAVESYNC_VERSION));
    json_object_object_add(root, "timestamp",
                           json_object_new_int64((int64_t)now));

    json_str = json_object_to_json_string(root);
    len = strlen(json_str);

    /* Persistent copy — zip_source_buffer holds a pointer, not a copy */
    buf = (char *)malloc(len + 1);
    if (!buf) { json_object_put(root); return -1; }
    memcpy(buf, json_str, len + 1);
    json_object_put(root);

    zs = zip_source_buffer(za, buf, (off_t)len, 0);
    if (!zs) { free(buf); return -1; }

    if (zip_add(za, "savesync.json", zs) < 0) {
        zip_source_free(zs);
        free(buf);
        return -1;
    }
    /* buf is intentionally not freed — zip_source_buffer holds it until
     * zip_close().  zip_source_buffer with freep=0 does NOT free it,
     * so we must keep it alive.  Acceptable: exactly one manifest per export. */
    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_convert_default_options
 * --------------------------------------------------------------------- */
void savesync_convert_default_options(savesync_convert_options_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->direction    = SAVESYNC_DIR_RPCS3_TO_PS3;
    out->cross_account = 1;
}

/* -----------------------------------------------------------------------
 * savesync_convert_dir
 * --------------------------------------------------------------------- */
int savesync_convert_dir(const char *save_dir,
                         const savesync_convert_options_t *opts,
                         savesync_progress_cb cb, void *user) {
    savesync_progress_t p;
    sfo_t sfo;
    char sfo_path[SAVESYNC_PATH_LEN];
    int ret = -1;

    memset(&p, 0, sizeof(p));
    sfo_init(&sfo);

    progress_phase(cb, user, &p, SAVESYNC_PHASE_PREPARE, NULL);

    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_dir);

    /* ---- PS3 → RPCS3 ---- */
    if (opts->direction == SAVESYNC_DIR_PS3_TO_RPCS3) {
        /* Step 1: strip PFD */
        progress_phase(cb, user, &p, SAVESYNC_PHASE_SIGN_PFD, "PARAM.PFD");
        if (savesync_pfd_strip(save_dir) < 0) {
            progress_fail(cb, user, &p, -1, "failed to strip PARAM.PFD");
            goto done;
        }

        /* Step 2: patch SFO */
        progress_phase(cb, user, &p, SAVESYNC_PHASE_TRANSFORM_SFO, "PARAM.SFO");
        if (sfo_load(&sfo, sfo_path) < 0) {
            progress_fail(cb, user, &p, -2, "failed to load PARAM.SFO");
            goto done;
        }

        /* clear_subtitle_marker: placeholder, no known RPCS3 prefix today */

        /* Always clear ACCOUNT_ID when converting to RPCS3 — RPCS3 ignores
         * it but it avoids leaking the source account binding. */
        sfo_set_account_id_zero(&sfo);

        if (sfo_save(&sfo, sfo_path) < 0) {
            progress_fail(cb, user, &p, -3, "failed to save PARAM.SFO");
            goto done;
        }

        ret = 0;
        progress_phase(cb, user, &p, SAVESYNC_PHASE_DONE, NULL);
        goto done;
    }

    /* ---- RPCS3 → PS3 ---- */
    {
        savesync_pfd_options_t pfd_opts;

        /* Step 1: load SFO and sanitize for lv2.
         *
         * Native PS3 saves don't necessarily emit TITLE_ID (GT6's own does
         * not), so don't gate on it — savesync's scanner already derives
         * the title id from the dir name. RPCS3 SFOs do carry extra entries
         * (per-file '*' protection flags + "RPCS3_BLIST") that lv2 doesn't
         * understand; strip them before re-saving. */
        progress_phase(cb, user, &p, SAVESYNC_PHASE_TRANSFORM_SFO, "PARAM.SFO");
        if (sfo_load(&sfo, sfo_path) < 0) {
            progress_fail(cb, user, &p, -2, "failed to load PARAM.SFO");
            goto done;
        }

        sfo_strip_rpcs3_specific(&sfo);

        /* Step 2: patch ATTRIBUTE and ACCOUNT_ID */
        if (opts->cross_account) {
            sfo_set_int(&sfo, "ATTRIBUTE", 0);
            sfo_set_account_id_zero(&sfo);
        }

        if (sfo_save(&sfo, sfo_path) < 0) {
            progress_fail(cb, user, &p, -3, "failed to save PARAM.SFO");
            goto done;
        }

        /* Step 3: build PFD */
        progress_phase(cb, user, &p, SAVESYNC_PHASE_SIGN_PFD, "PARAM.PFD");

        savesync_pfd_default_options(&pfd_opts);
        pfd_opts.cross_account = opts->cross_account;
        if (!opts->cross_account && opts->target_user_id[0]) {
            /* copy 8 chars of user_id as raw bytes */
            memcpy(pfd_opts.user_id, opts->target_user_id, 8);
            pfd_opts.user_id[8] = '\0';
        }

        if (savesync_pfd_build(save_dir, &pfd_opts) < 0) {
            progress_fail(cb, user, &p, -5, "failed to build PARAM.PFD");
            goto done;
        }

        ret = 0;
        progress_phase(cb, user, &p, SAVESYNC_PHASE_DONE, NULL);
    }

done:
    sfo_free(&sfo);
    return ret;
}

/* -----------------------------------------------------------------------
 * savesync_export_zip
 * --------------------------------------------------------------------- */
int savesync_export_zip(const char *save_dir,
                        const char *out_zip_path,
                        savesync_progress_cb cb, void *user) {
    savesync_progress_t p;
    sfo_t sfo;
    char sfo_path[SAVESYNC_PATH_LEN];
    const char *title_id = NULL;
    const char *title    = NULL;
    int err = 0;
    struct zip *za = NULL;
    DIR *d = NULL;
    int ret = -1;

    memset(&p, 0, sizeof(p));
    sfo_init(&sfo);

    /* Pre-count totals so the UI can show a real percent. The savesync.json
     * manifest is added below but doesn't count toward bytes_total — it
     * comes out of memory, not from the source dir. */
    count_save_dir(save_dir, &p.files_total, &p.bytes_total);

    progress_phase(cb, user, &p, SAVESYNC_PHASE_PREPARE, NULL);

    /* Load SFO metadata for the manifest */
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_dir);
    if (sfo_load(&sfo, sfo_path) == 0) {
        title_id = sfo_get_str(&sfo, "TITLE_ID");
        title    = sfo_get_str(&sfo, "TITLE");
    }

    /* Ensure output directory exists */
    {
        char out_dir[SAVESYNC_PATH_LEN];
        snprintf(out_dir, sizeof(out_dir), "%s", out_zip_path);
        char *slash = strrchr(out_dir, '/');
        if (slash) { *slash = '\0'; mkdirs(out_dir); }
    }

    remove(out_zip_path);

    progress_phase(cb, user, &p, SAVESYNC_PHASE_ARCHIVE, NULL);

    za = zip_open(out_zip_path, ZIP_CREATE, &err);
    if (!za) {
        progress_fail(cb, user, &p, -1, "failed to create zip");
        ret = -1;
        goto done;
    }

    /* Add savesync.json manifest */
    if (zip_add_manifest(za, title_id, title) < 0) {
        /* Non-fatal — continue without manifest */
    }

    /* Add all regular files except PARAM.PFD */
    d = opendir(save_dir);
    if (!d) {
        zip_close(za); za = NULL;
        remove(out_zip_path);
        progress_fail(cb, user, &p, -2, "failed to open save dir");
        goto done;
    }

    {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char fpath[SAVESYNC_PATH_LEN];
            struct stat st;
            struct zip_source *zs;

            if (de->d_name[0] == '.') continue;
            if (strcasecmp(de->d_name, "PARAM.PFD") == 0) continue;

            pjoin(fpath, sizeof(fpath), save_dir, de->d_name);
            if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            zs = zip_source_file(za, fpath, 0, -1);
            if (!zs) continue;

            if (zip_add(za, de->d_name, zs) < 0) {
                zip_source_free(zs);
                closedir(d); d = NULL;
                zip_close(za); za = NULL;
                remove(out_zip_path);
                progress_fail(cb, user, &p, -3, "failed to add file to zip");
                goto done;
            }

            p.files_done++;
            p.bytes_done += (uint64_t)st.st_size;
            if (cb) {
                strncpy(p.current_file, de->d_name, sizeof(p.current_file) - 1);
                p.current_file[sizeof(p.current_file) - 1] = '\0';
                cb(&p, user);
            }
        }
    }
    closedir(d);
    d = NULL;

    if (zip_close(za) < 0) {
        progress_fail(cb, user, &p, -4, "failed to finalize zip");
        goto done;
    }
    za = NULL;

    ret = 0;
    progress_phase(cb, user, &p, SAVESYNC_PHASE_DONE, NULL);

done:
    if (d)  closedir(d);
    if (za) { zip_close(za); remove(out_zip_path); }
    sfo_free(&sfo);
    return ret;
}

/* -----------------------------------------------------------------------
 * savesync_import_zip
 * --------------------------------------------------------------------- */
int savesync_import_zip(const char *in_zip_path,
                        const savesync_convert_options_t *opts,
                        savesync_progress_cb cb, void *user) {
    savesync_progress_t p;
    savesync_convert_options_t copts;
    char staging_dir[SAVESYNC_PATH_LEN];
    char save_root_in_zip[SAVESYNC_PATH_LEN];
    char extracted_save[SAVESYNC_PATH_LEN];
    char final_path[SAVESYNC_PATH_LEN];
    char user_id[9];
    int ret = -1;

    memset(&p, 0, sizeof(p));
    memset(save_root_in_zip, 0, sizeof(save_root_in_zip));

    /* Copy opts (we may need to mutate target_user_id) */
    if (opts) {
        copts = *opts;
    } else {
        savesync_convert_default_options(&copts);
    }
    copts.direction = SAVESYNC_DIR_RPCS3_TO_PS3;

    progress_phase(cb, user, &p, SAVESYNC_PHASE_PREPARE, NULL);

    /* Resolve target user */
    memset(user_id, 0, sizeof(user_id));
    if (copts.target_user_id[0]) {
        memcpy(user_id, copts.target_user_id, 8);
        user_id[8] = '\0';
    } else {
        if (find_highest_user(user_id) < 0) {
            progress_fail(cb, user, &p, -1, "no user dir found under /dev_hdd0/home");
            return -1;
        }
        memcpy(copts.target_user_id, user_id, 9);
    }

    /* Build staging path using zip path hash (cheap uniqueness) */
    {
        unsigned long h = 5381;
        const char *c;
        for (c = in_zip_path; *c; c++) h = ((h << 5) + h) + (unsigned char)*c;
        snprintf(staging_dir, sizeof(staging_dir),
                 "/dev_hdd0/tmp/savesync/staging/%08lx", h & 0xFFFFFFFF);
    }
    mkdirs(staging_dir);

    /* Detect save root inside zip */
    if (savesync_archive_find_save_root(in_zip_path,
                                        save_root_in_zip,
                                        sizeof(save_root_in_zip)) < 0) {
        progress_fail(cb, user, &p, -2, "no PARAM.SFO found in zip");
        return -1;
    }

    /* Extract zip to staging dir */
    progress_phase(cb, user, &p, SAVESYNC_PHASE_COPY, NULL);
    if (savesync_archive_unzip_to(in_zip_path, staging_dir, NULL, NULL) < 0) {
        progress_fail(cb, user, &p, -3, "failed to extract zip");
        return -1;
    }

    /* Compute the actual save directory inside staging */
    if (save_root_in_zip[0]) {
        /* Strip trailing slash from root prefix to get directory name */
        char root_dir[SAVESYNC_PATH_LEN];
        size_t rlen;
        snprintf(root_dir, sizeof(root_dir), "%s", save_root_in_zip);
        rlen = strlen(root_dir);
        if (rlen > 0 && root_dir[rlen - 1] == '/')
            root_dir[rlen - 1] = '\0';
        snprintf(extracted_save, sizeof(extracted_save),
                 "%s/%s", staging_dir, root_dir);
    } else {
        snprintf(extracted_save, sizeof(extracted_save), "%s", staging_dir);
    }

    /* Determine the save dir_name for the final HDD path.
     * Precedence: opts->target_dir_name > SFO's SAVEDATA_DIRECTORY > basename
     * of extracted_save. The target_dir_name path is what the web UI's slot
     * picker drives — user picks an existing slot or types a new one. */
    {
        char dir_name[SAVESYNC_DIR_NAME_LEN];
        sfo_t sfo;
        char sfo_path[SAVESYNC_PATH_LEN];
        const char *sfo_dir;

        sfo_init(&sfo);
        snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", extracted_save);

        dir_name[0] = '\0';
        if (copts.target_dir_name[0]) {
            snprintf(dir_name, sizeof(dir_name), "%s", copts.target_dir_name);
        } else if (sfo_load(&sfo, sfo_path) == 0) {
            sfo_dir = sfo_get_str(&sfo, "SAVEDATA_DIRECTORY");
            if (sfo_dir && sfo_dir[0])
                snprintf(dir_name, sizeof(dir_name), "%s", sfo_dir);
            sfo_free(&sfo);
        }

        /* Fallback: use the last component of extracted_save path */
        if (!dir_name[0]) {
            const char *slash = strrchr(extracted_save, '/');
            snprintf(dir_name, sizeof(dir_name), "%s",
                     slash ? slash + 1 : extracted_save);
        }

        snprintf(final_path, sizeof(final_path),
                 "/dev_hdd0/home/%s/savedata/%s", user_id, dir_name);
    }

    /* Slot-already-exists handling. PFD-signed PS3 saves are not safely
     * mergeable with a fresh sign pass, so we never blend old+new files —
     * the existing slot is either backed up, fully wiped, or refused. */
    {
        struct stat st;
        if (stat(final_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (copts.backup) {
                char backup_path[SAVESYNC_PATH_LEN];
                snprintf(backup_path, sizeof(backup_path),
                         "%s.bak.%lld", final_path, (long long)time(NULL));
                if (rename(final_path, backup_path) != 0) {
                    progress_fail(cb, user, &p, -6,
                                  "failed to back up existing slot");
                    goto done;
                }
            } else if (copts.overwrite) {
                if (clear_save_dir(final_path) != 0) {
                    progress_fail(cb, user, &p, -6,
                                  "failed to clear existing slot");
                    goto done;
                }
            } else {
                progress_fail(cb, user, &p, -6,
                              "target slot already exists");
                goto done;
            }
        }
    }

    /* Copy staging → final */
    mkdirs(final_path);
    if (copy_dir_files(extracted_save, final_path, cb, user, &p) < 0) {
        progress_fail(cb, user, &p, -4, "failed to copy files to HDD");
        goto done;
    }

    /* Convert in-place (RPCS3→PS3) */
    if (savesync_convert_dir(final_path, &copts, cb, user) < 0) {
        /* convert_dir already fired FAILED callback */
        goto done;
    }

    ret = 0;

done:
    return ret;
}
