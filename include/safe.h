#ifndef RE0_SAFE_H
#define RE0_SAFE_H

#include <stdlib.h>
#include <stdio.h>
#include "re0_log.h"

/* ── Checked allocation: OOM 时 abort 并报告 ──
 *
 * 生产环境原则：
 * - 内存分配失败不应该导致 segfault
 * - 应该在 abort 前打印清晰的错误消息
 * - arena 分配已由 arena.c 内部处理，此处仅覆盖堆分配
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
