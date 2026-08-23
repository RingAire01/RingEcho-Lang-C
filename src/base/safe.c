#include "base/safe.h"
#include "base/re0_log.h"

/* Checked allocation implementations (see base/safe.h for the rationale). */

void *re0_xmalloc(size_t sz, const char *file, int line) {
    void *p = malloc(sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (malloc %zu at %s:%d)", sz, file, line);
        abort();
    }
    return p;
}

void *re0_xcalloc(size_t n, size_t sz, const char *file, int line) {
    void *p = calloc(n, sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (calloc %zu*%zu at %s:%d)", n, sz, file, line);
        abort();
    }
    return p;
}

void *re0_xrealloc(void *ptr, size_t sz, const char *file, int line) {
    void *p = realloc(ptr, sz);
    if (!p) {
        re0_log(RE0_LOG_FATAL, "out of memory (realloc %zu at %s:%d)", sz, file, line);
        abort();
    }
    return p;
}
