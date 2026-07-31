#ifndef RE0_LOG_H
#define RE0_LOG_H

#include <stdarg.h>

typedef enum {
    RE0_LOG_DEBUG,
    RE0_LOG_INFO,
    RE0_LOG_WARN,
    RE0_LOG_ERROR,
    RE0_LOG_FATAL,
} Re0LogLevel;

void re0_log(Re0LogLevel level, const char *fmt, ...);

#endif
