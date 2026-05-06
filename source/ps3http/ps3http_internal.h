/*
 * ps3http_internal.h — private types shared between the ps3http translation
 * units. Not exported.
 */

#ifndef PS3HTTP_INTERNAL_H
#define PS3HTTP_INTERNAL_H

#include "ps3http.h"

#include <stdint.h>
#include <stddef.h>
#include <sys/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3HTTP_MAX_METHOD_LEN     16
#define PS3HTTP_MAX_PATH_LEN       1024
#define PS3HTTP_MAX_QUERY_LEN      1024
#define PS3HTTP_MAX_HEADER_NAME    64
#define PS3HTTP_MAX_HEADER_VALUE   1024
#define PS3HTTP_MAX_HEADERS        32
#define PS3HTTP_REQUEST_LINE_MAX   (PS3HTTP_MAX_METHOD_LEN + PS3HTTP_MAX_PATH_LEN + PS3HTTP_MAX_QUERY_LEN + 32)
#define PS3HTTP_RECV_BUF_SIZE      8192
#define PS3HTTP_FILE_CHUNK         (64 * 1024)

typedef struct {
    char name [PS3HTTP_MAX_HEADER_NAME];
    char value[PS3HTTP_MAX_HEADER_VALUE];
} ps3http_header_t;

struct ps3http_request {
    int    fd;
    char   method[PS3HTTP_MAX_METHOD_LEN];
    char   path  [PS3HTTP_MAX_PATH_LEN];
    char   query [PS3HTTP_MAX_QUERY_LEN];

    ps3http_header_t headers[PS3HTTP_MAX_HEADERS];
    int              header_count;
    ssize_t          content_length;     /* -1 if absent */

    /* Buffered read window. Holds bytes pulled from the socket but not yet
     * consumed by line parsing or body reads. */
    unsigned char  recv_buf[PS3HTTP_RECV_BUF_SIZE];
    size_t         recv_off;             /* next unread byte */
    size_t         recv_end;             /* one past last filled byte */

    /* Body streaming state */
    size_t         body_consumed;        /* bytes of body returned to caller */
    int            body_mode;            /* 0=none, 1=streaming, 2=buffered */

    /* Buffered-body cache (mode 2). */
    unsigned char *cached_body;
    size_t         cached_len;

    int read_timeout_sec;
};

struct ps3http_response {
    int    fd;
    int    status;
    char   reason[64];

    ps3http_header_t extra[PS3HTTP_MAX_HEADERS];
    int              extra_count;

    int    headers_sent;     /* set after first write to socket */
    int    body_known_len;   /* 1 if Content-Length was sent */
    int    send_timeout_sec;

    /* If begin_stream was called with known_length, we track remaining for
     * a sanity check on end_stream. -1 if unknown / not applicable. */
    ssize_t pending_bytes;
};

struct ps3http_server {
    ps3http_config_t cfg;

    sys_ppu_thread_t listen_thread;
    int              listen_fd;       /* -1 when not listening */
    volatile int     stop_requested;
    volatile int     running;
    volatile int     active_workers;  /* atomic counter for cap */
};

/* Internal helpers, shared between TUs. */

/* Block until len bytes are sent or a hard error occurs. Honors send_timeout. */
int ps3http__send_all(int fd, const void *buf, size_t len, int timeout_sec);

/* Logging helper that no-ops if cfg.log is NULL. */
void ps3http__log(const ps3http_server_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Pull more bytes from socket into req->recv_buf. Returns bytes appended,
 * 0 on peer-close-clean, -1 on error. */
int ps3http__recv_more(ps3http_request_t *r);

/* Find CRLF in the buffered window starting at r->recv_off. Returns offset
 * relative to recv_off, or -1 if no CRLF in current window. */
ssize_t ps3http__find_crlf(const ps3http_request_t *r);

/* Read and parse the request line + headers into r. fd must be set on r
 * before calling. Returns 0, -1 on parse/IO error, -2 on too-large header. */
int ps3http__parse_request(ps3http_request_t *r);

/* Flush the deferred response status line + headers to the socket.
 * content_length < 0 means omit the header. Returns 0 / -1. */
int ps3http__flush_headers(ps3http_response_t *res,
                           const char *content_type,
                           ssize_t content_length);

/* Init a fresh response on top of a fd. */
void ps3http__response_init(ps3http_response_t *res, int fd, int send_timeout_sec);

/* Init a fresh request on top of a fd. */
void ps3http__request_init(ps3http_request_t *req, int fd, int read_timeout_sec);

/* Drop any cached body buffer. */
void ps3http__request_free(ps3http_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* PS3HTTP_INTERNAL_H */
