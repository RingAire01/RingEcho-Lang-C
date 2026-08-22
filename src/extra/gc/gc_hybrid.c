#include "gc/gc_hybrid.h"
#include "gc/gc_arc.h"

/* ════════════════════════════════════════════════════
 *  HYBRID collect
 *
 *  collect logic mirrors ARC_CYCLE:
 *    tracing mark from roots -> sweep unreachable objects
 *
 *  The only difference is the release path (see engine_release):
 *    HYBRID releases OWNED objects immediately,
 *    ARC_CYCLE releases every object immediately.
 * ════════════════════════════════════════════════════ */

void re0_gc_hybrid_collect(Re0GcEngine *eng)
{
    /* reuse the ARC collect implementation (tracing backup) */
    re0_gc_arc_collect(eng);
}
