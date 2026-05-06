/*
 * ps3http_multipart.c — streaming multipart/form-data parser.
 *
 * Reads from the request body chunk-by-chunk and writes the matching file
 * field directly to disk while scanning for the boundary marker. No full-
 * body buffer is allocated at any point, so uploads are bounded only by
 * what the destination filesystem can hold (and the upload Content-Length
 * the caller's reverse proxy / browser will accept).
 *
 * Strategy: keep an 8 KB sliding window. After every fill, scan the window
 * for "\r\n--<boundary>" but reserve the last (boundary_len + 4) bytes from
 * the scan in case the boundary straddles the next chunk. Bytes before the
 * scan limit that aren't a match get flushed to disk (when in BODY state).
 */

#include "ps3http.h"
#include "ps3http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MP_BUF_SIZE       8192
#define MP_BOUNDARY_MAX   128

/* Scan haystack[0..hlen) for needle[0..nlen). Returns offset or -1. */
static ssize_t mem_find(const unsigned char *hay, size_t hlen,
                         const unsigned char *needle, size_t nlen)
{
    if (nlen == 0 || nlen > hlen) return -1;
    const unsigned char *end = hay + hlen - nlen;
    for (const unsigned char *p = hay; p <= end; p++) {
        if (*p == needle[0] && memcmp(p, needle, nlen) == 0)
            return (ssize_t)(p - hay);
    }
    return -1;
}

/* Pull boundary= token out of a Content-Type header value. */
static int extract_boundary(const char *content_type, char out[MP_BOUNDARY_MAX])
{
    const char *p = content_type;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (strncasecmp(p, "boundary=", 9) == 0) {
            p += 9;
            int quoted = (*p == '"');
            if (quoted) p++;
            size_t i = 0;
            while (*p && i < MP_BOUNDARY_MAX - 1) {
                if (quoted && *p == '"') break;
                if (!quoted && (*p == ';' || *p == ' ' || *p == '\r' || *p == '\n')) break;
                out[i++] = *p++;
            }
            out[i] = '\0';
            return i > 0 ? 0 : -1;
        }
        while (*p && *p != ';') p++;
    }
    return -1;
}

/* Pull a parameter value (e.g. name= or filename=) out of a header line. */
static int extract_param(const char *line, const char *param,
                         char *out, size_t out_cap)
{
    size_t plen = strlen(param);
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (strncasecmp(p, param, plen) == 0 && p[plen] == '=') {
            p += plen + 1;
            int quoted = (*p == '"');
            if (quoted) p++;
            size_t i = 0;
            while (*p && i < out_cap - 1) {
                if (quoted && *p == '"') break;
                if (!quoted && (*p == ';' || *p == ' ' || *p == '\r' || *p == '\n')) break;
                out[i++] = *p++;
            }
            out[i] = '\0';
            return 0;
        }
        while (*p && *p != ';') p++;
    }
    return -1;
}

/* Parse a part-header block (CRLF-separated lines). Sets *is_target = 1 if
 * the part is a file field whose name matches target_field (or any file
 * field if target_field is empty). Captures filename into out_filename. */
static void parse_part_headers(const unsigned char *hdr, size_t hlen,
                                const char *target_field,
                                int *is_target,
                                char *out_filename, size_t fn_cap)
{
    *is_target = 0;
    char *block = (char *)malloc(hlen + 1);
    if (!block) return;
    memcpy(block, hdr, hlen);
    block[hlen] = '\0';

    char *line = block;
    while (line && *line) {
        char *eol = strstr(line, "\r\n");
        if (eol) *eol = '\0';
        if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
            char name_val[64] = "";
            char fn_val[PS3HTTP_MULTIPART_FILENAME_MAX] = "";
            extract_param(line + 20, "name",     name_val, sizeof(name_val));
            extract_param(line + 20, "filename", fn_val,   sizeof(fn_val));

            int field_ok = (target_field == NULL || target_field[0] == '\0')
                            ? (fn_val[0] != '\0')                      /* any file field */
                            : (strcmp(name_val, target_field) == 0);
            if (field_ok && fn_val[0] != '\0') {
                *is_target = 1;
                size_t n = strlen(fn_val);
                if (n >= fn_cap) n = fn_cap - 1;
                memcpy(out_filename, fn_val, n);
                out_filename[n] = '\0';
            }
        }
        line = eol ? eol + 2 : NULL;
    }
    free(block);
}

/* Refill the window with body_read until full or end-of-body. */
static int refill(ps3http_request_t *req, unsigned char *buf, size_t *len, size_t cap)
{
    while (*len < cap) {
        ssize_t n = ps3http_request_body_read(req, buf + *len, cap - *len);
        if (n < 0) return -1;
        if (n == 0) return 0;
        *len += (size_t)n;
        /* One refill call per pass — let the state machine work between
         * reads so we don't starve on a tiny body. */
        return (int)n;
    }
    return (int)*len;
}

int ps3http_multipart_save_to_disk(ps3http_request_t *req,
                                   const char *field_name_or_null,
                                   const char *out_path,
                                   char out_filename[PS3HTTP_MULTIPART_FILENAME_MAX])
{
    out_filename[0] = '\0';

    const char *ct = ps3http_request_header(req, "Content-Type");
    if (!ct || !strstr(ct, "multipart/form-data")) return -1;

    char boundary[MP_BOUNDARY_MAX];
    if (extract_boundary(ct, boundary) != 0) return -1;

    char delim[MP_BOUNDARY_MAX + 4];
    int dlen = snprintf(delim, sizeof(delim), "--%s", boundary);
    if (dlen <= 2) return -1;

    char crlf_delim[MP_BOUNDARY_MAX + 6];
    int cdlen = snprintf(crlf_delim, sizeof(crlf_delim), "\r\n--%s", boundary);
    if (cdlen <= 4) return -1;

    enum { S_INIT, S_HEADERS, S_BODY, S_SKIP, S_DONE } st = S_INIT;
    unsigned char buf[MP_BUF_SIZE];
    size_t buf_len = 0;
    int    eof     = 0;

    FILE *out = NULL;
    char  filename[PS3HTTP_MULTIPART_FILENAME_MAX] = "";

    while (st != S_DONE) {
        if (!eof) {
            int n = refill(req, buf, &buf_len, sizeof(buf));
            if (n < 0) goto err;
            if (n == 0) eof = 1;
        }
        if (buf_len == 0 && eof) goto err;  /* truncated stream */

        if (st == S_INIT) {
            ssize_t pos = mem_find(buf, buf_len,
                                    (const unsigned char *)delim, (size_t)dlen);
            if (pos < 0) {
                /* Need more data; if buffer is full and we still didn't
                 * find the first boundary, the request is malformed. */
                if (buf_len >= sizeof(buf)) goto err;
                if (eof) goto err;
                continue;
            }
            size_t consumed = (size_t)pos + (size_t)dlen;
            if (consumed + 2 > buf_len) {
                /* Need more bytes to disambiguate "--" vs CRLF. */
                if (eof) goto err;
                if (consumed > sizeof(buf) / 2) {
                    memmove(buf, buf + pos, buf_len - (size_t)pos);
                    buf_len -= (size_t)pos;
                }
                continue;
            }
            if (buf[consumed] == '-' && buf[consumed + 1] == '-') {
                st = S_DONE; break; /* empty multipart */
            }
            if (buf[consumed] == '\r' && buf[consumed + 1] == '\n')
                consumed += 2;
            memmove(buf, buf + consumed, buf_len - consumed);
            buf_len -= consumed;
            st = S_HEADERS;
            continue;
        }

        if (st == S_HEADERS) {
            ssize_t pos = mem_find(buf, buf_len,
                                    (const unsigned char *)"\r\n\r\n", 4);
            if (pos < 0) {
                if (buf_len >= sizeof(buf)) goto err;
                if (eof) goto err;
                continue;
            }
            int is_target = 0;
            char fn[PS3HTTP_MULTIPART_FILENAME_MAX] = "";
            parse_part_headers(buf, (size_t)pos, field_name_or_null,
                                &is_target, fn, sizeof(fn));
            size_t consumed = (size_t)pos + 4;
            memmove(buf, buf + consumed, buf_len - consumed);
            buf_len -= consumed;

            if (is_target && !out) {
                out = fopen(out_path, "wb");
                if (!out) goto err;
                memcpy(filename, fn, sizeof(filename));
                st = S_BODY;
            } else {
                st = S_SKIP;
            }
            continue;
        }

        /* S_BODY or S_SKIP: scan for CRLF + boundary. */
        size_t scan_floor = (size_t)cdlen;       /* keep last cdlen bytes */
        size_t safe_len   = (buf_len > scan_floor) ? buf_len - scan_floor : 0;
        ssize_t pos = -1;
        if (buf_len >= (size_t)cdlen) {
            pos = mem_find(buf, buf_len,
                            (const unsigned char *)crlf_delim, (size_t)cdlen);
        }

        if (pos >= 0) {
            if (st == S_BODY) {
                if (fwrite(buf, 1, (size_t)pos, out) != (size_t)pos) goto err;
            }
            size_t consumed = (size_t)pos + (size_t)cdlen;
            /* Need 2 bytes after delim to read either "--" or CRLF. */
            if (consumed + 2 > buf_len) {
                if (eof) goto err;
                /* Compact what we already wrote (or skipped) so refill works. */
                memmove(buf, buf + (size_t)pos, buf_len - (size_t)pos);
                buf_len -= (size_t)pos;
                continue;
            }
            int is_last = (buf[consumed] == '-' && buf[consumed + 1] == '-');
            if (is_last) {
                if (out) { fclose(out); out = NULL; }
                st = S_DONE; break;
            }
            if (buf[consumed] == '\r' && buf[consumed + 1] == '\n')
                consumed += 2;
            memmove(buf, buf + consumed, buf_len - consumed);
            buf_len -= consumed;
            if (out) { fclose(out); out = NULL; }
            /* If we already captured the file we wanted, we could short-
             * circuit by setting S_DONE — but the request body may have
             * trailing parts and we should still drain it so the worker
             * doesn't return mid-stream and the client sees a clean close. */
            st = S_HEADERS;
            continue;
        }

        /* No boundary in current window. Flush all but the safety tail. */
        if (st == S_BODY && safe_len > 0) {
            if (fwrite(buf, 1, safe_len, out) != safe_len) goto err;
        }
        if (safe_len > 0) {
            memmove(buf, buf + safe_len, buf_len - safe_len);
            buf_len -= safe_len;
        }
        if (eof) goto err;     /* should have hit a boundary by now */
    }

    if (out) { fclose(out); out = NULL; }
    if (filename[0] == '\0') return -1; /* no matching file part */

    size_t n = strlen(filename);
    if (n >= PS3HTTP_MULTIPART_FILENAME_MAX) n = PS3HTTP_MULTIPART_FILENAME_MAX - 1;
    memcpy(out_filename, filename, n);
    out_filename[n] = '\0';
    return 0;

err:
    if (out) { fclose(out); remove(out_path); }
    return -1;
}
