#ifndef RE0_GC_ARC_H
#define RE0_GC_ARC_H

/* ════════════════════════════════════════════════════════════
 *  gc_arc.h — ARC (reference counting) + cycle detection
 *
 *  Core mechanisms:
 *
 *  1. Immediate release (retain/release driven)
 *     - when ref_count drops to 0 the object is freed at once
 *     - before freeing, walk children (trace callback) and release each
 *     - children that also reach ref_count 0 cascade-release
 *     - an explicit work stack (iterative) prevents stack overflow on deep chains
 *
 *  2. Cycle detection (tracing backup)
 *     - periodically run a tracing mark (from roots)
 *     - list objects with ref_count>0 that are unreachable (WHITE) are cycle garbage
 *     - reclaim those objects
 *
 *  Correctness guarantees:
 *     - acyclic objects: ref_count accurately reflects reachability → immediate release is correct
 *     - cyclic objects: ref_count never reaches 0 → reclaimed by periodic collect
 *     - this design is validated by production runtimes such as Python and PHP
 *
 *  Strength: zero-pause immediate release for acyclic objects
 *  Weakness: cyclic objects must wait for the periodic cycle scan
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* ARC immediate release chain: with ref_count==0, iteratively release the
 * object and its cascading children. Caller must confirm obj->ref_count <= 0.
 * Uses an explicit work stack — no recursion depth limit.
 */
void re0_gc_arc_release_chain(Re0GcEngine *eng, Re0GcObject *obj);

/* Cycle-detection collect (collect entry point):
 * 1. reset colors -> mark from roots
 * 2. reclaim unreachable (WHITE) objects (including cycle garbage)
 * Also clears any leftover ref_count<=0 dangling objects.
 */
void re0_gc_arc_collect(Re0GcEngine *eng);

#endif
