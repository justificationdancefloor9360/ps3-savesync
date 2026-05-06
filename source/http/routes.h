/*
 * routes.h — savesync HTTP route handlers.
 *
 * Exposes a single ps3http_handler_fn-shaped entry point that the
 * ps3http server invokes for every accepted connection.
 */

#ifndef SAVESYNC_ROUTES_H
#define SAVESYNC_ROUTES_H

#include "ps3http.h"

#ifdef __cplusplus
extern "C" {
#endif

void savesync_routes_dispatch(ps3http_request_t *req,
                              ps3http_response_t *res,
                              void *user);

#ifdef __cplusplus
}
#endif

#endif
