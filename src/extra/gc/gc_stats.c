#include "gc/gc_stats.h"
#include <string.h>

void re0_gc_stats_reset(Re0GcStats *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

void re0_gc_stats_on_alloc(Re0GcStats *s, size_t bytes)
{
    if (!s) return;
    s->alive_count++;
    s->alive_bytes += bytes;
    s->total_alloc++;
    s->total_alloc_bytes += bytes;
}

void re0_gc_stats_on_free(Re0GcStats *s, size_t bytes)
{
    if (!s) return;
    if (s->alive_count > 0) s->alive_count--;
    if (s->alive_bytes >= bytes) s->alive_bytes -= bytes;
    else s->alive_bytes = 0;
    s->total_freed++;
    s->total_freed_bytes += bytes;
}

void re0_gc_stats_on_collect(Re0GcStats *s, int freed_count,
                              size_t freed_bytes, uint64_t pause_ns)
{
    if (!s) return;
    s->collect_count++;
    s->last_freed_count  = freed_count;
    s->last_freed_bytes  = freed_bytes;
    s->last_pause_ns     = pause_ns;
    s->total_pause_ns   += pause_ns;
}
