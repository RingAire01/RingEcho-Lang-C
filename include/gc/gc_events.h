#ifndef RE0_GC_EVENTS_H
#define RE0_GC_EVENTS_H

#include "gc_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── GC event system (independent from the compiler EventBus) ──
 * The GC engine emits events at key points to notify registered listeners.
 * gc_engine.c may optionally bridge events into the compiler's Re0EventBus.
 */

typedef enum {
    RE0_GC_EV_NONE = 0,
    RE0_GC_EV_ALLOC,             /* a new object was allocated */
    RE0_GC_EV_THRESHOLD_REACHED, /* allocation count reached the threshold */
    RE0_GC_EV_COLLECT_START,     /* collection started */
    RE0_GC_EV_COLLECT_DONE,      /* collection finished */
    RE0_GC_EV_OBJECT_FREED,      /* a single object was freed (verbose mode) */
    RE0_GC_EV_ENGINE_DESTROY,    /* engine destroyed */
} Re0GcEventKind;

typedef struct {
    Re0GcEventKind kind;
    int            alive_count;    /* survivors at event time */
    size_t         alive_bytes;
    int            freed_count;    /* objects freed this collection (COLLECT_DONE) */
    size_t         freed_bytes;
    uint64_t       duration_ns;    /* collection duration (COLLECT_DONE) */
    Re0GcMode      mode;           /* engine mode at event time */
    Re0GcAlgo      algo;           /* engine algorithm at event time */
    const char    *message;        /* optional extra info */
} Re0GcEvent;

/* event callback signature */
typedef void (*Re0GcCallback)(const Re0GcEvent *ev, void *ctx);

/* ── listener set ── */
typedef struct {
    Re0GcCallback *fns;
    void         **ctxs;
    int            count;
    int            cap;
} Re0GcListeners;

/* initialize a listener set */
void re0_gc_listeners_init(Re0GcListeners *ls);

/* register a listener. returns true on success. */
bool re0_gc_listeners_add(Re0GcListeners *ls, Re0GcCallback fn, void *ctx);

/* unregister a listener */
void re0_gc_listeners_remove(Re0GcListeners *ls, Re0GcCallback fn, void *ctx);

/* broadcast an event to all listeners */
void re0_gc_listeners_emit(Re0GcListeners *ls, const Re0GcEvent *ev);

/* free internal arrays */
void re0_gc_listeners_free(Re0GcListeners *ls);

/* ── convenience event constructor ── */
Re0GcEvent re0_gc_event_make(Re0GcEventKind kind, Re0GcMode mode, Re0GcAlgo algo);

#endif
