#ifndef RE0_GC_EVENTS_H
#define RE0_GC_EVENTS_H

#include "gc_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── GC 事件系统（独立于编译器 EventBus） ──
 * GC 引擎在关键节点产生事件，通知已注册的监听器。
 * gc_engine.c 可选地将事件桥接到编译器的 Re0EventBus。
 */

typedef enum {
    RE0_GC_EV_NONE = 0,
    RE0_GC_EV_ALLOC,             /* 新对象分配 */
    RE0_GC_EV_THRESHOLD_REACHED, /* 分配量达到阈值 */
    RE0_GC_EV_COLLECT_START,     /* 回收开始 */
    RE0_GC_EV_COLLECT_DONE,      /* 回收完成 */
    RE0_GC_EV_OBJECT_FREED,      /* 单个对象被释放（verbose 模式） */
    RE0_GC_EV_ENGINE_DESTROY,    /* 引擎销毁 */
} Re0GcEventKind;

typedef struct {
    Re0GcEventKind kind;
    int            alive_count;    /* 事件发生时存活数 */
    size_t         alive_bytes;
    int            freed_count;    /* 本次回收释放数（COLLECT_DONE） */
    size_t         freed_bytes;
    uint64_t       duration_ns;    /* 回收耗时（COLLECT_DONE） */
    Re0GcMode      mode;           /* 引擎当前模式 */
    Re0GcAlgo      algo;           /* 引擎当前算法 */
    const char    *message;        /* 可选附加信息 */
} Re0GcEvent;

/* 事件回调签名 */
typedef void (*Re0GcCallback)(const Re0GcEvent *ev, void *ctx);

/* ── 监听器集合 ── */
typedef struct {
    Re0GcCallback *fns;
    void         **ctxs;
    int            count;
    int            cap;
} Re0GcListeners;

/* 初始化监听器集合 */
void re0_gc_listeners_init(Re0GcListeners *ls);

/* 注册监听器。成功返回 true。 */
bool re0_gc_listeners_add(Re0GcListeners *ls, Re0GcCallback fn, void *ctx);

/* 注销监听器 */
void re0_gc_listeners_remove(Re0GcListeners *ls, Re0GcCallback fn, void *ctx);

/* 向所有监听器广播事件 */
void re0_gc_listeners_emit(Re0GcListeners *ls, const Re0GcEvent *ev);

/* 释放内部数组 */
void re0_gc_listeners_free(Re0GcListeners *ls);

/* ── 便捷事件构造 ── */
Re0GcEvent re0_gc_event_make(Re0GcEventKind kind, Re0GcMode mode, Re0GcAlgo algo);

#endif
