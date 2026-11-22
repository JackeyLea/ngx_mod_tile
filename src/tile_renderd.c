/*
 * ngx_mod_tile - asynchronous renderd connection engine (Scheme B core).
 *
 * nginx workers are single-threaded and event-driven, so nothing here may
 * block. Every renderd exchange runs as an event-driven state machine:
 *
 *   resolve endpoint -> ngx_event_connect_peer() (non-blocking connect)
 *     -> send struct protocol (v2: 61B / v3: 143B, EAGAIN resumes on write)
 *     -> [wait_reply] accumulate reply, discover version from first int,
 *        deadline timer, verify x/y/z/xmlname, cmdDone/notDone
 *
 * Used in two flavours:
 *   - request-tied (ctx->conn): submission suspends the request
 *     (r->main->count++ + NGX_DONE); completion resumes the tile flow.
 *   - detached fire-and-forget (cmdDirty): lives in its own pool,
 *     survives the request that triggered it.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <netdb.h>
#include <sys/un.h>

#include "ngx_mod_tile.h"
#include "tile_renderd.h"

static void ngx_http_tile_conn_close(ngx_http_tile_conn_t *tc);
static void ngx_http_tile_conn_finish(ngx_http_tile_conn_t *tc,
    ngx_int_t rc);
static ngx_int_t ngx_http_tile_conn_send(ngx_http_tile_conn_t *tc);
static ngx_int_t ngx_http_tile_conn_reply_read(ngx_http_tile_conn_t *tc);

static void ngx_http_tile_conn_write_handler(ngx_event_t *ev);
static void ngx_http_tile_conn_read_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_tile_conn_test_connect(ngx_connection_t *c);

static ngx_int_t tile_peer_get(ngx_peer_connection_t *pc, void *data);
static void tile_peer_free(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state);

/* ------------------------------------------------------------------ */
/* submission                                                          */
/* ------------------------------------------------------------------ */

static ngx_int_t
tile_peer_get(ngx_peer_connection_t *pc, void *data)
{
    return NGX_OK;
}

static void
tile_peer_free(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    /* connections are closed explicitly by the engine */
}

ngx_int_t
ngx_http_tile_conn_submit(ngx_http_tile_conn_t *tc)
{
    ngx_int_t rc;

    tc->nrecv = 0;
    tc->nsent = 0;
    tc->want = sizeof(struct protocol_v2);

    tc->peer.get = tile_peer_get;
    tc->peer.free = tile_peer_free;
    tc->peer.data = tc;
    tc->peer.sockaddr = (struct sockaddr *) tc->sockaddr;
    tc->peer.socklen = tc->socklen;
    tc->peer.type = SOCK_STREAM;
    tc->peer.rcvbuf = 0;
    tc->peer.log = tc->r ? tc->r->connection->log : tc->pool->log;
    tc->peer.log_error = NGX_ERROR_ERR;
    tc->peer.name = &tc->name;

    rc = ngx_event_connect_peer(&tc->peer);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        /* pc->connection already closed by ngx_event_connect_peer */
        ngx_log_error(NGX_LOG_WARN, tc->peer.log, 0,
                      "tile: failed to connect to renderd (%V)", &tc->name);
        return rc;
    }

    if (tc->peer.connection == NULL) {
        return NGX_ERROR;
    }

    tc->peer.connection->data = tc;
    tc->peer.connection->log = tc->peer.log;
    tc->peer.connection->read->log = tc->peer.log;
    tc->peer.connection->write->log = tc->peer.log;
    tc->peer.connection->read->handler = ngx_http_tile_conn_read_handler;
    tc->peer.connection->write->handler = ngx_http_tile_conn_write_handler;

    if (rc == NGX_AGAIN) {
        /* non-blocking connect in progress */
        tc->connecting = 1;
        ngx_add_timer(tc->peer.connection->write, tc->connect_timeout);
        return NGX_OK;
    }

    /* connected immediately (typical for unix sockets) */
    return ngx_http_tile_conn_send(tc);
}

/* ------------------------------------------------------------------ */
/* endpoint resolution helpers (used by callers to prepare tc)         */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_conn_prepare(ngx_http_tile_conn_t *tc,
    ngx_http_tile_loc_conf_t *tlcf)
{
    struct sockaddr_un *sun;
    ngx_pool_t         *pool;

    pool = tc->r ? tc->r->pool : tc->pool;

    if (tlcf->renderd_port <= 0) {
        sun = ngx_pcalloc(pool, sizeof(struct sockaddr_un));
        if (sun == NULL) {
            return NGX_ERROR;
        }

        sun->sun_family = AF_UNIX;
        if (tlcf->socket_path.len >= sizeof(sun->sun_path)) {
            return NGX_ERROR;
        }
        ngx_memcpy(sun->sun_path, tlcf->socket_path.data, tlcf->socket_path.len);

        tc->sockaddr = (u_char *) sun;
        tc->socklen = sizeof(struct sockaddr_un);

        tc->name.data = (u_char *) "unix:renderd";
        tc->name.len = sizeof("unix:renderd") - 1;

        return NGX_OK;
    }

    /* TCP endpoint: resolve the host once (blocking getaddrinfo, same as
     * mod_tile's socket_init; use a numeric IP or a local host entry to
     * avoid a DNS lookup in the worker) */
    {
        struct addrinfo  hints, *res, *rp;
        char             portnum[16];

        ngx_memzero(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        snprintf(portnum, sizeof(portnum), "%d", (int) tlcf->renderd_port);

        if (getaddrinfo((const char *) tlcf->renderd_host.data, portnum,
                        &hints, &res) != 0)
        {
            return NGX_ERROR;
        }

        for (rp = res; rp != NULL; rp = rp->ai_next) {
            tc->sockaddr = ngx_palloc(pool, rp->ai_addrlen);
            if (tc->sockaddr == NULL) {
                freeaddrinfo(res);
                return NGX_ERROR;
            }
            ngx_memcpy(tc->sockaddr, rp->ai_addr, rp->ai_addrlen);
            tc->socklen = rp->ai_addrlen;
            break;
        }

        freeaddrinfo(res);

        if (rp == NULL) {
            return NGX_ERROR;
        }

        tc->name.data = (u_char *) "tcp:renderd";
        tc->name.len = sizeof("tcp:renderd") - 1;

        return NGX_OK;
    }
}

/* ------------------------------------------------------------------ */
/* sending                                                             */
/* ------------------------------------------------------------------ */

#define TILE_CONN_ALIVE(tc)                                                \
    ((tc)->magic == 0x7133C0DE && !(tc)->destroyed                          \
     && ((tc)->r == NULL || !(tc)->r->done))

static void
ngx_http_tile_conn_write_handler(ngx_event_t *ev)
{
    /* ev->data is the connection; the conn context hangs off c->data,
     * exactly like ngx_http_upstream_handler */
    ngx_connection_t       *c = ev->data;
    ngx_http_tile_conn_t   *tc = c->data;

    if (tc == NULL || tc->magic != 0x7133C0DE) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "tile: BUG stale write event c=%p", (void *) c);
        return;
    }

    if (tc->destroyed || tc->peer.connection == NULL) {
        return;
    }

    if (tc->r != NULL && tc->r->done) {
        ngx_log_error(NGX_LOG_ALERT, tc->peer.log, 0,
                      "tile: BUG write event after request done, closing");
        ngx_http_tile_conn_close(tc);
        tc->destroyed = 1;
        return;
    }

    if (ev->timedout) {
        if (!tc->sent) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "tile: connect to renderd timed out (%V)", &tc->name);
            ngx_http_tile_conn_close(tc);
            ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_ERROR);
        }
        return;
    }

    if (tc->connecting) {
        /* connect() completion: check for a deferred error (TCP) */
        if (ngx_http_tile_conn_test_connect(c) != NGX_OK) {
            ngx_http_tile_conn_close(tc);
            ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_ERROR);
            return;
        }
        tc->connecting = 0;
    }

    if (!tc->sent) {
        ngx_http_tile_conn_send(tc);
    }
}

static ngx_int_t
ngx_http_tile_conn_send(ngx_http_tile_conn_t *tc)
{
    ngx_connection_t  *c = tc->peer.connection;
    size_t             len;
    ssize_t            n;
    ngx_err_t          err;

    if (c == NULL) {
        return NGX_OK;
    }

    /* the connect timeout no longer applies once we start sending */
    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    /* v2 packets only carry the shorter struct */
    len = (tc->cmd.ver == 3) ? sizeof(struct protocol)
                             : sizeof(struct protocol_v2);

    for ( ;; ) {
        n = c->send(c, (u_char *) &tc->cmd + tc->nsent, len - tc->nsent);

        if (n > 0) {
            tc->nsent += n;

            if (tc->nsent >= len) {
                break;
            }

            continue;
        }

        if (n == NGX_EAGAIN) {
            /* resume on the next write event */
            return NGX_OK;
        }

        if (n == NGX_EINTR) {
            continue;
        }

        err = ngx_socket_errno;
        ngx_log_error(NGX_LOG_WARN, c->log, err,
                      "tile: failed to send request to renderd");

        if ((err == NGX_EPIPE || err == NGX_ECONNRESET) && tc->retries > 0) {
            ngx_int_t rc;

            /* one silent reconnect, mirroring mod_tile's EPIPE retry */
            tc->retries--;
            ngx_http_tile_conn_close(tc);
            rc = ngx_http_tile_conn_submit(tc);

            if (rc != NGX_OK) {
                ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_ERROR);
            }

            return rc;
        }

        ngx_http_tile_conn_close(tc);
        ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_ERROR);
        return NGX_OK;
    }

    tc->sent = 1;

    if (!tc->wait_reply) {
        /* fire-and-forget (cmdDirty): done as soon as the packet is out */
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "tile: dirty packet sent to renderd (%V)", &tc->name);
        ngx_http_tile_conn_close(tc);
        ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_DONE);
        return NGX_OK;
    }

    /* arm the render deadline and wait for the reply */
    ngx_add_timer(c->read, tc->reply_timeout);
    c->write->handler = ngx_http_tile_conn_write_handler; /* partial sends */
    c->read->handler = ngx_http_tile_conn_read_handler;

    if (c->read->ready) {
        ngx_http_tile_conn_reply_read(tc);
    }

    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* reply                                                               */
/* ------------------------------------------------------------------ */

static void
ngx_http_tile_conn_read_handler(ngx_event_t *ev)
{
    ngx_connection_t     *c = ev->data;
    ngx_http_tile_conn_t *tc = c->data;

    if (tc == NULL || tc->magic != 0x7133C0DE) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "tile: BUG stale read event c=%p", (void *) c);
        return;
    }

    if (tc->destroyed) {
        return;
    }

    if (tc->r != NULL && tc->r->done) {
        ngx_log_error(NGX_LOG_ALERT, tc->peer.log, 0,
                      "tile: BUG read event after request done, closing");
        ngx_http_tile_conn_close(tc);
        tc->destroyed = 1;
        return;
    }

    if (!tc->sent) {
        /* during connect/send only the write path drives the state machine;
         * a read event here (peer reset) surfaces via the write handler's
         * SO_ERROR check */
        return;
    }

    ngx_http_tile_conn_reply_read(tc);
}

static ngx_int_t
ngx_http_tile_conn_reply_read(ngx_http_tile_conn_t *tc)
{
    ngx_connection_t  *c = tc->peer.connection;
    ssize_t            n;
    size_t             want;
    ngx_err_t          err;

    if (c == NULL) {
        return NGX_OK;
    }

    if (c->read->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "tile: renderd did not answer in time for %s z%d x%d y%d",
                      tc->cmd.xmlname, tc->cmd.z, tc->cmd.x, tc->cmd.y);
        ngx_http_tile_conn_close(tc);
        ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_NOT_DONE);
        return NGX_OK;
    }

    for ( ;; ) {
        want = (tc->nrecv >= sizeof(int) && tc->resp.ver == 3)
               ? sizeof(struct protocol) : sizeof(struct protocol_v2);

        if (tc->nrecv >= want) {
            break;
        }

        n = c->recv(c, (u_char *) &tc->resp + tc->nrecv, want - tc->nrecv);

        if (n > 0) {
            tc->nrecv += n;
            continue;
        }

        if (n == NGX_EAGAIN) {
            return NGX_OK; /* wait for the next read event */
        }

        if (n == NGX_EINTR) {
            continue;
        }

        /* right after a non-blocking TCP connect the socket can still
         * report EINPROGRESS on the first read: keep waiting */
        if (n < 0 && ngx_socket_errno == NGX_EINPROGRESS) {
            return NGX_OK;
        }

        err = ngx_socket_errno;

        ngx_log_error(NGX_LOG_INFO, c->log, (n < 0) ? err : 0,
                      "tile: failed to read reply from renderd");

        ngx_http_tile_conn_close(tc);
        ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_NOT_DONE);
        return NGX_OK;
    }

    /* full packet received: verify it belongs to our request */
    ngx_http_tile_conn_close(tc);

    if (tc->cmd.x != tc->resp.x || tc->cmd.y != tc->resp.y
        || tc->cmd.z != tc->resp.z
        || ngx_strncmp((u_char *) tc->cmd.xmlname,
                       (u_char *) tc->resp.xmlname,
                       sizeof(tc->cmd.xmlname)) != 0)
    {
        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "tile: renderd reply does not match request: "
                      "xml(%s,%s) z(%d,%d) x(%d,%d) y(%d,%d)",
                      tc->cmd.xmlname, tc->resp.xmlname,
                      tc->cmd.z, tc->resp.z, tc->cmd.x, tc->resp.x,
                      tc->cmd.y, tc->resp.y);
        ngx_http_tile_conn_finish(tc, NGX_TILE_CONN_NOT_DONE);
        return NGX_OK;
    }

    ngx_http_tile_conn_finish(tc, (tc->resp.cmd == cmdDone)
                                   ? NGX_TILE_CONN_DONE
                                   : NGX_TILE_CONN_NOT_DONE);
    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* teardown                                                            */
/* ------------------------------------------------------------------ */

static void
ngx_http_tile_conn_close(ngx_http_tile_conn_t *tc)
{
    ngx_connection_t *c = tc->peer.connection;

    if (c == NULL) {
        return;
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    ngx_close_connection(c);
    tc->peer.connection = NULL;
}

static void
ngx_http_tile_conn_finish(ngx_http_tile_conn_t *tc, ngx_int_t rc)
{
    ngx_http_tile_conn_done_pt done = tc->done;
    void                      *data = tc->data;

    tc->destroyed = 1;

    done(data, rc);

    if (tc->pool != NULL) {
        ngx_destroy_pool(tc->pool);
    }
}

/* ------------------------------------------------------------------ */
/* connect completion test (port of ngx_http_upstream_test_connect)    */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_tile_conn_test_connect(ngx_connection_t *c)
{
    int        err;
    socklen_t  len;

    if (c->read->timedout || c->write->timedout) {
        return NGX_ERROR;
    }

    err = 0;
    len = sizeof(int);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
        == -1)
    {
        err = ngx_socket_errno;
    }

    if (err) {
        ngx_log_error(NGX_LOG_WARN, c->log, err,
                      "tile: connect() to renderd failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* cleanup hook for request-tied connections                           */
/* ------------------------------------------------------------------ */

static void
ngx_http_tile_conn_cleanup(void *data)
{
    ngx_http_tile_conn_t *tc = data;

    if (!tc->destroyed) {
        ngx_log_error(NGX_LOG_ALERT, tc->peer.log, 0,
                      "tile: BUG request pool destroyed with conn open");
        ngx_http_tile_conn_close(tc);
        tc->destroyed = 1;
    }
}

/* ------------------------------------------------------------------ */
/* public API: request-tied submit (called from tile_serve.c)          */
/* ------------------------------------------------------------------ */

static void
ngx_http_tile_render_done(void *data, ngx_int_t rc)
{
    ngx_http_tile_ctx_t    *ctx = data;
    ngx_http_request_t     *r = ctx->r;
    ngx_int_t               result;

    ctx->conn = NULL;

    if (r->connection->destroyed) {
        return;
    }

    /* refresh the tile state and let the state table decide between
     * serving the (re-)rendered tile, serving stale, or returning 404 */
    result = ngx_http_tile_check_state(r, ctx, rc);

    ngx_http_finalize_request(r, result);
}

ngx_int_t
ngx_http_tile_render_submit(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_http_tile_loc_conf_t *tlcf,
    ngx_int_t priority)
{
    ngx_http_tile_conn_t   *tc;
    ngx_pool_cleanup_t     *cln;

    tc = ngx_pcalloc(r->pool, sizeof(ngx_http_tile_conn_t));
    if (tc == NULL) {
        return NGX_ERROR;
    }

    tc->magic = 0x7133C0DE;
    tc->r = r;
    tc->pool = NULL;
    tc->data = ctx;
    tc->done = ngx_http_tile_render_done;
    tc->cmd = ctx->cmd;
    tc->wait_reply = 1;
    tc->retries = 1;
    tc->connect_timeout = tlcf->connect_timeout;
    tc->reply_timeout = (priority > 2) ? tlcf->request_timeout_priority
                                       : tlcf->request_timeout;

    /* map render priority to the wire opcode (request_tile equivalent) */
    switch (priority) {
    case 1:
        tc->cmd.cmd = cmdRenderLow;
        break;
    case 2:
        tc->cmd.cmd = cmdRender;
        break;
    case 3:
        tc->cmd.cmd = cmdRenderPrio;
        break;
    default:
        tc->cmd.cmd = cmdDirty;
        break;
    }

    if (tlcf->enable_bulk_mode) {
        tc->cmd.cmd = cmdRenderBulk;
    }

    if (ngx_http_tile_conn_prepare(tc, tlcf) != NGX_OK) {
        return NGX_ERROR;
    }

    /* if the client vanishes while we wait, drop the renderd connection */
    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }
    cln->handler = ngx_http_tile_conn_cleanup;
    cln->data = tc;
    ctx->cln = cln;

    if (ngx_http_tile_conn_submit(tc) != NGX_OK) {
        ctx->cln = NULL;
        return NGX_ERROR;
    }

    ctx->conn = tc;

    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* public API: detached dirty submit (survives the request)            */
/* ------------------------------------------------------------------ */

static void
ngx_http_tile_dirty_done(void *data, ngx_int_t rc)
{
    /* pool destroyed by the engine finish path */
}

void
ngx_http_tile_dirty_submit_detached(ngx_http_request_t *r,
    ngx_http_tile_loc_conf_t *tlcf, struct protocol *cmd)
{
    ngx_pool_t             *pool;
    ngx_http_tile_conn_t   *tc;

    pool = ngx_create_pool(512, ngx_cycle->log);
    if (pool == NULL) {
        return;
    }

    tc = ngx_pcalloc(pool, sizeof(ngx_http_tile_conn_t));
    if (tc == NULL) {
        ngx_destroy_pool(pool);
        return;
    }

    tc->magic = 0x7133C0DE;
    tc->r = NULL;
    tc->pool = pool;
    tc->data = tc;
    tc->done = ngx_http_tile_dirty_done;
    tc->cmd = *cmd;
    tc->cmd.cmd = cmdDirty;
    tc->wait_reply = 0;
    tc->retries = 1;
    tc->connect_timeout = tlcf->connect_timeout;

    if (ngx_http_tile_conn_prepare(tc, tlcf) != NGX_OK) {
        ngx_destroy_pool(pool);
        return;
    }

    if (ngx_http_tile_conn_submit(tc) != NGX_OK) {
        ngx_destroy_pool(pool);
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "tile: detached dirty submission started");
}
