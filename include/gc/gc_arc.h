#ifndef RE0_GC_ARC_H
#define RE0_GC_ARC_H

/* ════════════════════════════════════════════════════════════
 *  gc_arc.h — ARC（引用计数）+ 循环检测算法
 *
 *  核心机制：
 *
 *  1. 即时释放（retain/release 驱动）
 *     - ref_count 降到 0 时，立即释放该对象
 *     - 释放前遍历子对象（trace 回调），对每个子对象 release
 *     - 子对象 ref_count 也降到 0 时级联释放
 *     - 使用显式工作栈（迭代式），防止深链导致栈溢出
 *
 *  2. 循环检测（tracing backup）
 *     - 定期运行 tracing mark（从 roots 出发）
 *     - 链表中剩余的 ref_count>0 但不可达（WHITE）的对象即为循环垃圾
 *     - 回收这些对象
 *
 *  正确性保证：
 *     - 非循环对象：ref_count 准确反映可达性 → 即时释放正确
 *     - 循环对象：ref_count 永远 > 0 → 由周期性 collect 回收
 *     - 此方案被 Python、PHP 等生产运行时验证
 *
 *  优势：非循环对象零暂停即时释放
 *  劣势：循环对象需等待周期检测
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* ARC 即时释放链：ref_count==0 时，迭代释放该对象及其级联子对象。
 * 调用前需确认 obj->ref_count <= 0。
 * 使用显式工作栈，无递归深度限制。
 */
void re0_gc_arc_release_chain(Re0GcEngine *eng, Re0GcObject *obj);

/* 循环检测回收（collect 入口）：
 * 1. reset colors → mark from roots
 * 2. 回收不可达（WHITE）的对象（含循环垃圾）
 * 同时处理可能遗留的 ref_count<=0 悬挂对象。
 */
void re0_gc_arc_collect(Re0GcEngine *eng);

#endif
