#ifndef RE0_GC_OBJECT_H
#define RE0_GC_OBJECT_H

#include "gc_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── 前向声明 ── */
typedef struct Re0GcObject Re0GcObject;

/* trace 访问器：遍历子对象时对每个子对象调用 */
typedef void (*Re0GcTraceVisitor)(Re0GcObject *child, void *ctx);

/* trace 回调：对象知道如何遍历自身的子指针，调用 visit(child, ctx) */
typedef void (*Re0GcTraceFn)(Re0GcObject *self, Re0GcTraceVisitor visit, void *ctx);

/* 析构回调：在释放用户数据前调用，可清理内部资源 */
typedef void (*Re0GcDtorFn)(void *ptr, size_t size);

/* ── 对象图节点 ──
 * 每个 GC 跟踪的堆分配对应一个 GcObject 元数据节点。
 * 节点本身和用户数据分别分配，避免元数据被误释放。
 */
struct Re0GcObject {
    void         *ptr;         /* 用户数据指针 */
    size_t        size;        /* 用户数据大小（字节） */
    Re0PtrKind    kind;        /* 指针语义 */

    /* 引用计数 — ARC 算法使用；tracing 模式下仅做统计参考 */
    int32_t       ref_count;

    /* 三色标记 — tracing + 并发预留 */
    uint8_t       color;       /* RE0_GC_COLOR_WHITE/GRAY/BLACK */

    /* 回调 */
    Re0GcTraceFn  trace;       /* 遍历子对象（NULL 表示叶子节点） */
    Re0GcDtorFn   dtor;        /* 析构（NULL 表示无需清理） */

    /* 双向链表 — 挂在 GcEngine 上，O(1) 插入/删除 */
    Re0GcObject  *prev;
    Re0GcObject  *next;
};

/* ── 生命周期 ── */

/* 创建元数据节点 + 分配用户数据。失败返回 NULL。 */
Re0GcObject *re0_gc_object_alloc(size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* 同上，但用户数据区域清零 */
Re0GcObject *re0_gc_object_alloc_zero(size_t size, Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* 仅创建元数据节点，用户数据由调用者提供（attach 模式） */
Re0GcObject *re0_gc_object_wrap(void *ptr, size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor);

/* 析构 + 释放用户数据 + 释放节点本身 */
void re0_gc_object_destroy(Re0GcObject *obj);

/* 仅析构 + 释放用户数据，不释放节点（sweep 时链表操作在前） */
void re0_gc_object_free_payload(Re0GcObject *obj);

/* 重置标记颜色为 WHITE（新一轮 GC 前调用） */
static inline void re0_gc_object_set_white(Re0GcObject *o) {
    if (o) o->color = RE0_GC_COLOR_WHITE;
}

/* 判断对象是否存活（BLACK 或 GRAY） */
static inline bool re0_gc_object_is_alive(Re0GcObject *o) {
    return o && o->color != RE0_GC_COLOR_WHITE;
}

/* 引用计数操作 */
void re0_gc_object_retain(Re0GcObject *o);
void re0_gc_object_release(Re0GcObject *o);

/* 获取用户数据指针的便捷包装 */
static inline void *re0_gc_object_data(Re0GcObject *o) {
    return o ? o->ptr : NULL;
}

#endif
