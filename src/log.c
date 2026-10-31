/*
 * ds4xbox - logging helpers
 *
 * This file is released into the public domain (Unlicense).
 */
#include "common.h"

#include <time.h>

enum log_level g_log_level = LOG_LVL_INFO;

void log_msg(enum log_level level, const char *fmt, ...)
{
    static const char *const tags[] = {
        [LOG_LVL_ERROR] = "error",
        [LOG_LVL_WARN] = "warning",
        [LOG_LVL_INFO] = "info",
        [LOG_LVL_DEBUG] = "debug",
    };
    struct timespec ts;
    struct tm tm;
    char stamp[32];
    va_list ap;

    if (level > g_log_level)
        return;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    fprintf(stderr, "%s.%03ld ds4xbox[%s]: ", stamp, ts.tv_nsec / 1000000L,
            tags[level]);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
