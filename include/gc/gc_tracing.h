#ifndef RE0_GC_TRACING_H
#define RE0_GC_TRACING_H

#include <stdbool.h>
#include "gc_object.h"
#include "gc_roots.h"
#include "gc_stats.h"
#include "gc_events.h"

/* ── tracing mark-sweep algorithm ──
 * Tri-color marking (white/gray/black) + gray worklist.
 * Fixes the old implementation where mark never walked child nodes.
 *
 * Flow:
 *   1. mark  — all roots are marked GRAY and enqueued; pop each, mark BLACK,
 *              call the trace callback to walk children, WHITE→GRAY enqueue
 *   2. sweep — walk the object list, free collectable WHITE objects,
 *              reset BLACK→WHITE
 */

/* collection context: filled in by the engine and passed to collect */
typedef struct {
    Re0GcObject   **head;        /* object list head (sweep mutates the list) */
    Re0GcRootSet   *roots;       /* root set */
    Re0GcStats     *stats;       /* stats output */
    Re0GcListeners *listeners;   /* event listeners (may be NULL) */
    Re0GcMode       mode;        /* engine mode (for events) */
    Re0GcAlgo       algo;        /* engine algorithm (for events) */
} Re0GcTracingCtx;

/* run a full mark-sweep collection */
void re0_gc_tracing_collect(Re0GcTracingCtx *ctx);

/* run only the mark phase (reserved for concurrent mode: callable standalone)
 * Returns true on mark-stack OOM — the caller must abandon this sweep,
 * otherwise unmarked-but-reachable objects would be reclaimed. */
bool re0_gc_tracing_mark(Re0GcRootSet *roots, Re0GcListeners *ls);

/* run only the sweep phase (reserved for concurrent mode) */
int  re0_gc_tracing_sweep(Re0GcObject **head, Re0GcStats *stats);

/* reset every object in the list to WHITE (before a new mark cycle) */
void re0_gc_tracing_reset_colors(Re0GcObject *head);

#endif
