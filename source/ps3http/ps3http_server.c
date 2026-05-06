/*
 * ps3http_server.c — listen socket, per-connection worker dispatch, lifecycle.
 *
 * One PPU thread runs a blocking accept() loop on a non-reuseaddr listen
 * socket bound to 0.0.0.0:cfg.port. Each accepted connection is handed to
 * a fresh detached worker thread that parses the request, runs the user's
 * handler, and closes the socket. The accept thread caps concurrent workers
 * via a plain volatile counter — when at the cap, the new connection gets
 * an inline 503 and is closed before any worker is spawned. PSL1GHT lacks
 * sys_net_abort_socket, so stop() relies on netShutdown(listen_fd) to wake
 * the blocking accept(). Workers are detached and never joined on shutdown
 * because any one of them may be stuck in send() to a slow peer — joining
 * would wedge stop().
 */

#include "ps3http.h"
#include "ps3http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <net/net.h>
#include <netinet/in.h>
#include <sys/thread.h>
#include <sys/systime.h>

/* PSL1GHT's <net/socket.h> declares the BSD send/recv/shutdown wrappers
 * (which strip SOCKET_FD_MASK before calling the raw lv2 entry points)
 * but NOT closesocket — the symbol exists in libnet/socket.c. Declare it
 * locally so we can route through the wrapper instead of the raw netClose
 * (which gets EBADF when handed a masked fd from accept()). */
extern int closesocket(int socket);

#define LISTEN_BACKLOG          8
#define LISTEN_THREAD_STACK     (64 * 1024)
#define WORKER_THREAD_STACK     (256 * 1024)
#define WORKER_THREAD_PRIORITY  1500

void ps3http__log(const ps3http_server_t *s, const char *fmt, ...)
{
    if (!s->cfg.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s->cfg.log(buf, s->cfg.log_user);
}

/* ----- Worker thread ------------------------------------------------------ */

typedef struct {
    ps3http_server_t *server;
    int               connfd;
} worker_args_t;

static void worker_thread(void *arg)
{
    worker_args_t   *wa     = (worker_args_t *)arg;
    ps3http_server_t *s     = wa->server;
    int              connfd = wa->connfd;
    free(wa);

    ps3http_request_t  req;
    ps3http_response_t res;
    ps3http__request_init(&req, connfd, s->cfg.read_timeout_sec);
    ps3http__response_init(&res, connfd, s->cfg.send_timeout_sec);

    int prc = ps3http__parse_request(&req);
    if (prc == 0) {
        ps3http__log(s, "[ps3http] %s %s", req.method, req.path);
        if (s->cfg.handler) {
            s->cfg.handler(&req, &res, s->cfg.handler_user);
        }
        if (!res.headers_sent) {
            ps3http_response_set_status(&res, 500, "Internal Server Error");
            ps3http_response_send_text(&res, "application/json",
                                        "{\"error\":\"handler did not respond\"}");
        }
    } else if (prc == -2) {
        ps3http__log(s, "[ps3http] 431 header too large");
        ps3http_response_set_status(&res, 431, "Request Header Fields Too Large");
        ps3http_response_send_text(&res, "application/json",
                                    "{\"error\":\"header too large\"}");
    } else {
        ps3http__log(s, "[ps3http] parse rc=%d (peer dropped?)", prc);
    }

    ps3http__request_free(&req);

    /* Graceful close. Switch to non-blocking via SO_NBIO so the drain loop
     * can't pin the worker. shutdown(SHUT_WR) sends FIN; the drain reads
     * any pipelined bytes the peer queued; closesocket releases the fd.
     * Critically these are the BSD wrappers — netShutdown/netClose are the
     * raw lv2 exports that don't strip SOCKET_FD_MASK from accept()'s fd
     * and silently fail with EBADF, leaving the connection half-open and
     * Chrome/Safari spinning forever waiting for FIN. */
    int nb = 1;
    setsockopt(connfd, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));
    shutdown(connfd, SHUT_WR);
    {
        unsigned char drain[512];
        for (int i = 0; i < 32; i++) {
            ssize_t n = recv(connfd, drain, sizeof(drain), 0);
            if (n <= 0) break;
        }
    }
    closesocket(connfd);

    s->active_workers--;
    sysThreadExit(0);
}

/* ----- Listen thread ------------------------------------------------------ */

static int reject_too_busy(int connfd, int send_timeout_sec)
{
    static const char body[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: 21\r\n\r\n"
        "{\"error\":\"too busy\"}";
    int rc = ps3http__send_all(connfd, body, sizeof(body) - 1, send_timeout_sec);
    shutdown(connfd, SHUT_RDWR);
    closesocket(connfd);
    return rc;
}

static void listen_thread(void *arg)
{
    ps3http_server_t *s = (ps3http_server_t *)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { ps3http__log(s, "socket() failed"); s->running = 0; sysThreadExit(1); return; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(s->cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ps3http__log(s, "bind(:%u) failed", (unsigned)s->cfg.port);
        shutdown(fd, SHUT_RDWR); closesocket(fd);
        s->running = 0; sysThreadExit(1); return;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        ps3http__log(s, "listen() failed");
        shutdown(fd, SHUT_RDWR); closesocket(fd);
        s->running = 0; sysThreadExit(1); return;
    }
    s->listen_fd = fd;
    ps3http__log(s, "listening on :%u", (unsigned)s->cfg.port);

    while (!s->stop_requested) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int connfd = accept(fd, (struct sockaddr *)&cli, &clen);
        if (connfd < 0) {
            if (s->stop_requested) break;
            ps3http__log(s, "accept() = %d, retrying", connfd);
            sysUsleep(500 * 1000);
            continue;
        }

        /* Cap concurrent workers. The counter is volatile but the increment
         * happens only on this thread, so there's no race against itself.
         * Workers decrement on exit; a stale read here at worst rejects a
         * request that could have been served. */
        if (s->active_workers >= s->cfg.max_workers) {
            ps3http__log(s, "worker cap (%d) reached; sending 503",
                         s->cfg.max_workers);
            reject_too_busy(connfd, s->cfg.send_timeout_sec);
            continue;
        }

        worker_args_t *wa = (worker_args_t *)malloc(sizeof(*wa));
        if (!wa) {
            ps3http__log(s, "worker_args alloc failed");
            shutdown(connfd, SHUT_RDWR); closesocket(connfd);
            continue;
        }
        wa->server = s;
        wa->connfd = connfd;

        s->active_workers++;
        sys_ppu_thread_t wt;
        int wrc = sysThreadCreate(&wt, worker_thread, wa,
                                  WORKER_THREAD_PRIORITY, WORKER_THREAD_STACK,
                                  THREAD_JOINABLE, (char *)"ps3http_w");
        if (wrc != 0) {
            ps3http__log(s, "sysThreadCreate failed rc=%d", wrc);
            s->active_workers--;
            free(wa);
            shutdown(connfd, SHUT_RDWR); closesocket(connfd);
        } else {
            sysThreadDetach(wt);
        }
    }

    ps3http__log(s, "listen thread exiting");
    if (s->listen_fd >= 0) {
        shutdown(s->listen_fd, SHUT_RDWR);
        closesocket(s->listen_fd);
        s->listen_fd = -1;
    }
    s->running = 0;
    sysThreadExit(0);
}

/* ----- Public API --------------------------------------------------------- */

ps3http_server_t *ps3http_server_new(const ps3http_config_t *cfg)
{
    if (!cfg || cfg->port == 0 || !cfg->handler) return NULL;
    ps3http_server_t *s = (ps3http_server_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    if (s->cfg.max_workers      <= 0) s->cfg.max_workers      = 8;
    if (s->cfg.max_inmem_body   == 0) s->cfg.max_inmem_body   = 1 * 1024 * 1024;
    if (s->cfg.read_timeout_sec <= 0) s->cfg.read_timeout_sec = 30;
    if (s->cfg.send_timeout_sec <= 0) s->cfg.send_timeout_sec = 30;
    s->listen_fd = -1;
    return s;
}

int ps3http_server_start(ps3http_server_t *s)
{
    if (!s || s->running) return -1;
    s->stop_requested = 0;
    s->running        = 1;
    int rc = sysThreadCreate(&s->listen_thread, listen_thread, s,
                             WORKER_THREAD_PRIORITY, LISTEN_THREAD_STACK,
                             THREAD_JOINABLE, (char *)"ps3http_listen");
    if (rc != 0) {
        s->running = 0;
        ps3http__log(s, "sysThreadCreate(listen) failed rc=%d", rc);
        return -1;
    }
    return 0;
}

void ps3http_server_stop(ps3http_server_t *s)
{
    if (!s || !s->running) return;
    s->stop_requested = 1;
    if (s->listen_fd >= 0) {
        /* Wake the blocking accept(). The listen thread will close+null
         * out the fd before exiting; setting it here avoids a double-close
         * if stop is called twice. */
        shutdown(s->listen_fd, SHUT_RDWR);
    }
    /* Don't sysThreadJoin — workers may be stuck mid-send to a slow peer.
     * We rely on process exit to reap them. */
}

void ps3http_server_free(ps3http_server_t *s)
{
    if (!s) return;
    /* Caller is responsible for stopping first. We don't free if workers
     * are still alive — that would yank cfg out from under them. */
    if (s->running) return;
    free(s);
}
