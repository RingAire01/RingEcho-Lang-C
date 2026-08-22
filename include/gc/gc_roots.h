#ifndef RE0_GC_ROOTS_H
#define RE0_GC_ROOTS_H

#include "gc_object.h"
#include <stdbool.h>

/* ── root set management ──
 * Maintains a set of GC root pointers (globals, stack-frame refs, ...).
 * The mark phase walks the object graph starting from this set.
 * A lock field is reserved for phase-4 concurrent marking.
 */
typedef struct {
    Re0GcObject **items;
    int           count;
    int           cap;
    /* void *lock;  -- phase 4: pthread_mutex_t, enabled for concurrent marking */
} Re0GcRootSet;

/* initialize (default capacity) */
void re0_gc_roots_init(Re0GcRootSet *rs);

/* add a root (deduplicated: skipped if already present). true on success. */
bool re0_gc_roots_add(Re0GcRootSet *rs, Re0GcObject *obj);

/* remove a root. true on success. */
bool re0_gc_roots_remove(Re0GcRootSet *rs, Re0GcObject *obj);

/* clear all roots */
void re0_gc_roots_clear(Re0GcRootSet *rs);

/* iterate: call fn(ctx, obj) for each root */
typedef void (*Re0GcRootVisitor)(Re0GcObject *obj, void *ctx);
void re0_gc_roots_foreach(Re0GcRootSet *rs, Re0GcRootVisitor fn, void *ctx);

/* free internal arrays (does not free the pointed-to objects) */
void re0_gc_roots_free(Re0GcRootSet *rs);

/* whether the set is empty */
static inline bool re0_gc_roots_empty(Re0GcRootSet *rs) {
    return !rs || rs->count == 0;
}

#endif
