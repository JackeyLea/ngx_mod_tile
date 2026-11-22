/*
 * ngx_mod_tile - thread-pool offload implementation (stage 3).
 *
 * Lifecycle: post() allocates a private task pool -> the thread handler runs
 * the blocking store call -> the completion event fires in the worker loop.
 * The request may abort while the task runs: the pool cleanup only raises a
 * flag, and the completion event destroys the task pool after checking it,
 * so neither side can touch freed memory.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <pthread.h>

#include "ngx_mod_tile.h"
#include "tile_thread.h"

#if (NGX_THREADS)

#define NGX_TILE_THREAD_TIMEOUT 60000  /* watchdog, ms */

typedef struct {
    ngx_pool_t             *pool;      /* private task pool */
    ngx_http_request_t     *r;
    ngx_http_tile_ctx_t    *ctx;
    ngx_thread_task_t      *task;
    ngx_thread_pool_t      *tp;
    struct storage_backend *store;     /* per-thread instance */
    unsigned                request_gone:1;
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
static void ngx_http_tile_thread_cleanup(void *data);

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
    ngx_http_tile_thread_t *t = ev->data;
    ngx_http_request_t     *r = t->r;

    if (ev->timedout) {
        /* watchdog: the task is still running, keep waiting for completion */
        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                      "tile: thread pool operation took too long");
        ev->timedout = 0;
        return;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    if (t->request_gone) {
        ngx_destroy_pool(t->pool);
        return;
    }

    t->ctx->thread_stage = NGX_TILE_THREAD_NONE;

    ngx_http_tile_thread_resume(r);

    ngx_http_run_posted_requests(r->connection);
}

static void
ngx_http_tile_thread_cleanup(void *data)
{
    ngx_http_tile_thread_t *t = data;

    /* the request pool died while the task may still run */
    t->request_gone = 1;
}

static ngx_int_t
ngx_http_tile_thread_post(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx,
    ngx_uint_t stage)
{
    ngx_http_tile_loc_conf_t *tlcf;
    ngx_http_tile_main_conf_t      *mcf;
    ngx_http_tile_thread_t         *t;
    ngx_pool_cleanup_t             *cln;
    ngx_pool_t                     *tpool;
    ngx_str_t                       store_uri;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (tlcf->thread_pool == NULL) {
        return NGX_ERROR;
    }

    /* the task lives in its own pool: it may outlive the request if the
     * client aborts while the thread is running */
    tpool = ngx_create_pool(1024, r->connection->log);
    if (tpool == NULL) {
        return NGX_ERROR;
    }

    t = ngx_pcalloc(tpool, sizeof(ngx_http_tile_thread_t));
    if (t == NULL) {
        ngx_destroy_pool(tpool);
        return NGX_ERROR;
    }

    t->pool = tpool;
    t->r = r;
    t->ctx = ctx;
    t->tp = tlcf->thread_pool;

    /* the task uses a per-thread store instance (thread-safe clients) */
    store_uri = ctx->layer->store_uri.len ? ctx->layer->store_uri
                                          : tlcf->tile_dir;
    t->store = ngx_http_tile_get_store_thread(mcf, &store_uri,
                                              r->connection->log);
    if (t->store == NULL) {
        ngx_destroy_pool(tpool);
        return NGX_ERROR;
    }

    if (stage == NGX_TILE_THREAD_READ) {
        t->buf = ngx_palloc(tpool, MAX_SIZE);
        if (t->buf == NULL) {
            ngx_destroy_pool(tpool);
            return NGX_ERROR;
        }
    }

    t->task = ngx_thread_task_alloc(tpool, 0);
    if (t->task == NULL) {
        ngx_destroy_pool(tpool);
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

    /* if the request aborts, the completion event cleans the task pool up */
    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        ngx_destroy_pool(tpool);
        return NGX_ERROR;
    }
    cln->handler = ngx_http_tile_thread_cleanup;
    cln->data = t;
    ctx->thread_cleanup = cln;

    if (ngx_thread_task_post(t->tp, t->task) != NGX_OK) {
        ctx->thread_cleanup = NULL;
        ngx_destroy_pool(tpool);
        return NGX_ERROR;
    }

    ngx_add_timer(&t->task->event, NGX_TILE_THREAD_TIMEOUT);

    ctx->thread_stage = stage;
    ctx->thread_ctx = t;

    /* hold the request; the handler returns NGX_DONE and the core
     * finalize drops the hold, same as the renderd wait path */
    r->main->count++;

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

void
ngx_http_tile_thread_resume(ngx_http_request_t *r)
{
    ngx_http_tile_ctx_t       *ctx;
    ngx_http_tile_thread_t    *t;
    ngx_int_t                  rc;

    ctx = ngx_http_get_module_ctx(r, ngx_http_tile_module);

    if (ctx == NULL || ctx->thread_ctx == NULL) {
        return;
    }

    t = ctx->thread_ctx;
    ctx->thread_ctx = NULL;

    /* hand the results over to the request context */
    if (ctx->thread_stage == NGX_TILE_THREAD_STAT) {
        ctx->st_result = t->st;
        ctx->thread_stage = NGX_TILE_THREAD_NONE;
        ngx_destroy_pool(t->pool);

        rc = ngx_http_tile_serve_flow_after_stat(r, ctx);

        if (rc != NGX_DONE) {
            ngx_http_finalize_request(r, rc);
        }

        return;
    }

    if (ctx->thread_stage == NGX_TILE_THREAD_READ) {
        ctx->thread_stage = NGX_TILE_THREAD_NONE;
        rc = ngx_http_tile_send_response_after_read(r, ctx, t->buf, t->len,
                                                    t->compressed, t->err_msg);
        ngx_destroy_pool(t->pool);
        ngx_http_finalize_request(r, rc);
        return;
    }
}

#endif /* NGX_THREADS */
