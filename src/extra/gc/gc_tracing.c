#include "gc/gc_tracing.h"
#include <stdlib.h>
#include <stdbool.h>

/* ── internal: gray mark queue (mark stack / worklist) ── */
typedef struct {
    Re0GcObject **data;
    int count;
    int cap;
    bool oom;   /* mark stack OOM: skip this sweep, keep live objects */
} GcMarkStack;

static bool ms_push(GcMarkStack *ms, Re0GcObject *obj)
{
    if (ms->oom) return false;
    if (ms->count >= ms->cap) {
        int nc = ms->cap ? ms->cap * 2 : RE0_GC_MARK_STACK_INIT;
        Re0GcObject **nd = (Re0GcObject **)realloc(ms->data,
                                sizeof(Re0GcObject *) * nc);
        if (!nd) { ms->oom = true; return false; }   /* OOM: give up sweep */
        ms->data = nd;
        ms->cap  = nc;
    }
    ms->data[ms->count++] = obj;
    return true;
}

/* trace visitor: child WHITE -> GRAY -> enqueue.
 * Signature-compatible with Re0GcRootVisitor, also usable for root walks.
 */
static void mark_visit(Re0GcObject *child, void *ctx)
{
    if (!child || child->color != RE0_GC_COLOR_WHITE) return;
    child->color = RE0_GC_COLOR_GRAY;
    GcMarkStack *ms = (GcMarkStack *)ctx;
    if (!ms_push(ms, child)) {
        /* OOM: cannot enqueue — revert color to avoid retry storms */
        child->color = RE0_GC_COLOR_WHITE;
    }
}

void re0_gc_tracing_reset_colors(Re0GcObject *head)
{
    while (head) {
        head->color = RE0_GC_COLOR_WHITE;
        head = head->next;
    }
}

bool re0_gc_tracing_mark(Re0GcRootSet *roots, Re0GcListeners *ls)
{
    (void)ls;
    GcMarkStack ms = { NULL, 0, 0, false };

    /* phase 1: roots -> GRAY, enqueue */
    re0_gc_roots_foreach(roots, mark_visit, &ms);

    /* phase 2: drain the gray queue. Abort early on OOM. */
    while (ms.count > 0) {
        Re0GcObject *obj = ms.data[--ms.count];
        obj->color = RE0_GC_COLOR_BLACK;

        /* call the object's trace callback to walk children */
        if (obj->trace) {
            obj->trace(obj, mark_visit, &ms);
        }
        if (ms.oom) break;
    }

    bool oom = ms.oom;
    free(ms.data);
    return oom;   /* true = mark OOM, caller must skip the sweep */
}

int re0_gc_tracing_sweep(Re0GcObject **head, Re0GcStats *stats)
{
    if (!head) return 0;

    int freed_count = 0;
    Re0GcObject *obj = *head;

    while (obj) {
        Re0GcObject *next = obj->next;

        /* tracing mode: mark color only, ref_count is ignored here.
         * ref_count reclamation semantics apply to ARC/HYBRID only.
         * BORROWED pointers are never reclaimed (borrowers do not own). */
        bool collectable = (obj->color == RE0_GC_COLOR_WHITE) &&
                           (obj->kind != RE0_PTR_KIND_BORROWED);

        if (collectable) {
            /* unlink from the doubly-linked list */
            if (obj->prev) obj->prev->next = obj->next;
            else           *head = obj->next;
            if (obj->next) obj->next->prev = obj->prev;

            /* stats + free */
            if (stats) re0_gc_stats_on_free(stats, obj->size);
            re0_gc_object_destroy(obj);
            freed_count++;
        } else {
            /* reset surviving objects to WHITE for the next cycle */
            obj->color = RE0_GC_COLOR_WHITE;
        }
        obj = next;
    }
    return freed_count;
}

void re0_gc_tracing_collect(Re0GcTracingCtx *ctx)
{
    if (!ctx || !ctx->head || !ctx->roots) return;

    /* defensive: ensure a clean starting state */
    re0_gc_tracing_reset_colors(*ctx->head);

    /* notify: collection start */
    if (ctx->listeners) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_START, ctx->mode, ctx->algo);
        ev.alive_count = ctx->stats ? (int)ctx->stats->alive_count : 0;
        ev.alive_bytes = ctx->stats ? ctx->stats->alive_bytes : 0;
        re0_gc_listeners_emit(ctx->listeners, &ev);
    }

    /* mark from roots -> sweep.
     * If the mark stack hits OOM, this sweep must be abandoned —
     * otherwise unmarked-but-reachable objects would be reclaimed,
     * causing use-after-free (C1 fix). */
    bool mark_oom = re0_gc_tracing_mark(ctx->roots, ctx->listeners);
    if (mark_oom) {
        if (ctx->listeners) {
            Re0GcEvent ev = re0_gc_event_make(
                RE0_GC_EV_COLLECT_DONE, ctx->mode, ctx->algo);
            ev.freed_count = 0;
            re0_gc_listeners_emit(ctx->listeners, &ev);
        }
        return;
    }
    int freed = re0_gc_tracing_sweep(ctx->head, ctx->stats);

    /* notify: collection done */
    if (ctx->listeners) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_DONE, ctx->mode, ctx->algo);
        if (ctx->stats) {
            ev.alive_count  = (int)ctx->stats->alive_count;
            ev.alive_bytes  = ctx->stats->alive_bytes;
        }
        ev.freed_count = freed;
        re0_gc_listeners_emit(ctx->listeners, &ev);
    }
}
