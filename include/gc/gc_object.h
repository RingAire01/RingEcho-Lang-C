#ifndef RE0_GC_OBJECT_H
#define RE0_GC_OBJECT_H

#include "gc_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── forward declarations ── */
typedef struct Re0GcObject Re0GcObject;

/* trace visitor: called for each child while walking children */
typedef void (*Re0GcTraceVisitor)(Re0GcObject *child, void *ctx);

/* trace callback: the object knows how to walk its child pointers,
 * calling visit(child, ctx) for each */
typedef void (*Re0GcTraceFn)(Re0GcObject *self, Re0GcTraceVisitor visit, void *ctx);

/* destructor callback: invoked before freeing user data, may clean up
 * internal resources */
typedef void (*Re0GcDtorFn)(void *ptr, size_t size);

/* ── object-graph node ──
 * Each GC-tracked heap allocation maps to one GcObject metadata node.
 * The node and the user data are allocated separately so the metadata
 * is never freed by mistake.
 */
struct Re0GcObject {
    void         *ptr;         /* user data pointer */
    size_t        size;        /* user data size in bytes */
    Re0PtrKind    kind;        /* pointer semantics */

    /* reference count — used by the ARC algorithm; advisory under tracing */
    int32_t       ref_count;

    /* tri-color marking — tracing + concurrency reserved */
    uint8_t       color;       /* RE0_GC_COLOR_WHITE/GRAY/BLACK */

    /* callbacks */
    Re0GcTraceFn  trace;       /* walk children (NULL = leaf node) */
    Re0GcDtorFn   dtor;        /* destructor (NULL = nothing to clean) */

    /* doubly-linked list — hung on the GcEngine, O(1) insert/remove */
    Re0GcObject  *prev;
    Re0GcObject  *next;
};

/* ── lifecycle ── */

/* create a metadata node + allocate user data. returns NULL on failure. */
Re0GcObject *re0_gc_object_alloc(size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* same, but the user data region is zero-filled */
Re0GcObject *re0_gc_object_alloc_zero(size_t size, Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* create only the metadata node; user data provided by the caller (attach mode) */
Re0GcObject *re0_gc_object_wrap(void *ptr, size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* destroy + free user data + free the node itself */
void re0_gc_object_destroy(Re0GcObject *obj);

/* destroy + free user data only, keep the node (sweep unlinks first) */
void re0_gc_object_free_payload(Re0GcObject *obj);

/* reset the mark color to WHITE (before a new GC cycle) */
static inline void re0_gc_object_set_white(Re0GcObject *o) {
    if (o) o->color = RE0_GC_COLOR_WHITE;
}

/* whether the object is alive (BLACK or GRAY) */
static inline bool re0_gc_object_is_alive(Re0GcObject *o) {
    return o && o->color != RE0_GC_COLOR_WHITE;
}

/* reference-count operations */
void re0_gc_object_retain(Re0GcObject *o);
void re0_gc_object_release(Re0GcObject *o);

/* convenience wrapper to get the user data pointer */
static inline void *re0_gc_object_data(Re0GcObject *o) {
    return o ? o->ptr : NULL;
}

#endif
