#ifndef RE0_GC_HYBRID_H
#define RE0_GC_HYBRID_H

/* ════════════════════════════════════════════════════════════
 *  gc_hybrid.h — 混合 GC 算法
 *
 *  核心设计：
 *
 *  - OWNED 指针：走 ARC 即时释放路径
 *    retain/release 驱动，ref_count==0 时立即级联释放
 *    适合生命周期明确、所有权清晰的场景
 *
 *  - 其他指针（NULLABLE/_NONNULL/WEAK）：走 tracing mark-sweep
 *    由周期性 collect 回收
 *    适合共享引用、弱引用、不确定生命周期的场景
 *
 *  collect 时统一做 tracing mark + sweep：
 *    1. 从 roots 出发标记可达对象
 *    2. 回收不可达（WHITE）对象
 *    3. 同时处理 OWNED 对象可能形成的循环垃圾
 *
 *  与纯 ARC 的区别：release 时仅对 OWNED 对象即时释放
 *  与纯 TRACING 的区别：OWNED 对象不需要等 collect
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* 判断对象是否由 ARC 即时管理（OWNED kind） */
static inline bool re0_gc_hybrid_is_arc_managed(Re0GcObject *obj)
{
    return obj && obj->kind == RE0_PTR_KIND_OWNED;
}

/* HYBRID collect：tracing mark-sweep + 循环垃圾回收 */
void re0_gc_hybrid_collect(Re0GcEngine *eng);

#endif
