#include "gc/gc_arc.h"
#include "gc/gc_internal.h"
#include "gc/gc_tracing.h"
#include <stdlib.h>

/* ── internal: work stack (iterative release, stack-overflow safe) ── */

#define ARC_WQ_INIT_CAP 32

typedef struct {
    Re0GcObject **data;
    int sp;
    int cap;
} ArcWorkQueue;

static bool arc_wq_push(ArcWorkQueue *wq, Re0GcObject *obj)
{
    if (wq->sp >= wq->cap) {
        int nc = wq->cap ? wq->cap * 2 : ARC_WQ_INIT_CAP;
        Re0GcObject **nd = (Re0GcObject **)realloc(
            wq->data, sizeof(Re0GcObject *) * nc);
        if (!nd) return false;
        wq->data = nd;
        wq->cap  = nc;
    }
    wq->data[wq->sp++] = obj;
    return true;
}

/* trace visitor: release child objects; mark and enqueue when ref_count==0.
 * GRAY marks "already queued" so an object is never enqueued twice. */
static void arc_release_visitor(Re0GcObject *child, void *ctx)
{
    ArcWorkQueue *wq = (ArcWorkQueue *)ctx;
    if (!child) return;
    if (child->color == RE0_GC_COLOR_GRAY) return;   /* already queued */

    re0_gc_object_release(child);

    if (child->ref_count <= 0 &&
        child->kind != RE0_PTR_KIND_BORROWED) {
        child->color = RE0_GC_COLOR_GRAY;             /* mark as queued */
        arc_wq_push(wq, child);
    }
}

/* ════════════════════════════════════════════════════
 *  ARC immediate release chain (iterative)
 * ════════════════════════════════════════════════════ */

void re0_gc_arc_release_chain(Re0GcEngine *eng, Re0GcObject *start)
{
    if (!eng || !start) return;

    /* Reentry guard: GRAY means "already queued in an ongoing release
     * chain". Destroying it here would double-free when the outer chain
     * pops it; re-releasing it would corrupt the in-flight traversal.
     * Typical trigger: a dtor releases a member that another dtor in the
     * same chain has already queued. */
    if (start->color == RE0_GC_COLOR_GRAY) return;

    ArcWorkQueue wq = { NULL, 0, 0 };
    if (!arc_wq_push(&wq, start)) return;
    start->color = RE0_GC_COLOR_GRAY;

    while (wq.sp > 0) {
        Re0GcObject *obj = wq.data[--wq.sp];

        /* trace children: release ref counts, cascade-enqueue */
        if (obj->trace) {
            obj->trace(obj, arc_release_visitor, &wq);
        }

        /* free the object itself */
        re0_gc_engine_destroy_obj(eng, obj);
    }

    free(wq.data);
}

/* ════════════════════════════════════════════════════
 *  cycle detection (tracing backup)
 * ════════════════════════════════════════════════════ */

void re0_gc_arc_collect(Re0GcEngine *eng)
{
    if (!eng) return;

    /* notify: collection start */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_START, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        ev.alive_bytes = eng->stats.alive_bytes;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    /* phase 1: reset colors */
    re0_gc_tracing_reset_colors(eng->head);

    /* phase 2: mark reachable objects from roots.
     * On mark OOM skip the sweep to avoid reclaiming live objects. */
    if (re0_gc_tracing_mark(&eng->roots, &eng->listeners)) {
        if (eng->listeners.count > 0) {
            Re0GcEvent ev = re0_gc_event_make(
                RE0_GC_EV_COLLECT_DONE, eng->config.mode, eng->config.algo);
            ev.freed_count = 0;
            re0_gc_listeners_emit(&eng->listeners, &ev);
        }
        return;
    }

    /* phase 3: reclaim unreachable objects (cycles and dangling).
     * Under ARC most list objects still have ref_count > 0 (zero-count
     * ones were released immediately), but leftover dangling objects
     * may exist — sweep them all. */
    int freed = 0;
    Re0GcObject *obj = eng->head;
    while (obj) {
        Re0GcObject *next = obj->next;

        bool collectable = (obj->color == RE0_GC_COLOR_WHITE) &&
                           (obj->kind != RE0_PTR_KIND_BORROWED);

        if (collectable) {
            re0_gc_engine_destroy_obj(eng, obj);
            freed++;
        } else {
            obj->color = RE0_GC_COLOR_WHITE;   /* reset surviving object color */
        }
        obj = next;
    }

    /* notify: collection done */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_DONE, eng->config.mode, eng->config.algo);
        ev.freed_count = freed;
        ev.alive_count = eng->stats.alive_count;
        ev.alive_bytes = eng->stats.alive_bytes;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }
}
