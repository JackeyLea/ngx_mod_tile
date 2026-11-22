/*
 * Implementation of the g_logger shim declared in g_logger.h, forwarding to
 * nginx's logging core. All vendored mod_tile sources log through this shim,
 * so tile-store diagnostics land in the nginx error log with a "tile: "
 * prefix.
 *
 * Messages are rendered with vsnprintf first because the vendored sources use
 * plain libc format specifiers (e.g. "%lf") that nginx's ngx_vslprintf does
 * not all support.
 */

#include <ngx_config.h>
#include <ngx_core.h>

#include "g_logger.h"

int foreground = 1;

void g_logger(int log_level, const char *format, ...)
{
    char             msg[2048];
    va_list          args;
    ngx_uint_t       level;

    /* the global default log is file-static in ngx_log.c; before the cycle
     * exists there is nothing to log to */
    if (ngx_cycle == NULL || ngx_cycle->log == NULL) {
        return;
    }

    switch (log_level) {
    case G_LOG_LEVEL_ERROR:
        level = NGX_LOG_ALERT;
        break;
    case G_LOG_LEVEL_CRITICAL:
        level = NGX_LOG_ERR;
        break;
    case G_LOG_LEVEL_WARNING:
        level = NGX_LOG_WARN;
        break;
    case G_LOG_LEVEL_MESSAGE:
        level = NGX_LOG_NOTICE;
        break;
    case G_LOG_LEVEL_INFO:
        level = NGX_LOG_INFO;
        break;
    case G_LOG_LEVEL_DEBUG:
        level = NGX_LOG_DEBUG;
        break;
    default:
        level = NGX_LOG_INFO;
    }

    va_start(args, format);
    (void) vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    ngx_log_error_core(level, ngx_cycle->log, 0, "tile: %s", msg);
}

const char *g_logger_level_name(int log_level)
{
    switch (log_level) {
    case G_LOG_LEVEL_ERROR:
        return "Error";
    case G_LOG_LEVEL_CRITICAL:
        return "Critical";
    case G_LOG_LEVEL_WARNING:
        return "Warning";
    case G_LOG_LEVEL_MESSAGE:
        return "Message";
    case G_LOG_LEVEL_INFO:
        return "Info";
    case G_LOG_LEVEL_DEBUG:
        return "Debug";
    default:
        return "Unknown";
    }
}
