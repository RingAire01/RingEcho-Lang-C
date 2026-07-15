#ifndef RE0_GC_INTERNAL_H
#define RE0_GC_INTERNAL_H

/* ════════════════════════════════════════════════════════════
 *  gc_internal.h — GC 子系统内部接口
 *
 *  仅供 gc_arc.c / gc_hybrid.c 使用，不对外暴露。
 *  用户代码应使用 gc_engine.h 的公共 API。
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* 从引擎对象链表中摘除节点（维护双向链表 + 计数） */
void re0_gc_engine_unlink_obj(Re0GcEngine *eng, Re0GcObject *obj);

/* 摘除 + 统计更新 + 析构 + 释放节点
 * 这是 ARC/HYBRID 模式的标准释放路径 */
void re0_gc_engine_destroy_obj(Re0GcEngine *eng, Re0GcObject *obj);

#endif
