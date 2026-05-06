/*
 * ps3http_request.c — request line + header parsing, body access.
 *
 * The request reader is buffered: a single recv() pulls up to 8 KB into
 * r->recv_buf, the parser then scans that window for CRLF-delimited lines.
 * That avoids the byte-per-recv() pattern which is correct but cripplingly
 * slow on PSL1GHT's libnet (each recv crosses the lv2 boundary).
 *
 * Body access has two modes:
 *   - read_all_body  — pull the whole body into a malloc'd buffer (capped)
 *   - body_read      — stream chunks directly from the socket
 *
 * After the headers are parsed, recv_buf may already hold the first slice
 * of the body. body_read drains that first, then continues with raw recv().
 */

#include "ps3http.h"
#include "ps3http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include <net/net.h>

void ps3http__request_init(ps3http_request_t *req, int fd, int read_timeout_sec)
{
    memset(req, 0, sizeof(*req));
    req->fd               = fd;
    req->content_length   = -1;
    req->read_timeout_sec = read_timeout_sec;
}

void ps3http__request_free(ps3http_request_t *req)
{
    free(req->cached_body);
    req->cached_body = NULL;
    req->cached_len  = 0;
}

/* ----- Buffer plumbing ---------------------------------------------------- */

int ps3http__recv_more(ps3http_request_t *r)
{
    /* Compact the buffer if we've consumed any prefix. */
    if (r->recv_off > 0) {
        size_t live = r->recv_end - r->recv_off;
        if (live > 0) memmove(r->recv_buf, r->recv_buf + r->recv_off, live);
        r->recv_end = live;
        r->recv_off = 0;
    }
    if (r->recv_end >= sizeof(r->recv_buf)) return -1; /* line too long */

    /* Plain blocking recv — the ps3-libnet skill explicitly warns that
     * poll()-based timeouts during request parsing have caused full-system
     * lockups (kernel + webMAN frozen). Trust the LAN. */
    ssize_t got = recv(r->fd, r->recv_buf + r->recv_end,
                       sizeof(r->recv_buf) - r->recv_end, 0);
    if (got < 0) return -1;
    if (got == 0) return 0;
    r->recv_end += (size_t)got;
    return (int)got;
}

ssize_t ps3http__find_crlf(const ps3http_request_t *r)
{
    if (r->recv_end <= r->recv_off + 1) return -1;
    const unsigned char *p = r->recv_buf + r->recv_off;
    size_t n = r->recv_end - r->recv_off;
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] == '\r' && p[i+1] == '\n') return (ssize_t)i;
    }
    return -1;
}

/* Read a single CRLF-terminated line into out (NUL-terminated, no CRLF).
 * Returns line length, -1 on error, -2 if line exceeds out_cap-1. */
static int read_line(ps3http_request_t *r, char *out, size_t out_cap)
{
    for (;;) {
        ssize_t pos = ps3http__find_crlf(r);
        if (pos >= 0) {
            if ((size_t)pos >= out_cap) return -2;
            memcpy(out, r->recv_buf + r->recv_off, (size_t)pos);
            out[pos] = '\0';
            r->recv_off += (size_t)pos + 2; /* consume CRLF */
            return (int)pos;
        }
        /* No CRLF in current window; pull more. */
        size_t live = r->recv_end - r->recv_off;
        if (live >= out_cap) return -2;
        int rc = ps3http__recv_more(r);
        if (rc <= 0) return -1;
    }
}

/* ----- Parser ------------------------------------------------------------- */

int ps3http__parse_request(ps3http_request_t *r)
{
    char line[PS3HTTP_REQUEST_LINE_MAX];

    /* Request line: METHOD SP path SP HTTP/x.y */
    int ll = read_line(r, line, sizeof(line));
    if (ll <= 0) return -1;

    char *sp1 = strchr(line, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    char *path_start = sp1 + 1;
    char *sp2 = strchr(path_start, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';

    if (strlen(line) >= sizeof(r->method)) return -1;
    strcpy(r->method, line);

    /* Split path and query. */
    char *qmark = strchr(path_start, '?');
    if (qmark) {
        *qmark = '\0';
        size_t qlen = strlen(qmark + 1);
        if (qlen >= sizeof(r->query)) qlen = sizeof(r->query) - 1;
        memcpy(r->query, qmark + 1, qlen);
        r->query[qlen] = '\0';
    }
    if (strlen(path_start) >= sizeof(r->path)) return -1;
    strcpy(r->path, path_start);
    ps3http_url_decode(r->path);

    /* Headers until blank line. */
    while (r->header_count < PS3HTTP_MAX_HEADERS) {
        char hline[PS3HTTP_MAX_HEADER_NAME + PS3HTTP_MAX_HEADER_VALUE + 4];
        int n = read_line(r, hline, sizeof(hline));
        if (n < 0) return n == -2 ? -2 : -1;
        if (n == 0) break;

        char *colon = strchr(hline, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *vp = colon + 1;
        while (*vp == ' ' || *vp == '\t') vp++;

        size_t nlen = strlen(hline);
        size_t vlen = strlen(vp);
        if (nlen == 0 || nlen >= sizeof(r->headers[0].name)) continue;
        if (vlen >= sizeof(r->headers[0].value)) vlen = sizeof(r->headers[0].value) - 1;

        ps3http_header_t *h = &r->headers[r->header_count++];
        memcpy(h->name, hline, nlen + 1);
        memcpy(h->value, vp, vlen);
        h->value[vlen] = '\0';

        if (strcasecmp(h->name, "Content-Length") == 0) {
            r->content_length = (ssize_t)strtoll(h->value, NULL, 10);
        }
    }

    return 0;
}

/* ----- Public inspection -------------------------------------------------- */

const char *ps3http_request_method(const ps3http_request_t *r) { return r->method; }
const char *ps3http_request_path  (const ps3http_request_t *r) { return r->path;   }
const char *ps3http_request_query (const ps3http_request_t *r) { return r->query;  }

const char *ps3http_request_header(const ps3http_request_t *r, const char *name)
{
    for (int i = 0; i < r->header_count; i++) {
        if (strcasecmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    }
    return NULL;
}

ssize_t ps3http_request_content_length(const ps3http_request_t *r)
{
    return r->content_length;
}

/* ----- Path / URL helpers ------------------------------------------------- */

int ps3http_path_is_safe(const char *path)
{
    if (!path || path[0] != '/') return 0;
    /* Reject any ".." segment to block traversal. */
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '/'))
            return 0;
        while (*p && *p != '/') p++;
        if (*p == '/') p++;
    }
    return 1;
}

void ps3http_url_decode(char *s)
{
    char *w = s, *r = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)(int)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* ----- Body access -------------------------------------------------------- */

ssize_t ps3http_request_body_read(ps3http_request_t *r, void *buf, size_t cap)
{
    if (r->body_mode == 2) return -1;             /* already buffered */
    if (cap == 0) return 0;

    if (r->content_length >= 0 &&
        r->body_consumed >= (size_t)r->content_length)
        return 0;                                  /* clean EOF */

    r->body_mode = 1;

    /* First, drain whatever is left in recv_buf (the parser may have
     * over-read into the body). */
    size_t live = r->recv_end - r->recv_off;
    if (live > 0) {
        size_t take = live < cap ? live : cap;
        if (r->content_length >= 0) {
            size_t remaining = (size_t)r->content_length - r->body_consumed;
            if (take > remaining) take = remaining;
        }
        memcpy(buf, r->recv_buf + r->recv_off, take);
        r->recv_off += take;
        r->body_consumed += take;
        return (ssize_t)take;
    }

    /* No buffered bytes; recv directly into caller's buffer. */
    size_t want = cap;
    if (r->content_length >= 0) {
        size_t remaining = (size_t)r->content_length - r->body_consumed;
        if (want > remaining) want = remaining;
    }
    ssize_t got = recv(r->fd, buf, want, 0);
    if (got < 0) return -1;
    if (got == 0) return 0;
    r->body_consumed += (size_t)got;
    return got;
}

int ps3http_request_read_all_body(ps3http_request_t *r,
                                  const void **body, size_t *len)
{
    if (r->body_mode == 1) return -1;             /* already streaming */
    if (r->cached_body) {
        if (body) *body = r->cached_body;
        if (len)  *len  = r->cached_len;
        return 0;
    }

    /* If no Content-Length header, refuse — we don't support chunked
     * request bodies (no client we care about uses them). */
    if (r->content_length < 0) {
        if (body) *body = NULL;
        if (len)  *len  = 0;
        return 0;
    }
    /* We need access to the server's max_inmem_body cap. The cap is
     * enforced by the worker BEFORE calling the handler if Content-Length
     * is known; here we just allocate the requested size. */

    size_t total = (size_t)r->content_length;
    if (total == 0) {
        r->body_mode = 2;
        r->cached_body = NULL;
        r->cached_len  = 0;
        if (body) *body = NULL;
        if (len)  *len  = 0;
        return 0;
    }

    unsigned char *buf = (unsigned char *)malloc(total);
    if (!buf) return -1;

    size_t got = 0;
    while (got < total) {
        ssize_t n = ps3http_request_body_read(r, buf + got, total - got);
        if (n <= 0) { free(buf); return -1; }
        got += (size_t)n;
    }

    r->body_mode   = 2;
    r->cached_body = buf;
    r->cached_len  = total;
    if (body) *body = buf;
    if (len)  *len  = total;
    return 0;
}
