/*
 * archive.c — ZIP pack/unpack for savesync using libzip 0.9.x API.
 *
 * libzip 0.9 uses struct zip / struct zip_file (opaque), zip_add() for
 * adding files (not zip_file_add()), and zip_get_num_files() (not
 * zip_get_num_entries()).  All of these match the header shipped in
 * /opt/ps3dev/portlibs/ppu/include/zip.h.
 */

#include "archive.h"
#include "savedata.h"  /* SAVESYNC_PATH_LEN — found via VPATH */

#include <zip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Join dir + "/" + name safely.  Both components clipped to PATH_HALF. */
#define PATH_HALF (SAVESYNC_PATH_LEN / 2 - 2)
static void pjoin(char *out, size_t outsz, const char *dir, const char *name) {
    char d[PATH_HALF + 1], n[PATH_HALF + 1];
    strncpy(d, dir,  PATH_HALF); d[PATH_HALF] = '\0';
    strncpy(n, name, PATH_HALF); n[PATH_HALF] = '\0';
    snprintf(out, outsz, "%s/%s", d, n);
}
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * Internal: mkdir -p for a single path component chain.
 * Only creates the leaf; assumes the parent exists.
 * --------------------------------------------------------------------- */
static int mkdir_p(const char *path) {
    char tmp[SAVESYNC_PATH_LEN];
    char *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return 0;

    /* strip trailing slash */
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
    return 0;
}

/* -----------------------------------------------------------------------
 * Internal: return total bytes of regular files in a directory
 * (non-recursive — saves are flat).
 * --------------------------------------------------------------------- */
static uint64_t dir_total_bytes(const char *dir) {
    DIR *d = opendir(dir);
    uint64_t total = 0;
    struct dirent *de;

    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char p[SAVESYNC_PATH_LEN];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        pjoin(p, sizeof(p), dir, de->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

/* -----------------------------------------------------------------------
 * savesync_archive_zip_dir
 * --------------------------------------------------------------------- */
int savesync_archive_zip_dir(const char *src_dir,
                             const char *out_zip_path,
                             savesync_archive_progress_cb cb,
                             void *user) {
    int err = 0;
    struct zip *za = NULL;
    DIR *d = NULL;
    struct dirent *de;

    uint64_t bytes_total = dir_total_bytes(src_dir);
    uint64_t bytes_done  = 0;

    /* Remove existing zip so ZIP_CREATE starts fresh */
    remove(out_zip_path);

    za = zip_open(out_zip_path, ZIP_CREATE, &err);
    if (!za) return -1;

    d = opendir(src_dir);
    if (!d) { zip_close(za); remove(out_zip_path); return -1; }

    while ((de = readdir(d)) != NULL) {
        char fpath[SAVESYNC_PATH_LEN];
        struct stat st;
        struct zip_source *zs;

        if (de->d_name[0] == '.') continue;
        /* Skip PARAM.PFD — caller handles PFD lifecycle separately */
        if (strcasecmp(de->d_name, "PARAM.PFD") == 0) continue;

        pjoin(fpath, sizeof(fpath), src_dir, de->d_name);
        if (stat(fpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        /* zip_source_file: add entire file (offset=0, length=-1) */
        zs = zip_source_file(za, fpath, 0, -1);
        if (!zs) { closedir(d); zip_close(za); remove(out_zip_path); return -1; }

        if (zip_add(za, de->d_name, zs) < 0) {
            zip_source_free(zs);
            closedir(d);
            zip_close(za);
            remove(out_zip_path);
            return -1;
        }

        bytes_done += (uint64_t)st.st_size;
        if (cb) cb(de->d_name, bytes_done, bytes_total, user);
    }
    closedir(d);

    /* zip_close commits the archive to disk */
    if (zip_close(za) < 0) return -1;

    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_archive_unzip_to
 * --------------------------------------------------------------------- */
int savesync_archive_unzip_to(const char *in_zip_path,
                              const char *dest_dir,
                              savesync_archive_progress_cb cb,
                              void *user) {
    int err = 0;
    struct zip *za;
    int n, i;
    uint64_t bytes_total = 0;
    uint64_t bytes_done  = 0;

    za = zip_open(in_zip_path, 0, &err);
    if (!za) return -1;

    n = zip_get_num_files(za);
    if (n < 0) { zip_close(za); return -1; }

    /* First pass: accumulate total for progress */
    {
        struct zip_stat zst;
        for (i = 0; i < n; i++) {
            zip_stat_init(&zst);
            if (zip_stat_index(za, i, 0, &zst) == 0)
                bytes_total += (uint64_t)zst.size;
        }
    }

    mkdir_p(dest_dir);

    /* Second pass: extract */
    for (i = 0; i < n; i++) {
        struct zip_stat zst;
        struct zip_file *zf;
        char out_path[SAVESYNC_PATH_LEN];
        const char *entry_name;
        char dir_part[SAVESYNC_PATH_LEN];
        char *slash;

        zip_stat_init(&zst);
        if (zip_stat_index(za, i, 0, &zst) < 0) continue;

        entry_name = zst.name;
        if (!entry_name || entry_name[0] == '\0') continue;

        snprintf(out_path, sizeof(out_path), "%s/%s", dest_dir, entry_name);

        /* If entry is a directory (trailing slash), just mkdir and continue */
        {
            size_t elen = strlen(entry_name);
            if (entry_name[elen - 1] == '/') {
                mkdir_p(out_path);
                continue;
            }
        }

        /* Ensure parent directory exists */
        snprintf(dir_part, sizeof(dir_part), "%s", out_path);
        slash = strrchr(dir_part, '/');
        if (slash && slash != dir_part) {
            *slash = '\0';
            mkdir_p(dir_part);
        }

        zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;

        {
            FILE *fout = fopen(out_path, "wb");
            if (!fout) { zip_fclose(zf); zip_close(za); return -1; }

            {
                uint8_t buf[8192];
                ssize_t nr;
                const char *basename = strrchr(entry_name, '/');
                basename = basename ? basename + 1 : entry_name;

                while ((nr = zip_fread(zf, buf, sizeof(buf))) > 0) {
                    fwrite(buf, 1, (size_t)nr, fout);
                    bytes_done += (uint64_t)nr;
                }
                fclose(fout);
                if (cb) cb(basename, bytes_done, bytes_total, user);
            }
        }
        zip_fclose(zf);
    }

    zip_close(za);
    return 0;
}

/* -----------------------------------------------------------------------
 * savesync_archive_find_save_root
 * --------------------------------------------------------------------- */
int savesync_archive_find_save_root(const char *in_zip_path,
                                    char *out_root_in_zip,
                                    size_t out_size) {
    int err = 0;
    struct zip *za;
    int n, i;

    if (!out_root_in_zip || out_size == 0) return -1;
    out_root_in_zip[0] = '\0';

    za = zip_open(in_zip_path, 0, &err);
    if (!za) return -1;

    n = zip_get_num_files(za);

    for (i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (!name) continue;

        /* Look for an entry whose basename is PARAM.SFO */
        const char *basename = strrchr(name, '/');
        if (basename) {
            /* e.g. "BLES01807-SAVE00/PARAM.SFO" — basename is "/PARAM.SFO" */
            if (strcasecmp(basename + 1, "PARAM.SFO") == 0) {
                /* The root prefix is everything up to and including the slash */
                size_t prefix_len = (size_t)(basename - name) + 1;
                if (prefix_len >= out_size) prefix_len = out_size - 1;
                memcpy(out_root_in_zip, name, prefix_len);
                out_root_in_zip[prefix_len] = '\0';
                zip_close(za);
                return 0;
            }
        } else {
            /* Flat: "PARAM.SFO" at zip root */
            if (strcasecmp(name, "PARAM.SFO") == 0) {
                out_root_in_zip[0] = '\0';
                zip_close(za);
                return 0;
            }
        }
    }

    zip_close(za);
    return -1;  /* PARAM.SFO not found */
}

/* -----------------------------------------------------------------------
 * savesync_archive_extract_one
 * --------------------------------------------------------------------- */
int savesync_archive_extract_one(const char *in_zip_path,
                                 const char *entry_name,
                                 const char *out_path) {
    int err = 0;
    struct zip *za;
    struct zip_file *zf;
    FILE *fout;
    uint8_t buf[8192];
    ssize_t nr;

    if (!in_zip_path || !entry_name || !out_path) return -1;

    za = zip_open(in_zip_path, 0, &err);
    if (!za) return -1;

    zf = zip_fopen(za, entry_name, 0);
    if (!zf) { zip_close(za); return -1; }

    fout = fopen(out_path, "wb");
    if (!fout) { zip_fclose(zf); zip_close(za); return -1; }

    while ((nr = zip_fread(zf, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)nr, fout);
    }
    fclose(fout);
    zip_fclose(zf);
    zip_close(za);
    return 0;
}
