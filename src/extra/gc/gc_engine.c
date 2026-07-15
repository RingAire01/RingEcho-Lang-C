#include "gc/gc_engine.h"
#include "gc/gc_tracing.h"
#include "gc/gc_arc.h"
#include "gc/gc_hybrid.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── 内部工具 ── */

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

/* 从引擎链表摘除节点（非 static，供 gc_arc/gc_hybrid 调用） */
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

/* 摘除 + 统计 + 析构 + 释放（arc/hybrid 的标准释放路径） */
void re0_gc_engine_destroy_obj(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    re0_gc_engine_unlink_obj(eng, obj);
    re0_gc_stats_on_free(&eng->stats, obj->size);
    re0_gc_object_destroy(obj);
}

/* NONE 模式：仅清理 ref_count<=0 的悬挂对象，不做 tracing */
static int engine_collect_dangling(Re0GcEngine *eng)
{
    int freed = 0;
    Re0GcObject *obj = eng->head;
    while (obj) {
        Re0GcObject *next = obj->next;
        if (obj->ref_count <= 0 && obj->kind != RE0_PTR_KIND_BORROWED) {
            re0_gc_engine_unlink_obj(eng, obj);
            re0_gc_stats_on_free(&eng->stats, obj->size);
            re0_gc_object_destroy(obj);
            freed++;
        }
        obj = next;
    }
    return freed;
}

static void engine_maybe_auto_collect(Re0GcEngine *eng)
{
    if (eng->config.mode != RE0_GC_MODE_AUTO) return;
    if (eng->alloc_since_gc < eng->next_threshold) return;

    /* 达到阈值，广播事件 */
    Re0GcEvent ev = re0_gc_event_make(
        RE0_GC_EV_THRESHOLD_REACHED, eng->config.mode, eng->config.algo);
    ev.alive_count = eng->stats.alive_count;
    re0_gc_listeners_emit(&eng->listeners, &ev);

    re0_gc_engine_collect(eng);
}

/* ── 公共 API ── */

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

    return eng;
}

void re0_gc_engine_destroy(Re0GcEngine *eng)
{
    if (!eng) return;

    /* 广播销毁事件 */
    Re0GcEvent ev = re0_gc_event_make(
        RE0_GC_EV_ENGINE_DESTROY, eng->config.mode, eng->config.algo);
    ev.alive_count = eng->stats.alive_count;
    ev.alive_bytes = eng->stats.alive_bytes;
    re0_gc_listeners_emit(&eng->listeners, &ev);

    /* 释放所有存活对象（不区分模式） */
    Re0GcObject *obj = eng->head;
    while (obj) {
        Re0GcObject *next = obj->next;
        re0_gc_object_destroy(obj);
        obj = next;
    }

    re0_gc_roots_free(&eng->roots);
    re0_gc_listeners_free(&eng->listeners);
    free(eng);
}

Re0GcObject *re0_gc_engine_alloc(Re0GcEngine *eng, size_t size,
                                  Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (!eng) return NULL;

    /* 先检查是否需要 GC（在 link 新对象之前，避免回收正在分配的对象） */
    engine_maybe_auto_collect(eng);

    Re0GcObject *obj = re0_gc_object_alloc(size, kind, trace, dtor);
    if (!obj) return NULL;

    engine_link(eng, obj);
    re0_gc_stats_on_alloc(&eng->stats, size);
    eng->alloc_since_gc++;

    /* 广播分配事件 */
    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_ALLOC, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    return obj;
}

Re0GcObject *re0_gc_engine_alloc_zero(Re0GcEngine *eng, size_t size,
                                       Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (!eng) return NULL;

    /* 先检查是否需要 GC（同上） */
    engine_maybe_auto_collect(eng);

    Re0GcObject *obj = re0_gc_object_alloc_zero(size, kind, trace, dtor);
    if (!obj) return NULL;

    engine_link(eng, obj);
    re0_gc_stats_on_alloc(&eng->stats, size);
    eng->alloc_since_gc++;

    if (eng->listeners.count > 0) {
        Re0GcEvent ev = re0_gc_event_make(
            RE0_GC_EV_ALLOC, eng->config.mode, eng->config.algo);
        ev.alive_count = eng->stats.alive_count;
        re0_gc_listeners_emit(&eng->listeners, &ev);
    }

    return obj;
}

void re0_gc_engine_free(Re0GcEngine *eng, Re0GcObject *obj)
{
    re0_gc_engine_destroy_obj(eng, obj);
}

char *re0_gc_engine_strdup(Re0GcEngine *eng, const char *s)
{
    if (!eng || !s) return NULL;
    size_t len = strlen(s) + 1;
    Re0GcObject *obj = re0_gc_engine_alloc(
        eng, len, RE0_PTR_KIND_OWNED, NULL, NULL);
    if (!obj) return NULL;
    memcpy(obj->ptr, s, len);
    return (char *)obj->ptr;
}

void re0_gc_engine_retain(Re0GcEngine *eng, Re0GcObject *obj)
{
    (void)eng;
    re0_gc_object_retain(obj);
}

void re0_gc_engine_release(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng || !obj) return;
    re0_gc_object_release(obj);

    if (obj->ref_count > 0 || obj->kind == RE0_PTR_KIND_BORROWED) return;

    if (eng->config.algo == RE0_GC_ALGO_ARC_CYCLE) {
        /* ARC：ref_count==0 立即递归释放子对象链 */
        re0_gc_arc_release_chain(eng, obj);
    } else if (eng->config.algo == RE0_GC_ALGO_HYBRID &&
               re0_gc_hybrid_is_arc_managed(obj)) {
        /* HYBRID：仅 OWNED 对象即时释放，其余等 collect */
        re0_gc_arc_release_chain(eng, obj);
    }
}

bool re0_gc_engine_add_root(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng) return false;
    return re0_gc_roots_add(&eng->roots, obj);
}

bool re0_gc_engine_remove_root(Re0GcEngine *eng, Re0GcObject *obj)
{
    if (!eng) return false;
    return re0_gc_roots_remove(&eng->roots, obj);
}

void re0_gc_engine_collect(Re0GcEngine *eng)
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

    /* 重置分配计数器 */
    eng->alloc_since_gc = 0;

    /* AUTO：动态调整下次阈值 */
    if (eng->config.mode == RE0_GC_MODE_AUTO) {
        int new_th = (int)((float)eng->stats.alive_count * eng->config.gc_factor);
        if (new_th < eng->config.threshold)
            new_th = eng->config.threshold;
        eng->next_threshold = new_th;
    }

    eng->collecting = false;
}

void re0_gc_engine_stats(Re0GcEngine *eng, Re0GcStats *out)
{
    if (!eng || !out) return;
    *out = eng->stats;
}

bool re0_gc_engine_on_event(Re0GcEngine *eng, Re0GcCallback fn, void *ctx)
{
    if (!eng) return false;
    return re0_gc_listeners_add(&eng->listeners, fn, ctx);
}

void re0_gc_engine_set_mode(Re0GcEngine *eng, Re0GcMode mode)
{
    if (!eng) return;
    eng->config.mode = mode;
    if (mode == RE0_GC_MODE_AUTO && eng->next_threshold < eng->config.threshold)
        eng->next_threshold = eng->config.threshold;
}

void re0_gc_engine_set_algo(Re0GcEngine *eng, Re0GcAlgo algo)
{
    if (!eng) return;
    eng->config.algo = algo;
}

void re0_gc_engine_set_verbose(Re0GcEngine *eng, bool verbose)
{
    if (!eng) return;
    eng->config.verbose = verbose;
}
