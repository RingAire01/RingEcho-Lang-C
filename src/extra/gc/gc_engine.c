#include "gc/gc_engine.h"
#include "gc/gc_tracing.h"
#include "gc/gc_arc.h"
#include "gc/gc_hybrid.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(RE0_PLATFORM_WINDOWS)
/* Win32 mutex backend (CRITICAL_SECTION: process-local, recursive-capable
 * but we never recurse into it — see header thread-model notes). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *re0_gc_mutex_create(void) {
    CRITICAL_SECTION *cs = (CRITICAL_SECTION*)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return NULL;
    InitializeCriticalSection(cs);
    return cs;
}
void re0_gc_mutex_destroy(void *m) {
    if (m) { DeleteCriticalSection((CRITICAL_SECTION*)m); free(m); }
}
void re0_gc_mutex_lock(void *m) {
    if (m) EnterCriticalSection((CRITICAL_SECTION*)m);
}
void re0_gc_mutex_unlock(void *m) {
    if (m) LeaveCriticalSection((CRITICAL_SECTION*)m);
}
#endif

/* lock helpers (both platforms) */
static inline void gc_lock(Re0GcEngine *eng)
{
#if defined(RE0_PLATFORM_WINDOWS)
    re0_gc_mutex_lock(eng->lock);
#else
    pthread_mutex_lock(&eng->lock);
#endif
}

static inline void gc_unlock(Re0GcEngine *eng)
{
#if defined(RE0_PLATFORM_WINDOWS)
    re0_gc_mutex_unlock(eng->lock);
#else
    pthread_mutex_unlock(&eng->lock);
#endif
}

/* ── internal helpers ── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void engine_link(Re0GcEngine *eng, Re0GcObject *obj)
{
    obj->prev = NULL;
    obj->next = eng->head;
    if (eng->head) eng->head->prev = obj;
    eng->head = obj;
    eng->obj_count++;
}

/* unlink a node from the engine list (non-static: used by gc_arc/gc_hybrid) */
void re0_gc_engine_unlink_obj(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    if (obj->prev) obj->prev->next = obj->next;
    else           eng->head = obj->next;
    if (obj->next) obj->next->prev = obj->prev;
    obj->prev = NULL;
    obj->next = NULL;
    eng->obj_count--;
}

/* unlink + root-set removal + stats + destroy + free (standard release path
 * for arc/hybrid/sweep). Roots are removed in sync: a stale root record
 * would be dereferenced by the next mark. */
void re0_gc_engine_destroy_obj(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    re0_gc_roots_remove(&eng->roots, obj);
    re0_gc_engine_unlink_obj(eng, obj);
    re0_gc_stats_on_free(&eng->stats, obj->size);
    re0_gc_object_destroy(obj);
}

/* NONE mode: clear only ref_count<=0 dangling objects, no tracing */
static int engine_collect_dangling(Re0GcEngine *eng)
{
    int freed = 0;
    Re0GcObject *obj = eng->head;
    while (obj) {
        Re0GcObject *next = obj->next;
        if (obj->ref_count <= 0 && obj->kind != RE0_PTR_KIND_BORROWED) {
            re0_gc_engine_destroy_obj(eng, obj);
            freed++;
        }
        obj = next;
    }
    return freed;
}

/* ── lifecycle ── */

Re0GcEngine *re0_gc_engine_new(Re0GcConfig config)
{
    Re0GcEngine *eng = (Re0GcEngine *)malloc(sizeof(Re0GcEngine));
    if (!eng) return NULL;

    eng->config         = config;
    eng->head           = NULL;
    eng->obj_count      = 0;
    eng->alloc_since_gc = 0;
    eng->next_threshold = config.threshold;
    eng->collecting     = false;

    re0_gc_roots_init(&eng->roots);
    re0_gc_stats_reset(&eng->stats);
    re0_gc_listeners_init(&eng->listeners);

#if defined(RE0_PLATFORM_WINDOWS)
    eng->lock = re0_gc_mutex_create();
    if (!eng->lock) { free(eng); return NULL; }
#else
    if (pthread_mutex_init(&eng->lock, NULL) != 0) { free(eng); return NULL; }
#endif

    return eng;
}

void re0_gc_engine_destroy(Re0GcEngine *eng)
{
    if (!eng) return;

    gc_lock(eng);

    /* broadcast destroy event */
    Re0GcEvent ev = re0_gc_event_make(
        RE0_GC_EV_ENGINE_DESTROY, eng->config.mode, eng->config.algo);
    ev.alive_count = eng->stats.alive_count;
    ev.alive_bytes = eng->stats.alive_bytes;
    re0_gc_listeners_emit(&eng->listeners, &ev);

    /* free all surviving objects regardless of mode */
    Re0GcObject *obj = eng->head;
    while (obj) {
        Re0GcObject *next = obj->next;
        re0_gc_object_destroy(obj);
        obj = next;
    }

    re0_gc_roots_free(&eng->roots);
    re0_gc_listeners_free(&eng->listeners);

    gc_unlock(eng);
#if defined(RE0_PLATFORM_WINDOWS)
    re0_gc_mutex_destroy(eng->lock);
#else
    pthread_mutex_destroy(&eng->lock);
#endif
    free(eng);
}

/* collect kernel: caller must hold the lock. */
static void engine_collect_locked(Re0GcEngine *eng)
{
    if (!eng || eng->collecting) return;

    eng->collecting = true;
    uint64_t start = now_ns();

    int alive_before  = eng->stats.alive_count;
    size_t bytes_before = eng->stats.alive_bytes;

    if (eng->config.mode == RE0_GC_MODE_NONE) {
        engine_collect_dangling(eng);
    } else {
        switch (eng->config.algo) {
            case RE0_GC_ALGO_TRACING: {
                Re0GcTracingCtx ctx;
                ctx.head      = &eng->head;
                ctx.roots     = &eng->roots;
                ctx.stats     = &eng->stats;
                ctx.listeners = &eng->listeners;
                ctx.mode      = eng->config.mode;
                ctx.algo      = eng->config.algo;
                re0_gc_tracing_collect(&ctx);
                break;
            }
            case RE0_GC_ALGO_ARC_CYCLE:
                re0_gc_arc_collect(eng);
                break;
            case RE0_GC_ALGO_HYBRID:
                re0_gc_hybrid_collect(eng);
                break;
        }
    }

    int freed       = alive_before - eng->stats.alive_count;
    size_t freed_bytes = bytes_before - eng->stats.alive_bytes;

    uint64_t elapsed = now_ns() - start;
    re0_gc_stats_on_collect(&eng->stats, freed, freed_bytes, elapsed);

    /* reset allocation counter */
    eng->alloc_since_gc = 0;

    /* AUTO: dynamically adjust the next threshold */
    if (eng->config.mode == RE0_GC_MODE_AUTO) {
        int new_th = (int)((float)eng->stats.alive_count * eng->config.gc_factor);
        if (new_th < eng->config.threshold)
            new_th = eng->config.threshold;
        eng->next_threshold = new_th;
    }

    eng->collecting = false;
}

/* maybe-auto-collect kernel: caller must hold the lock. */
static void engine_maybe_auto_collect_locked(Re0GcEngine *eng)
{
    if (eng->config.mode != RE0_GC_MODE_AUTO) return;
    if (eng->alloc_since_gc < eng->next_threshold) return;

    /* threshold reached: broadcast event */
    Re0GcEvent ev = re0_gc_event_make(
        RE0_GC_EV_THRESHOLD_REACHED, eng->config.mode, eng->config.algo);
    ev.alive_count = eng->stats.alive_count;
    re0_gc_listeners_emit(&eng->listeners, &ev);

    engine_collect_locked(eng);
}

/* ── public API (lock wrapper layer) ── */

Re0GcObject *re0_gc_engine_alloc(Re0GcEngine *eng, size_t size,
                                  Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (!eng) return NULL;

    gc_lock(eng);
    engine_maybe_auto_collect_locked(eng);

    Re0GcObject *obj = re0_gc_object_alloc(size, kind, trace, dtor);
    if (!obj) { gc_unlock(eng); return NULL; }

    engine_link(eng, obj);
    re0_gc_stats_on_alloc(&eng->stats, size);
    eng->alloc_since_gc++;

    /* broadcast alloc event */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_ALLOC, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    gc_unlock(eng);
    return obj;
}

Re0GcObject *re0_gc_engine_alloc_zero(Re0GcEngine *eng, size_t size,
                                       Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (!eng) return NULL;

    gc_lock(eng);
    engine_maybe_auto_collect_locked(eng);

    Re0GcObject *obj = re0_gc_object_alloc_zero(size, kind, trace, dtor);
    if (!obj) { gc_unlock(eng); return NULL; }

    engine_link(eng, obj);
    re0_gc_stats_on_alloc(&eng->stats, size);
    eng->alloc_since_gc++;

    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_ALLOC, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    gc_unlock(eng);
    return obj;
}

void re0_gc_engine_free(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    gc_lock(eng);
    re0_gc_engine_destroy_obj(eng, obj);
    gc_unlock(eng);
}

char *re0_gc_engine_strdup(Re0GcEngine *eng, const char *s)
{
    if (!eng || !s) return NULL;
    size_t len = strlen(s) + 1;
    Re0GcObject *obj = re0_gc_engine_alloc(
        eng, len, RE0_PTR_KIND_OWNED, NULL, NULL);
    if (!obj) return NULL;
    /* The caller only receives the raw payload pointer and cannot root it,
     * so root it here: otherwise the next tracing collect would sweep the
     * string while it is still in use. It stays reachable until the engine
     * is destroyed (or the object is explicitly released via the API). */
    gc_lock(eng);
    if (!re0_gc_roots_add(&eng->roots, obj)) {
        re0_gc_engine_destroy_obj(eng, obj);
        gc_unlock(eng);
        return NULL;
    }
    gc_unlock(eng);
    memcpy(obj->ptr, s, len);
    return (char *)obj->ptr;
}

void re0_gc_engine_retain(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    gc_lock(eng);
    re0_gc_object_retain(obj);
    gc_unlock(eng);
}

void re0_gc_engine_release(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    gc_lock(eng);
    re0_gc_object_release(obj);

    if (obj->ref_count > 0 || obj->kind == RE0_PTR_KIND_BORROWED) {
        gc_unlock(eng);
        return;
    }

    if (eng->config.algo == RE0_GC_ALGO_ARC_CYCLE) {
        /* ARC: ref_count==0 — release the child chain immediately */
        re0_gc_arc_release_chain(eng, obj);
    } else if (eng->config.algo == RE0_GC_ALGO_HYBRID &&
               re0_gc_hybrid_is_arc_managed(obj)) {
        /* HYBRID: only OWNED objects release now, the rest wait for collect */
        re0_gc_arc_release_chain(eng, obj);
    }
    gc_unlock(eng);
}

bool re0_gc_engine_add_root(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng) return false;
    gc_lock(eng);
    bool ok = re0_gc_roots_add(&eng->roots, obj);
    gc_unlock(eng);
    return ok;
}

bool re0_gc_engine_remove_root(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng) return false;
    gc_lock(eng);
    bool ok = re0_gc_roots_remove(&eng->roots, obj);
    gc_unlock(eng);
    return ok;
}

void re0_gc_engine_collect(Re0GcEngine *eng)
{
    if (!eng) return;
    gc_lock(eng);
    engine_collect_locked(eng);
    gc_unlock(eng);
}

void re0_gc_engine_stats(Re0GcEngine *eng, Re0GcStats *out)
{
    if (!eng || !out) return;
    gc_lock(eng);
    *out = eng->stats;
    gc_unlock(eng);
}

bool re0_gc_engine_on_event(Re0GcEngine *eng, Re0GcCallback fn, void *ctx)
{
    if (!eng) return false;
    gc_lock(eng);
    bool ok = re0_gc_listeners_add(&eng->listeners, fn, ctx);
    gc_unlock(eng);
    return ok;
}

void re0_gc_engine_set_mode(Re0GcEngine *eng, Re0GcMode mode)
{
    if (!eng) return;
    gc_lock(eng);
    eng->config.mode = mode;
    if (mode == RE0_GC_MODE_AUTO && eng->next_threshold < eng->config.threshold)
        eng->next_threshold = eng->config.threshold;
    gc_unlock(eng);
}

void re0_gc_engine_set_algo(Re0GcEngine *eng, Re0GcAlgo algo)
{
    if (!eng) return;
    gc_lock(eng);
    eng->config.algo = algo;
    gc_unlock(eng);
}

void re0_gc_engine_set_verbose(Re0GcEngine *eng, bool verbose)
{
    if (!eng) return;
    gc_lock(eng);
    eng->config.verbose = verbose;
    gc_unlock(eng);
}
