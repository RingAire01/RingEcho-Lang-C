#include "base/re0_log.h"
#include "platform.h"
#include <stdio.h>
#include <time.h>

#if defined(RE0_PLATFORM_WINDOWS)
static struct tm *localtime_r(const time_t *t, struct tm *tm_buf) {
    if (localtime_s(tm_buf, t) == 0) return tm_buf;
    return NULL;
}
#endif

static const char *re0_log_level_tag(Re0LogLevel l) {
    switch (l) {
        case RE0_LOG_DEBUG: return "DEBUG";
        case RE0_LOG_INFO:  return "INFO";
        case RE0_LOG_WARN:  return "WARN";
        case RE0_LOG_ERROR: return "ERROR";
        case RE0_LOG_FATAL: return "FATAL";
    }
    return "?";
}

void re0_log(Re0LogLevel level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char ts[32];
    if (tm) strftime(ts, sizeof(ts), "%Y-%m.%d-%H:%M:%S", tm);
    else { ts[0] = '?'; ts[1] = '\0'; }

    fprintf(stderr, "[*] [%s] [%s] ", ts, re0_log_level_tag(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
