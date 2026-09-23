#ifndef RE0_GC_H
#define RE0_GC_H

/* ════════════════════════════════════════════════════════════
 *  re0_gc.h — GC subsystem aggregate header + backward-compat layer
 *
 *  New code should use the modular interfaces under gc/ directly:
 *    #include "gc/gc_engine.h"
 *
 *  This file keeps the legacy API aliases so existing callers
 *  compile unchanged.
 * ════════════════════════════════════════════════════════════ */

#include "gc/gc_config.h"
#include "gc/gc_object.h"
#include "gc/gc_roots.h"
#include "gc/gc_tracing.h"
#include "gc/gc_events.h"
#include "gc/gc_stats.h"
#include "gc/gc_engine.h"

/* ── type aliases ── */
typedef Re0GcEngine  Re0GcPool;   /* legacy GcPool -> new GcEngine */
typedef Re0GcObject  Re0GcNode;   /* legacy GcNode -> new GcObject */

/* ── legacy enum-name aliases ── */
#define RE0_GC_NONE   RE0_GC_MODE_NONE
#define RE0_GC_AUTO   RE0_GC_MODE_AUTO
#define RE0_GC_MANUAL RE0_GC_MODE_MANUAL

/* ── compat helpers: create an engine in NONE mode ── */

static inline int re0_gc_mode_to_int(Re0GcMode m)
{
    return (int)m;
}

static inline Re0GcPool *re0_gc_new(int threshold)
{
    Re0GcConfig c = re0_gc_config_default();
    c.threshold = threshold > 0 ? threshold : RE0_GC_DEFAULT_THRESHOLD;
    return re0_gc_engine_new(c);
}

static inline void re0_gc_destroy(Re0GcPool *gc)
{
    re0_gc_engine_destroy(gc);
}

static inline void re0_gc_collect(Re0GcPool *gc)
{
    re0_gc_engine_collect(gc);
}

/* ── compat: Re0Ptr wrapper ── */
typedef struct {
    Re0GcObject *node;
    bool nullable;
} Re0Ptr;

static inline void *re0_ptr_unwrap(Re0Ptr p)
{
    return p.node ? p.node->ptr : NULL;
}

static inline void *re0_ptr_unwrap_or(Re0Ptr p, void *fallback)
{
    return p.node ? p.node->ptr : fallback;
}

static inline Re0Ptr re0_ptr_bind(Re0GcObject *node, bool nullable)
{
    Re0Ptr p;
    p.node     = node;
    p.nullable = nullable;
    return p;
}

#endif
