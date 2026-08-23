#ifndef RE0_SAFE_H
#define RE0_SAFE_H

#include <stdlib.h>

/* ── Checked allocation: abort with a report on OOM ──
 *
 * Production principles:
 * - memory allocation failure must not cause a segfault
 * - a clear error message should be printed before abort
 * - arena allocation is already handled inside arena.c; only heap allocation is covered here
 *
 * Implementations live in base/safe.c.
 */

void *re0_xmalloc(size_t sz, const char *file, int line);
void *re0_xcalloc(size_t n, size_t sz, const char *file, int line);
void *re0_xrealloc(void *ptr, size_t sz, const char *file, int line);

#define xmalloc(sz)        re0_xmalloc((sz), __FILE__, __LINE__)
#define xcalloc(n, sz)     re0_xcalloc((n), (sz), __FILE__, __LINE__)
#define xrealloc(ptr, sz)  re0_xrealloc((ptr), (sz), __FILE__, __LINE__)

#endif
