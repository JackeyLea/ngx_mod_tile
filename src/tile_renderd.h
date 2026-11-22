/*
 * ngx_mod_tile - asynchronous renderd connection engine declarations.
 */

#ifndef NGX_TILE_RENDERD_H
#define NGX_TILE_RENDERD_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"

/**
 * @brief Completion outcome codes passed to ngx_http_tile_conn_done_pt.
 */
#define NGX_TILE_CONN_DONE     NGX_OK        /* cmdDone matched our request */
#define NGX_TILE_CONN_NOT_DONE NGX_DECLINED  /* notDone, timeout, EOF, mismatch */
#define NGX_TILE_CONN_ERROR    NGX_ERROR     /* fatal connection failure */

typedef struct ngx_http_tile_conn_s  ngx_http_tile_conn_t;
typedef void (*ngx_http_tile_conn_done_pt)(void *data, ngx_int_t rc);

struct ngx_http_tile_conn_s {
    uint32_t                    magic;      /* heap-corruption sentinel */
    void                       *data;       /* passed back to done() */
    ngx_http_tile_conn_done_pt  done;
    ngx_http_request_t         *r;          /* NULL for detached connections */
    ngx_pool_t                 *pool;       /* own pool for detached conns */

    ngx_peer_connection_t       peer;
    u_char                     *sockaddr;   /* resolved endpoint */
    socklen_t                   socklen;
    ngx_str_t                   name;       /* peer name for logging */

    struct protocol             cmd;
    struct protocol             resp;
    size_t                      nrecv;
    size_t                      want;
    size_t                      nsent;
    ngx_uint_t                  retries;    /* remaining reconnect attempts */

    ngx_msec_t                  connect_timeout;
    ngx_msec_t                  reply_timeout;

    unsigned                    wait_reply:1; /* 0 = fire-and-forget (dirty) */
    unsigned                    connecting:1; /* connect in progress */
    unsigned                    sent:1;
    unsigned                    destroyed:1;
};


ngx_int_t ngx_http_tile_conn_submit(ngx_http_tile_conn_t *tc);

#endif /* NGX_TILE_RENDERD_H */
