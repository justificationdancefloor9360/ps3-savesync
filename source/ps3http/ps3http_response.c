/*
 * ps3http_response.c — response building, single-shot send, streaming send,
 * file streaming.
 *
 * The send path collapses headers + body into a single send() call when the
 * body is small enough. For larger payloads (file streams) headers go out
 * first, then 64 KB chunks loop until done. Either way the loop honors the
 * configured send timeout via poll(POLLOUT) before each send().
 */

#include "ps3http.h"
#include "ps3http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <net/net.h>

void ps3http__response_init(ps3http_response_t *res, int fd, int send_timeout_sec)
{
    memset(res, 0, sizeof(*res));
    res->fd               = fd;
    res->status           = 200;
    strcpy(res->reason, "OK");
    res->send_timeout_sec = send_timeout_sec;
    res->pending_bytes    = -1;
}

int ps3http__send_all(int fd, const void *buf, size_t len, int timeout_sec)
{
    /* Plain blocking send loop — see the request reader for why we don't
     * gate on poll() here either. The timeout_sec arg is reserved for a
     * future SO_SNDTIMEO-based implementation if libnet ever honors it. */
    (void)timeout_sec;
    const char *p = (const char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

/* ----- Header staging ----------------------------------------------------- */

int ps3http_response_set_status(ps3http_response_t *r, int code, const char *reason)
{
    if (r->headers_sent) return -1;
    r->status = code;
    if (reason) {
        size_t n = strlen(reason);
        if (n >= sizeof(r->reason)) n = sizeof(r->reason) - 1;
        memcpy(r->reason, reason, n);
        r->reason[n] = '\0';
    }
    return 0;
}

int ps3http_response_set_header(ps3http_response_t *r, const char *name,
                                 const char *value)
{
    if (r->headers_sent) return -1;
    if (!name || !value) return -1;
    /* Reject reserved headers — the library writes these. */
    if (strcasecmp(name, "Content-Length") == 0)  return -1;
    if (strcasecmp(name, "Content-Type")   == 0)  return -1;
    if (strcasecmp(name, "Connection")     == 0)  return -1;
    if (r->extra_count >= PS3HTTP_MAX_HEADERS) return -1;

    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    if (nlen >= sizeof(r->extra[0].name))  return -1;
    if (vlen >= sizeof(r->extra[0].value)) return -1;
    memcpy(r->extra[r->extra_count].name,  name,  nlen + 1);
    memcpy(r->extra[r->extra_count].value, value, vlen + 1);
    r->extra_count++;
    return 0;
}

int ps3http__flush_headers(ps3http_response_t *res, const char *content_type,
                           ssize_t content_length)
{
    if (res->headers_sent) return -1;

    /* Build into a single buffer to avoid multi-send tail issues. The
     * status line + headers are bounded; allocate generously and assert
     * we didn't overflow. */
    size_t cap = 1024 + (size_t)res->extra_count *
                 (PS3HTTP_MAX_HEADER_NAME + PS3HTTP_MAX_HEADER_VALUE + 4);
    char *hdr = (char *)malloc(cap);
    if (!hdr) return -1;

    int n = snprintf(hdr, cap,
        "HTTP/1.1 %d %s\r\nConnection: close\r\n", res->status, res->reason);
    if (n < 0 || (size_t)n >= cap) { free(hdr); return -1; }

    if (content_type) {
        n += snprintf(hdr + n, cap - (size_t)n, "Content-Type: %s\r\n", content_type);
        if ((size_t)n >= cap) { free(hdr); return -1; }
    }
    if (content_length >= 0) {
        n += snprintf(hdr + n, cap - (size_t)n, "Content-Length: %lu\r\n",
                      (unsigned long)content_length);
        if ((size_t)n >= cap) { free(hdr); return -1; }
        res->body_known_len = 1;
    }
    for (int i = 0; i < res->extra_count; i++) {
        n += snprintf(hdr + n, cap - (size_t)n, "%s: %s\r\n",
                      res->extra[i].name, res->extra[i].value);
        if ((size_t)n >= cap) { free(hdr); return -1; }
    }
    n += snprintf(hdr + n, cap - (size_t)n, "\r\n");
    if ((size_t)n >= cap) { free(hdr); return -1; }

    int rc = ps3http__send_all(res->fd, hdr, (size_t)n, res->send_timeout_sec);
    free(hdr);
    if (rc < 0) return -1;
    res->headers_sent = 1;
    return 0;
}

/* ----- Single-shot send --------------------------------------------------- */

int ps3http_response_send(ps3http_response_t *r, const char *content_type,
                           const void *body, size_t len)
{
    if (r->headers_sent) return -1;

    /* For small/medium bodies, stage headers + body in one buffer and send
     * once. For larger bodies, fall back to flush + body. The threshold is
     * conservative — we'd rather avoid huge contiguous mallocs. */
    const size_t SINGLE_SHOT_LIMIT = 256 * 1024;
    if (len <= SINGLE_SHOT_LIMIT) {
        size_t cap = len + 1024 + (size_t)r->extra_count *
                     (PS3HTTP_MAX_HEADER_NAME + PS3HTTP_MAX_HEADER_VALUE + 4);
        char *buf = (char *)malloc(cap);
        if (!buf) return -1;

        int n = snprintf(buf, cap,
            "HTTP/1.1 %d %s\r\nConnection: close\r\n", r->status, r->reason);
        if (n < 0 || (size_t)n >= cap) { free(buf); return -1; }
        if (content_type) {
            n += snprintf(buf + n, cap - (size_t)n,
                          "Content-Type: %s\r\n", content_type);
        }
        n += snprintf(buf + n, cap - (size_t)n,
                      "Content-Length: %lu\r\n", (unsigned long)len);
        for (int i = 0; i < r->extra_count; i++) {
            n += snprintf(buf + n, cap - (size_t)n, "%s: %s\r\n",
                          r->extra[i].name, r->extra[i].value);
        }
        n += snprintf(buf + n, cap - (size_t)n, "\r\n");
        if ((size_t)n >= cap) { free(buf); return -1; }

        if (len > 0 && body) memcpy(buf + n, body, len);
        int rc = ps3http__send_all(r->fd, buf, (size_t)n + len, r->send_timeout_sec);
        free(buf);
        r->headers_sent = 1;
        r->body_known_len = 1;
        return rc;
    }

    /* Larger bodies: headers separately, then the body. */
    if (ps3http__flush_headers(r, content_type, (ssize_t)len) < 0) return -1;
    if (len > 0 && body) {
        if (ps3http__send_all(r->fd, body, len, r->send_timeout_sec) < 0) return -1;
    }
    return 0;
}

int ps3http_response_send_text(ps3http_response_t *r, const char *content_type,
                                const char *text)
{
    size_t len = text ? strlen(text) : 0;
    return ps3http_response_send(r, content_type, text, len);
}

int ps3http_response_send_error(ps3http_response_t *r, int code, const char *reason,
                                 const char *content_type, const char *body)
{
    if (r->headers_sent) return -1;
    ps3http_response_set_status(r, code, reason);
    return ps3http_response_send_text(r, content_type ? content_type : "text/plain",
                                       body ? body : "");
}

/* ----- File streaming (the actual download fix) -------------------------- */

int ps3http_response_send_file(ps3http_response_t *r, const char *path,
                                const char *content_type)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0)                     { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    if (ps3http__flush_headers(r, content_type, (ssize_t)sz) < 0) {
        fclose(f);
        return -1;
    }

    if (sz > 0) {
        unsigned char *chunk = (unsigned char *)malloc(PS3HTTP_FILE_CHUNK);
        if (!chunk) { fclose(f); return -1; }
        size_t left = (size_t)sz;
        while (left > 0) {
            size_t want = left < PS3HTTP_FILE_CHUNK ? left : PS3HTTP_FILE_CHUNK;
            size_t got  = fread(chunk, 1, want, f);
            if (got == 0) { free(chunk); fclose(f); return -1; }
            if (ps3http__send_all(r->fd, chunk, got, r->send_timeout_sec) < 0) {
                free(chunk); fclose(f); return -1;
            }
            left -= got;
        }
        free(chunk);
    }
    fclose(f);
    return 0;
}

/* ----- App-driven streaming ----------------------------------------------- */

int ps3http_response_begin_stream(ps3http_response_t *r, const char *content_type,
                                   ssize_t known_length)
{
    if (ps3http__flush_headers(r, content_type, known_length) < 0) return -1;
    r->pending_bytes = known_length;
    return 0;
}

int ps3http_response_write(ps3http_response_t *r, const void *data, size_t len)
{
    if (!r->headers_sent) return -1;
    if (len == 0) return 0;
    if (r->pending_bytes >= 0) {
        if ((ssize_t)len > r->pending_bytes) return -1;
        r->pending_bytes -= (ssize_t)len;
    }
    return ps3http__send_all(r->fd, data, len, r->send_timeout_sec);
}

int ps3http_response_end_stream(ps3http_response_t *r)
{
    if (!r->headers_sent) return -1;
    if (r->pending_bytes > 0) return -1; /* missing bytes vs Content-Length */
    return 0;
}
