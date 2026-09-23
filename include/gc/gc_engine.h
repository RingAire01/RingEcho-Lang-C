#ifndef RE0_GC_ENGINE_H
#define RE0_GC_ENGINE_H

#include "gc_config.h"
#include "gc_object.h"
#include "gc_roots.h"
#include "gc_stats.h"
#include "gc_events.h"
#include "platform.h"
#include <stddef.h>
#include <stdbool.h>

#if defined(RE0_PLATFORM_WINDOWS)
/* avoid pulling windows.h into every consumer */
void *re0_gc_mutex_create(void);
void  re0_gc_mutex_destroy(void *m);
void  re0_gc_mutex_lock(void *m);
void  re0_gc_mutex_unlock(void *m);
#else
#include <pthread.h>
#endif

/* ════════════════════════════════════════════════════════════
 *  Re0GcEngine — GC engine facade
 *
 *  Unified heap object lifecycle management: 3 modes x 3 algorithms.
 *
 *  mode (when to collect):
 *    NONE   — no automatic reclamation; manual free / engine destroy as backstop
 *    AUTO   — auto mark-sweep when allocations reach the threshold
 *    MANUAL — only triggered explicitly via collect()
 *
 *  algo (how to collect):
 *    TRACING   — mark-sweep walking the object graph from roots (fully implemented)
 *    ARC_CYCLE — reference counting + cycle detection (interface reserved)
 *    HYBRID    — OWNED instant release + tracing for the rest (interface reserved)
 *
 *  All key operations broadcast events via Re0GcListeners.
 *
 *  Thread model (important):
 *    Every public API is serialized by an internal mutex (coarse-grained,
 *    correctness first). All APIs may be called from any thread, but
 *    operations on a single engine instance never run concurrently —
 *    no concurrent-read path; throughput traded for correctness.
 *    Callbacks (trace/dtor/listener) execute under the lock: callbacks
 *    must not call other APIs of the same engine (self-deadlock); the
 *    GRAY reentry guard of release_chain also covers cross-chain
 *    releases issued from dtors.
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    Re0GcConfig    config;          /* runtime configuration */
    Re0GcObject   *head;            /* object list head */
    int            obj_count;       /* objects in list */
    Re0GcRootSet   roots;           /* GC root set */
    Re0GcStats     stats;           /* cumulative statistics */
    Re0GcListeners listeners;       /* event listeners */
    int            alloc_since_gc;  /* allocations since last collection */
    int            next_threshold;  /* next AUTO trigger threshold */
    bool           collecting;      /* collection in progress (reentry guard) */
#if defined(RE0_PLATFORM_WINDOWS)
    void          *lock;            /* CRITICAL_SECTION* */
#else
    pthread_mutex_t lock;           /* coarse-grained global mutex */
#endif
} Re0GcEngine;

/* ── lifecycle ── */

/* Create an engine. config selects mode/algo/threshold. */
Re0GcEngine *re0_gc_engine_new(Re0GcConfig config);

/* Destroy the engine: frees all surviving objects, roots, listeners. */
void re0_gc_engine_destroy(Re0GcEngine *eng);

/* ── allocation / free ── */

/* Allocate a GC-tracked object.
 * Under AUTO mode a collection is triggered when the threshold is exceeded.
 * The returned object is linked into the engine list with ref_count = 1.
 */
Re0GcObject *re0_gc_engine_alloc(Re0GcEngine *eng, size_t size,
                                  Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* Allocate and zero-fill */
Re0GcObject *re0_gc_engine_alloc_zero(Re0GcEngine *eng, size_t size,
                                       Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* Manually free a single object (list unlink + destroy + free).
 * Immediate under NONE/MANUAL; also allowed under AUTO.
 */
void re0_gc_engine_free(Re0GcEngine *eng, Re0GcObject *obj);

/* strdup convenience: allocate a GC-tracked string */
char *re0_gc_engine_strdup(Re0GcEngine *eng, const char *s);

/* ── reference counting (ARC algorithm) ── */
void re0_gc_engine_retain(Re0GcEngine *eng, Re0GcObject *obj);
void re0_gc_engine_release(Re0GcEngine *eng, Re0GcObject *obj);

/* ── root set management ── */
bool re0_gc_engine_add_root(Re0GcEngine *eng, Re0GcObject *obj);
bool re0_gc_engine_remove_root(Re0GcEngine *eng, Re0GcObject *obj);

/* ── collection ── */

/* Trigger a collection explicitly.
 * Main entry under MANUAL mode; also usable under AUTO.
 * Under NONE mode only frees ref_count <= 0 dangling objects.
 */
void re0_gc_engine_collect(Re0GcEngine *eng);

/* ── statistics / events ── */

/* Snapshot the current statistics */
void re0_gc_engine_stats(Re0GcEngine *eng, Re0GcStats *out);

/* Register an event listener */
bool re0_gc_engine_on_event(Re0GcEngine *eng, Re0GcCallback fn, void *ctx);

/* ── configuration ── */

/* Switch mode at runtime */
void re0_gc_engine_set_mode(Re0GcEngine *eng, Re0GcMode mode);

/* Switch algorithm at runtime */
void re0_gc_engine_set_algo(Re0GcEngine *eng, Re0GcAlgo algo);

/* Set verbose (print GC events to stderr) */
void re0_gc_engine_set_verbose(Re0GcEngine *eng, bool verbose);

#endif
