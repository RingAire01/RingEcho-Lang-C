#ifndef RE0_GC_ENGINE_H
#define RE0_GC_ENGINE_H

#include "gc_config.h"
#include "gc_object.h"
#include "gc_roots.h"
#include "gc_stats.h"
#include "gc_events.h"
#include <stddef.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════════════════
 *  Re0GcEngine — GC 引擎门面
 *
 *  统一管理堆对象的生命周期，支持三种模式 × 三种算法。
 *
 *  mode（何时回收）：
 *    NONE   — 不自动回收，依赖手动 free / engine destroy 兜底
 *    AUTO   — 分配量达阈值时自动 mark-sweep
 *    MANUAL — 仅 collect() API 显式触发
 *
 *  algo（如何回收）：
 *    TRACING   — mark-sweep 从 roots 遍历对象图（本次完整实现）
 *    ARC_CYCLE — 引用计数 + 循环检测（接口预留）
 *    HYBRID    — OWNED 即时释放 + 其余 tracing（接口预留）
 *
 *  所有关键操作通过 Re0GcListeners 广播事件。
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    Re0GcConfig    config;          /* 运行时配置 */
    Re0GcObject   *head;            /* 对象链表头 */
    int            obj_count;       /* 链表对象计数 */
    Re0GcRootSet   roots;           /* GC 根集 */
    Re0GcStats     stats;           /* 累计统计 */
    Re0GcListeners listeners;       /* 事件监听器 */
    int            alloc_since_gc;  /* 自上次回收后的分配计数 */
    int            next_threshold;  /* AUTO 模式下次触发阈值 */
    bool           collecting;      /* 正在回收（防重入） */
} Re0GcEngine;

/* ── 生命周期 ── */

/* 创建引擎。config 决定 mode/algo/threshold。 */
Re0GcEngine *re0_gc_engine_new(Re0GcConfig config);

/* 销毁引擎：释放所有存活对象、根集、监听器。 */
void re0_gc_engine_destroy(Re0GcEngine *eng);

/* ── 分配 / 释放 ── */

/* 分配一个 GC 跟踪对象。
 * AUTO 模式下，超阈值时自动触发回收。
 * 返回的对象已挂入引擎链表，ref_count = 1（OWNED/BORROWED）或 0。
 */
Re0GcObject *re0_gc_engine_alloc(Re0GcEngine *eng, size_t size,
                                  Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* 分配并清零 */
Re0GcObject *re0_gc_engine_alloc_zero(Re0GcEngine *eng, size_t size,
                                       Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* 手动释放单个对象（从链表移除 + 析构 + free）。
 * NONE/MANUAL 模式下即时生效；AUTO 模式也可手动释放。
 */
void re0_gc_engine_free(Re0GcEngine *eng, Re0GcObject *obj);

/* strdup 便捷接口：分配 GC 跟踪的字符串 */
char *re0_gc_engine_strdup(Re0GcEngine *eng, const char *s);

/* ── 引用计数（ARC 算法使用） ── */
void re0_gc_engine_retain(Re0GcEngine *eng, Re0GcObject *obj);
void re0_gc_engine_release(Re0GcEngine *eng, Re0GcObject *obj);

/* ── 根集管理 ── */
bool re0_gc_engine_add_root(Re0GcEngine *eng, Re0GcObject *obj);
bool re0_gc_engine_remove_root(Re0GcEngine *eng, Re0GcObject *obj);

/* ── 回收 ── */

/* 显式触发回收。
 * MANUAL 模式的主要入口；AUTO 模式也可手动触发。
 * NONE 模式仅释放 ref_count <= 0 的悬挂对象。
 */
void re0_gc_engine_collect(Re0GcEngine *eng);

/* ── 统计 / 事件 ── */

/* 获取当前统计快照 */
void re0_gc_engine_stats(Re0GcEngine *eng, Re0GcStats *out);

/* 注册事件监听器 */
bool re0_gc_engine_on_event(Re0GcEngine *eng, Re0GcCallback fn, void *ctx);

/* ── 配置 ── */

/* 运行时切换模式 */
void re0_gc_engine_set_mode(Re0GcEngine *eng, Re0GcMode mode);

/* 运行时切换算法 */
void re0_gc_engine_set_algo(Re0GcEngine *eng, Re0GcAlgo algo);

/* 设置 verbose（打印 GC 事件到 stderr） */
void re0_gc_engine_set_verbose(Re0GcEngine *eng, bool verbose);

#endif
