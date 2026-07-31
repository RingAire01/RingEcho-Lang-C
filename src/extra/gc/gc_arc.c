#include "gc/gc_arc.h"
#include "gc/gc_internal.h"
#include "gc/gc_tracing.h"
#include <stdlib.h>

/* ── 内部：工作栈（迭代式释放，防栈溢出） ── */

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

/* trace visitor：release 子对象，ref_count==0 则标记并入队。
 * 使用 GRAY 标记"已入队"，避免同一对象被多次入队。 */
static void arc_release_visitor(Re0GcObject *child, void *ctx)
{
    ArcWorkQueue *wq = (ArcWorkQueue *)ctx;
    if (!child) return;
    if (child->color == RE0_GC_COLOR_GRAY) return;   /* 已入队 */

    re0_gc_object_release(child);

    if (child->ref_count <= 0 &&
        child->kind != RE0_PTR_KIND_BORROWED) {
        child->color = RE0_GC_COLOR_GRAY;             /* 标记已入队 */
        arc_wq_push(wq, child);
    }
}

/* ════════════════════════════════════════════════════
 *  ARC 即时释放链（迭代式）
 * ════════════════════════════════════════════════════ */

void re0_gc_arc_release_chain(Re0GcEngine *eng, Re0GcObject *start)
{
    if (!eng || !start) return;

    ArcWorkQueue wq = { NULL, 0, 0 };
    if (!arc_wq_push(&wq, start)) return;
    start->color = RE0_GC_COLOR_GRAY;

    while (wq.sp > 0) {
        Re0GcObject *obj = wq.data[--wq.sp];

        /* 遍历子对象：release 引用计数，级联入队 */
        if (obj->trace) {
            obj->trace(obj, arc_release_visitor, &wq);
        }

        /* 释放对象本身 */
        re0_gc_engine_destroy_obj(eng, obj);
    }

    free(wq.data);
}

/* ════════════════════════════════════════════════════
 *  循环检测（tracing backup）
 * ════════════════════════════════════════════════════ */

void re0_gc_arc_collect(Re0GcEngine *eng)
{
    if (!eng) return;

    /* 通知：回收开始 */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_START, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        ev.alive_bytes = eng->stats.alive_bytes;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    /* phase 1：重置颜色 */
    re0_gc_tracing_reset_colors(eng->head);

    /* phase 2：从 roots 出发标记可达对象。
     * mark OOM 时放弃 sweep，避免误回收可达对象（同 C1）。 */
    if (re0_gc_tracing_mark(&eng->roots, &eng->listeners)) {
        if (eng->listeners.count > 0) {
            Re0GcEvent ev = re0_gc_event_make(
                RE0_GC_EV_COLLECT_DONE, eng->config.mode, eng->config.algo);
            ev.freed_count = 0;
            re0_gc_listeners_emit(&eng->listeners, &ev);
        }
        return;
    }

    /* phase 3：回收不可达对象（含循环垃圾和悬挂对象）。
     * ARC 模式下链表中的对象 ref_count 通常 > 0（因为 ==0 的已被即时释放），
     * 但也可能有遗留的悬挂对象，统一回收。 */
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
            obj->color = RE0_GC_COLOR_WHITE;   /* 重置存活对象颜色 */
        }
        obj = next;
    }

    /* 通知：回收完成 */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_COLLECT_DONE, eng->config.mode, eng->config.algo);
        ev.freed_count = freed;
        ev.alive_count = eng->stats.alive_count;
        ev.alive_bytes = eng->stats.alive_bytes;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }
}
