#ifndef RE0_GC_TRACING_H
#define RE0_GC_TRACING_H

#include <stdbool.h>
#include "gc_object.h"
#include "gc_roots.h"
#include "gc_stats.h"
#include "gc_events.h"

/* ── Tracing mark-sweep 算法 ──
 * 三色标记（白/灰/黑）+ 灰色队列（worklist）。
 * 修复了旧实现 mark 不遍历子节点的缺陷。
 *
 * 流程：
 *   1. mark  — 所有 root 标 GRAY 入队，逐个取出标 BLACK，
 *              调用 trace 回调遍历子对象，WHITE→GRAY 入队
 *   2. sweep — 遍历对象链表，WHITE 且可回收的释放，
 *              BLACK→WHITE 重置
 */

/* 回收上下文：collect 时由 engine 填充传入 */
typedef struct {
    Re0GcObject   **head;        /* 对象链表头（sweep 会修改链表） */
    Re0GcRootSet   *roots;       /* 根集 */
    Re0GcStats     *stats;       /* 统计输出 */
    Re0GcListeners *listeners;   /* 事件监听（可为 NULL） */
    Re0GcMode       mode;        /* 引擎模式（事件用） */
    Re0GcAlgo       algo;        /* 引擎算法（事件用） */
} Re0GcTracingCtx;

/* 执行完整的 mark-sweep 回收 */
void re0_gc_tracing_collect(Re0GcTracingCtx *ctx);

/* 仅执行 mark 阶段（并发模式预留：可独立调用） */
/* 执行 mark 阶段。返回 true 表示 mark-stack OOM——
 * 调用方必须放弃本次 sweep，否则会误回收未被标记的可达对象。 */
bool re0_gc_tracing_mark(Re0GcRootSet *roots, Re0GcListeners *ls);

/* 仅执行 sweep 阶段（并发模式预留） */
int  re0_gc_tracing_sweep(Re0GcObject **head, Re0GcStats *stats);

/* 将链表中所有对象重置为 WHITE（新一轮 mark 前调用） */
void re0_gc_tracing_reset_colors(Re0GcObject *head);

#endif
