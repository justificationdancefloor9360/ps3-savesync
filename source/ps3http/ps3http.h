/*
 * ps3http — small, streaming HTTP/1.1 server for PSL1GHT homebrew.
 *
 * Built around three things PSL1GHT's libnet does badly when ignored:
 *   - send() loops on a closed peer can hang forever (no SO_SNDTIMEO)
 *   - one stuck handler on a single-threaded server kills accept()
 *   - large in-memory response bodies fragment the heap fast (PS3 user-mode
 *     processes have ~200 MB and homebrew shares it with everything else)
 *
 * Design choices that follow:
 *   - one accept thread, one detached worker thread per connection (capped)
 *   - Connection: close on every response, no keep-alive, no chunked encoding
 *     (Content-Length when known; close-as-EOF for unknown-length streams)
 *   - response_send_file() streams 64 KB chunks from disk, never malloc()s
 *     the whole body
 *   - request body can be read incrementally so multipart uploads of multi-MB
 *     payloads stream through to disk
 *
 * Drop the source/ps3http/ directory into another PSL1GHT project, link
 * -lnet -lsysmodule -lrt -llv2 -lio, and call ps3http_server_new(); no other
 * dependencies on this project.
 */

#ifndef PS3HTTP_H
#define PS3HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>      /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3http_server   ps3http_server_t;
typedef struct ps3http_request  ps3http_request_t;
typedef struct ps3http_response ps3http_response_t;

/* The handler is invoked once per accepted connection from a worker thread.
 * The handler must call exactly one of: ps3http_response_send,
 * ps3http_response_send_text, ps3http_response_send_file, or
 * ps3http_response_end_stream (after begin_stream + write*).
 * If the handler returns without sending, the library sends a 500 error. */
typedef void (*ps3http_handler_fn)(ps3http_request_t *req,
                                   ps3http_response_t *res,
                                   void *user);

/* Optional log callback. Called from any thread; the implementation must be
 * thread-safe. msg is NUL-terminated, no trailing newline. */
typedef void (*ps3http_log_fn)(const char *msg, void *user);

typedef struct {
    uint16_t           port;             /* required, > 0 */
    int                max_workers;      /* concurrent connection workers; 0 = 8 */
    size_t             max_inmem_body;   /* cap for read_all_body; 0 = 1 MiB */
    int                read_timeout_sec; /* per-recv timeout; 0 = 30 */
    int                send_timeout_sec; /* per-send timeout; 0 = 30 */
    ps3http_handler_fn handler;          /* required */
    void              *handler_user;
    ps3http_log_fn     log;              /* optional; NULL silences */
    void              *log_user;
} ps3http_config_t;

/* Lifecycle. ps3http_server_start spawns the listen thread and returns
 * once bind+listen succeed. ps3http_server_stop is idempotent and safe
 * to call from any thread. ps3http_server_free joins nothing (workers are
 * detached) but releases the server struct; call after stop. */
ps3http_server_t *ps3http_server_new  (const ps3http_config_t *cfg);
int               ps3http_server_start(ps3http_server_t *s);
void              ps3http_server_stop (ps3http_server_t *s);
void              ps3http_server_free (ps3http_server_t *s);

/* ----- Request inspection ------------------------------------------------- */

const char *ps3http_request_method        (const ps3http_request_t *r);
const char *ps3http_request_path          (const ps3http_request_t *r);
const char *ps3http_request_query         (const ps3http_request_t *r); /* "" if none */
const char *ps3http_request_header        (const ps3http_request_t *r,
                                            const char *name);   /* NULL if absent */
ssize_t     ps3http_request_content_length(const ps3http_request_t *r); /* -1 if absent */

/* True if path contains no ".." segment and is absolute. */
int  ps3http_path_is_safe(const char *path);

/* In-place URL-decode (s is mutated). */
void ps3http_url_decode (char *s);

/* ----- Request body ------------------------------------------------------- */

/* Read the entire body into a buffer owned by the request. Capped at
 * config.max_inmem_body. Subsequent calls return the same buffer.
 * Returns 0 on success, -1 on I/O error, -2 if the body exceeds the cap. */
int ps3http_request_read_all_body(ps3http_request_t *r,
                                  const void **body, size_t *len);

/* Read the next chunk of the body directly from the socket. Use this for
 * uploads larger than max_inmem_body so they can stream to disk.
 * Returns bytes read (0 = end of body, -1 = error). After the first
 * read_all_body call you cannot also stream — pick one. */
ssize_t ps3http_request_body_read(ps3http_request_t *r,
                                  void *buf, size_t cap);

/* ----- Response ----------------------------------------------------------- */

/* Default status is 200 OK. set_status/set_header must be called BEFORE any
 * send/begin_stream call. Setting Content-Length manually is illegal — the
 * library writes it. Content-Type is set by the send call, not via set_header. */
int ps3http_response_set_status(ps3http_response_t *r, int code, const char *reason);
int ps3http_response_set_header(ps3http_response_t *r, const char *name,
                                 const char *value);

/* Single-shot send. body may be NULL when len == 0. Builds headers + body in
 * one allocation and one send() call. Best for responses up to a few hundred
 * KB; for anything larger prefer send_file or begin_stream. */
int ps3http_response_send(ps3http_response_t *r, const char *content_type,
                           const void *body, size_t len);

/* Convenience for NUL-terminated strings (JSON, HTML, etc.). */
int ps3http_response_send_text(ps3http_response_t *r, const char *content_type,
                                const char *text);

/* Stream a file from disk to the client in 64 KB chunks. Sets Content-Length
 * from the file size. Returns -1 on open/read/send failure (after which the
 * connection is unusable; the worker will close it). */
int ps3http_response_send_file(ps3http_response_t *r, const char *path,
                                const char *content_type);

/* App-driven streaming. known_length >= 0 sends Content-Length and frames the
 * body precisely; known_length < 0 omits Content-Length and the client reads
 * until the connection closes. Call write() any number of times, then end. */
int ps3http_response_begin_stream(ps3http_response_t *r, const char *content_type,
                                   ssize_t known_length);
int ps3http_response_write       (ps3http_response_t *r, const void *data, size_t len);
int ps3http_response_end_stream  (ps3http_response_t *r);

/* Convenience error reply. body may be NULL. */
int ps3http_response_send_error(ps3http_response_t *r, int code, const char *reason,
                                 const char *content_type, const char *body);

/* ----- Streaming multipart/form-data --------------------------------------
 * Reads a single file field directly from the request body to disk while
 * scanning for the boundary. No full-body buffer is allocated: works on
 * uploads as large as the disk can hold. The field name to capture is
 * "file" by default; pass NULL to accept the first file field encountered. */

#define PS3HTTP_MULTIPART_FILENAME_MAX 256

int ps3http_multipart_save_to_disk(ps3http_request_t *r,
                                   const char *field_name_or_null,
                                   const char *out_path,
                                   char out_filename[PS3HTTP_MULTIPART_FILENAME_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* PS3HTTP_H */
