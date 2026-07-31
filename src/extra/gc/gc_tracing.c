#include "gc/gc_tracing.h"
#include <stdlib.h>
#include <stdbool.h>

/* ── 内部：灰色标记队列（mark stack / worklist） ── */
typedef struct {
    Re0GcObject **data;
    int count;
    int cap;
    bool oom;   /* mark 栈 OOM：放弃本次 sweep，防误回收活对象 */
} GcMarkStack;

static bool ms_push(GcMarkStack *ms, Re0GcObject *obj)
{
    if (ms->oom) return false;
    if (ms->count >= ms->cap) {
        int nc = ms->cap ? ms->cap * 2 : RE0_GC_MARK_STACK_INIT;
        Re0GcObject **nd = (Re0GcObject **)realloc(ms->data,
                                sizeof(Re0GcObject *) * nc);
        if (!nd) { ms->oom = true; return false; }   /* OOM：放弃 sweep */
        ms->data = nd;
        ms->cap  = nc;
    }
    ms->data[ms->count++] = obj;
    return true;
}

/* trace visitor：子对象 WHITE → GRAY → 入队
 * 签名兼容 Re0GcRootVisitor，可同时用于 root 遍历。
 */
static void mark_visit(Re0GcObject *child, void *ctx)
{
    if (!child || child->color != RE0_GC_COLOR_WHITE) return;
    child->color = RE0_GC_COLOR_GRAY;
    GcMarkStack *ms = (GcMarkStack *)ctx;
    if (!ms_push(ms, child)) {
        /* OOM：无法入队，回退颜色避免重复尝试 */
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

    /* phase 1：roots → GRAY 入队 */
    re0_gc_roots_foreach(roots, mark_visit, &ms);

    /* phase 2：处理灰色队列。OOM 则提前终止。 */
    while (ms.count > 0) {
        Re0GcObject *obj = ms.data[--ms.count];
        obj->color = RE0_GC_COLOR_BLACK;

        /* 调用对象的 trace 回调遍历子对象 */
        if (obj->trace) {
            obj->trace(obj, mark_visit, &ms);
        }
        if (ms.oom) break;
    }

    bool oom = ms.oom;
    free(ms.data);
    return oom;   /* true = mark OOM，调用方应放弃 sweep */
}

int re0_gc_tracing_sweep(Re0GcObject **head, Re0GcStats *stats)
{
    if (!head) return 0;

    int freed_count = 0;
    Re0GcObject *obj = *head;

    while (obj) {
        Re0GcObject *next = obj->next;

        /* tracing 模式：仅看 mark color，不看 ref_count。
         * ref_count 的回收语义仅用于 ARC/HYBRID 模式。
         * BORROWED 指针永远不回收（借用者不拥有生命周期）。 */
        bool collectable = (obj->color == RE0_GC_COLOR_WHITE) &&
                           (obj->kind != RE0_PTR_KIND_BORROWED);

        if (collectable) {
            /* 从双向链表摘除 */
            if (obj->prev) obj->prev->next = obj->next;
            else           *head = obj->next;
            if (obj->next) obj->next->prev = obj->prev;

            /* 统计 + 释放 */
            if (stats) re0_gc_stats_on_free(stats, obj->size);
            re0_gc_object_destroy(obj);
            freed_count++;
        } else {
            /* 存活对象重置为 WHITE，为下一轮准备 */
            obj->color = RE0_GC_COLOR_WHITE;
        }
        obj = next;
    }
    return freed_count;
}

void re0_gc_tracing_collect(Re0GcTracingCtx *ctx)
{
    if (!ctx || !ctx->head || !ctx->roots) return;

    /* 防御：确保从干净状态开始 */
    re0_gc_tracing_reset_colors(*ctx->head);

    /* 通知：回收开始 */
    if (ctx->listeners) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_START, ctx->mode, ctx->algo);
        ev.alive_count = ctx->stats ? (int)ctx->stats->alive_count : 0;
        ev.alive_bytes = ctx->stats ? ctx->stats->alive_bytes : 0;
        re0_gc_listeners_emit(ctx->listeners, &ev);
    }

    /* mark from roots → sweep.
     * 若 mark 期间 mark-stack OOM，必须放弃本次 sweep——否则未被
     * 标记的可达对象会被误回收，导致 use-after-free（C1 修复）。 */
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

    /* 通知：回收完成 */
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
