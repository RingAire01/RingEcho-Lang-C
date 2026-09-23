#ifndef RE0_GC_HYBRID_H
#define RE0_GC_HYBRID_H

/* ════════════════════════════════════════════════════════════
 *  gc_hybrid.h — hybrid GC algorithm
 *
 *  Core design:
 *
 *  - OWNED pointers: ARC immediate-release path
 *    retain/release driven, cascade-release when ref_count==0
 *    suited to clear ownership and well-defined lifetimes
 *
 *  - other pointers (NULLABLE/NONNULL/WEAK): tracing mark-sweep
 *    reclaimed by periodic collect
 *    suited to shared refs, weak refs, uncertain lifetimes
 *
 *  collect runs a unified tracing mark + sweep:
 *    1. mark reachable objects from roots
 *    2. reclaim unreachable (WHITE) objects
 *    3. also handle cycle garbage possibly formed by OWNED objects
 *
 *  Difference from pure ARC: release frees only OWNED objects immediately
 *  Difference from pure TRACING: OWNED objects do not wait for collect
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* whether the object is ARC-managed for immediate release (OWNED kind) */
static inline bool re0_gc_hybrid_is_arc_managed(Re0GcObject *obj)
{
    return obj && obj->kind == RE0_PTR_KIND_OWNED;
}

/* HYBRID collect: tracing mark-sweep + cycle garbage reclamation */
void re0_gc_hybrid_collect(Re0GcEngine *eng);

#endif
