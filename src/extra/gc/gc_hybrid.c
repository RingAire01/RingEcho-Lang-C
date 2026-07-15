#include "gc/gc_hybrid.h"
#include "gc/gc_arc.h"

/* ════════════════════════════════════════════════════
 *  HYBRID collect
 *
 *  collect 逻辑与 ARC_CYCLE 一致：
 *    tracing mark from roots → sweep 不可达对象
 *
 *  区别仅在 release 路径（engine_release 中区分）：
 *    HYBRID 仅对 OWNED 对象即时释放
 *    ARC_CYCLE 对所有对象即时释放
 * ════════════════════════════════════════════════════ */

void re0_gc_hybrid_collect(Re0GcEngine *eng)
{
    /* 复用 ARC 的 collect 实现（tracing backup） */
    re0_gc_arc_collect(eng);
}
