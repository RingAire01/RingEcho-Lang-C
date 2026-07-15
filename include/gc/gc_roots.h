#ifndef RE0_GC_ROOTS_H
#define RE0_GC_ROOTS_H

#include "gc_object.h"
#include <stdbool.h>

/* ── 根集管理 ──
 * 维护一组 GC root 指针（全局变量、栈帧引用等）。
 * mark 阶段从此集合出发遍历对象图。
 * 预留锁字段，阶段4并发标记时启用。
 */
typedef struct {
    Re0GcObject **items;
    int           count;
    int           cap;
    /* void *lock;  -- 阶段4：pthread_mutex_t，并发标记时启用 */
} Re0GcRootSet;

/* 初始化（使用默认容量） */
void re0_gc_roots_init(Re0GcRootSet *rs);

/* 添加 root（去重：已存在则跳过）。成功返回 true。 */
bool re0_gc_roots_add(Re0GcRootSet *rs, Re0GcObject *obj);

/* 移除 root。成功返回 true。 */
bool re0_gc_roots_remove(Re0GcRootSet *rs, Re0GcObject *obj);

/* 清空所有 root */
void re0_gc_roots_clear(Re0GcRootSet *rs);

/* 遍历：对每个 root 调用 fn(ctx, obj) */
typedef void (*Re0GcRootVisitor)(Re0GcObject *obj, void *ctx);
void re0_gc_roots_foreach(Re0GcRootSet *rs, Re0GcRootVisitor fn, void *ctx);

/* 释放内部数组（不释放 root 指向的对象） */
void re0_gc_roots_free(Re0GcRootSet *rs);

/* 判断是否为空 */
static inline bool re0_gc_roots_empty(Re0GcRootSet *rs) {
    return !rs || rs->count == 0;
}

#endif
