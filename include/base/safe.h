#ifndef RE0_SAFE_H
#define RE0_SAFE_H

#include <stdlib.h>
#include <stdio.h>
#include "base/re0_log.h"

/* ── Checked allocation: abort with a report on OOM ──
 *
 * Production principles:
 * - memory allocation failure must not cause a segfault
 * - a clear error message should be printed before abort
 * - arena allocation is already handled inside arena.c; only heap allocation is covered here
 */

static inline void *re0_xmalloc(size_t sz, const char *file, int line) {
    void *p = malloc(sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (malloc %zu at %s:%d)", sz, file, line);
        abort();
    }
    return p;
}

static inline void *re0_xcalloc(size_t n, size_t sz, const char *file, int line) {
    void *p = calloc(n, sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (calloc %zu*%zu at %s:%d)", n, sz, file, line);
        abort();
    }
    return p;
}

static inline void *re0_xrealloc(void *ptr, size_t sz, const char *file, int line) {
    void *p = realloc(ptr, sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (realloc %zu at %s:%d)", sz, file, line);
        abort();
    }
    return p;
}

#define xmalloc(sz)        re0_xmalloc((sz), __FILE__, __LINE__)
#define xcalloc(n, sz)     re0_xcalloc((n), (sz), __FILE__, __LINE__)
#define xrealloc(ptr, sz)  re0_xrealloc((ptr), (sz), __FILE__, __LINE__)

#endif
