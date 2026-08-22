#ifndef RE0_GC_STATS_H
#define RE0_GC_STATS_H

#include <stddef.h>
#include <stdint.h>

/* ── GC statistics ──
 * Cumulative data recorded over the engine's lifetime, for verbose
 * output and external queries.
 */
typedef struct {
    /* current snapshot */
    int    alive_count;       /* live objects */
    size_t alive_bytes;       /* live bytes */

    /* cumulative totals */
    int    total_alloc;       /* total allocations */
    size_t total_alloc_bytes; /* total bytes allocated */
    int    total_freed;       /* total frees */
    size_t total_freed_bytes; /* total bytes freed */
    int    collect_count;     /* total collection cycles */
    uint64_t total_pause_ns;  /* total STW pause time (ns) */

    /* last collection details */
    int    last_freed_count;
    size_t last_freed_bytes;
    uint64_t last_pause_ns;
} Re0GcStats;

void re0_gc_stats_reset(Re0GcStats *s);

/* call after allocation: bump live counters */
void re0_gc_stats_on_alloc(Re0GcStats *s, size_t bytes);

/* call after free: decrement live counters */
void re0_gc_stats_on_free(Re0GcStats *s, size_t bytes);

/* call at the end of a collection cycle: record pause and freed amount */
void re0_gc_stats_on_collect(Re0GcStats *s, int freed_count,
                              size_t freed_bytes, uint64_t pause_ns);

#endif
