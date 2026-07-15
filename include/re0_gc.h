#ifndef RE0_GC_H
#define RE0_GC_H

/* ════════════════════════════════════════════════════════════
 *  re0_gc.h — GC 子系统聚合头文件 + 向后兼容层
 *
 *  新代码应直接使用 gc/ 下的模块化接口：
 *    #include "gc/gc_engine.h"
 *
 *  本文件保留旧 API 别名，确保已有调用方无需修改。
 * ════════════════════════════════════════════════════════════ */

#include "gc/gc_config.h"
#include "gc/gc_object.h"
#include "gc/gc_roots.h"
#include "gc/gc_tracing.h"
#include "gc/gc_events.h"
#include "gc/gc_stats.h"
#include "gc/gc_engine.h"

/* ── 类型别名 ── */
typedef Re0GcEngine  Re0GcPool;   /* 旧 GcPool → 新 GcEngine */
typedef Re0GcObject  Re0GcNode;   /* 旧 GcNode → 新 GcObject */

/* ── 旧枚举值名兼容 ── */
#define RE0_GC_NONE   RE0_GC_MODE_NONE
#define RE0_GC_AUTO   RE0_GC_MODE_AUTO
#define RE0_GC_MANUAL RE0_GC_MODE_MANUAL

/* ── 兼容函数：以 NONE 模式创建引擎 ── */

static inline int re0_gc_mode_to_int(Re0GcMode m)
{
    return (int)m;
}

static inline Re0GcPool *re0_gc_new(int threshold)
{
    Re0GcConfig c = re0_gc_config_default();
    c.threshold = threshold > 0 ? threshold : RE0_GC_DEFAULT_THRESHOLD;
    return re0_gc_engine_new(c);
}

static inline void re0_gc_destroy(Re0GcPool *gc)
{
    re0_gc_engine_destroy(gc);
}

static inline void re0_gc_collect(Re0GcPool *gc)
{
    re0_gc_engine_collect(gc);
}

/* ── 兼容：Re0Ptr 包装 ── */
typedef struct {
    Re0GcObject *node;
    bool nullable;
} Re0Ptr;

static inline void *re0_ptr_unwrap(Re0Ptr p)
{
    return p.node ? p.node->ptr : NULL;
}

static inline void *re0_ptr_unwrap_or(Re0Ptr p, void *fallback)
{
    return p.node ? p.node->ptr : fallback;
}

static inline Re0Ptr re0_ptr_bind(Re0GcObject *node, bool nullable)
{
    Re0Ptr p;
    p.node     = node;
    p.nullable = nullable;
    return p;
}

#endif
