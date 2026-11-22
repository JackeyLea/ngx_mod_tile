/*
 * ngx_mod_tile - thread-pool offload implementation (stage 3).
 *
 * Mirrors ngx_http_upstream's sendfile-in-threads pattern exactly:
 *   - task context and payload are allocated from the REQUEST pool;
 *   - posting holds the request with r->main->blocked++ and r->aio = 1,
 *     so the pool survives even if the client aborts mid-task;
 *   - the completion event decrements blocked, resumes the tile flow and
 *     runs posted requests, like ngx_http_upstream_thread_event_handler.
 *
 * Each thread uses its own storage_backend instance (pthread TLS in
 * tile_store.c) because the network-backed clients are not thread-safe.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_THREADS)

#include "ngx_mod_tile.h"
#include "tile_thread.h"

#define NGX_TILE_THREAD_TIMEOUT 60000  /* watchdog, ms */

typedef struct {
    ngx_http_request_t     *r;
    ngx_http_tile_ctx_t    *ctx;
    ngx_thread_task_t      *task;
    struct storage_backend *store;
    /* stat result */
    struct stat_info        st;
    /* read result */
    u_char                 *buf;
    ssize_t                 len;
    int                     compressed;
    char                    err_msg[1024];
} ngx_http_tile_thread_t;

static void ngx_http_tile_thread_stat_task(void *data, ngx_log_t *log);
static void ngx_http_tile_thread_read_task(void *data, ngx_log_t *log);
static void ngx_http_tile_thread_event(ngx_event_t *ev);

static void
ngx_http_tile_thread_stat_task(void *data, ngx_log_t *log)
{
    ngx_http_tile_thread_t *t = data;

    t->st = t->store->tile_stat(t->store, (const char *) t->ctx->cmd.xmlname,
                                (const char *) t->ctx->cmd.options,
                                t->ctx->cmd.x, t->ctx->cmd.y, t->ctx->cmd.z);
}

static void
ngx_http_tile_thread_read_task(void *data, ngx_log_t *log)
{
    ngx_http_tile_thread_t *t = data;

    t->err_msg[0] = '\0';

    t->len = t->store->tile_read(t->store,
                                 (const char *) t->ctx->cmd.xmlname,
                                 (const char *) t->ctx->cmd.options,
                                 t->ctx->cmd.x, t->ctx->cmd.y, t->ctx->cmd.z,
                                 (char *) t->buf, MAX_SIZE, &t->compressed,
                                 t->err_msg);
}

static void
ngx_http_tile_thread_event(ngx_event_t *ev)
{
    ngx_http_tile_thread_t    *t = ev->data;
    ngx_http_request_t        *r = t->r;
    ngx_connection_t          *c = r->connection;
    ngx_int_t                  rc;
    ngx_uint_t                 stage = t->ctx->thread_stage;

    ngx_http_set_log_request(c->log, r);

    if (ev->timedout) {
        /* watchdog: log and keep waiting for the task to complete */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "tile: thread pool operation took too long");
        ev->timedout = 0;
        return;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    r->main->blocked--;
    r->aio = 0;

    if (r->done || r->main->terminated) {
        /* the request was finalized while the task ran; drive teardown the
         * way ngx_http_upstream_thread_event_handler does */
        c->write->handler(c->write);
        return;
    }

    rc = ngx_http_tile_thread_resume(r, stage);

    if (rc != NGX_DONE) {
        ngx_http_finalize_request(r, rc);
    }

    ngx_http_run_posted_requests(c);
}

static ngx_int_t
ngx_http_tile_thread_post(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx,
    ngx_uint_t stage)
{
    ngx_http_tile_loc_conf_t *tlcf;
    ngx_http_tile_main_conf_t      *mcf;
    ngx_http_tile_thread_t         *t;
    ngx_str_t                       store_uri;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (tlcf->thread_pool == NULL) {
        return NGX_ERROR;
    }

    /* from the request pool: blocked++ keeps the pool alive until the
     * completion event runs, even when the client aborts */
    t = ngx_pcalloc(r->pool, sizeof(ngx_http_tile_thread_t));
    if (t == NULL) {
        return NGX_ERROR;
    }

    t->r = r;
    t->ctx = ctx;

    /* the task uses a per-thread store instance (thread-safe clients) */
    store_uri = ctx->layer->store_uri.len ? ctx->layer->store_uri
                                          : tlcf->tile_dir;
    t->store = ngx_http_tile_get_store_thread(mcf, &store_uri,
                                              r->connection->log);
    if (t->store == NULL) {
        return NGX_ERROR;
    }

    if (stage == NGX_TILE_THREAD_READ) {
        t->buf = ngx_palloc(r->pool, MAX_SIZE);
        if (t->buf == NULL) {
            return NGX_ERROR;
        }
    }

    t->task = ngx_thread_task_alloc(r->pool, 0);
    if (t->task == NULL) {
        return NGX_ERROR;
    }

    t->task->ctx = t;
    t->task->handler = (stage == NGX_TILE_THREAD_STAT)
                       ? ngx_http_tile_thread_stat_task
                       : ngx_http_tile_thread_read_task;

    t->task->event.data = t;
    t->task->event.handler = ngx_http_tile_thread_event;
    t->task->event.log = r->connection->log;
    t->task->event.complete = 1;

    if (ngx_thread_task_post(tlcf->thread_pool, t->task) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_add_timer(&t->task->event, NGX_TILE_THREAD_TIMEOUT);

    ctx->thread_stage = stage;
    ctx->thread_ctx = t;

    /* hold the request through the suspension: the handler returns
     * NGX_DONE (core drops one hold) and blocked++ covers the task */
    r->main->count++;
    r->main->blocked++;
    r->aio = 1;

    return NGX_DONE;
}

ngx_int_t
ngx_http_tile_thread_stat(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    return ngx_http_tile_thread_post(r, ctx, NGX_TILE_THREAD_STAT);
}

ngx_int_t
ngx_http_tile_thread_read(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    return ngx_http_tile_thread_post(r, ctx, NGX_TILE_THREAD_READ);
}

ngx_int_t
ngx_http_tile_thread_resume(ngx_http_request_t *r, ngx_uint_t stage)
{
    ngx_http_tile_ctx_t    *ctx;
    ngx_http_tile_thread_t *t;
    ngx_int_t               rc;

    ctx = ngx_http_get_module_ctx(r, ngx_http_tile_module);

    if (ctx == NULL || ctx->thread_ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    t = ctx->thread_ctx;
    ctx->thread_ctx = NULL;
    ctx->thread_stage = NGX_TILE_THREAD_NONE;

    /* results live in the request pool */
    if (stage == NGX_TILE_THREAD_STAT) {
        ctx->st_result = t->st;

        rc = ngx_http_tile_serve_flow_after_stat(r, ctx);
        return rc;
    }

    rc = ngx_http_tile_send_response_after_read(r, ctx, t->buf, t->len,
                                                t->compressed, t->err_msg);
    return rc;
}

#endif /* NGX_THREADS */
