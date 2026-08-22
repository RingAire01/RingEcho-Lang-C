#ifndef RE0_GC_INTERNAL_H
#define RE0_GC_INTERNAL_H

/* ════════════════════════════════════════════════════════════
 *  gc_internal.h — GC subsystem internal interface
 *
 *  Only for gc_arc.c / gc_hybrid.c; not exposed externally.
 *  User code should use the public API in gc_engine.h.
 * ════════════════════════════════════════════════════════════ */

#include "gc_engine.h"

/* unlink a node from the engine object list (maintains list + count) */
void re0_gc_engine_unlink_obj(Re0GcEngine *eng, Re0GcObject *obj);

/* unlink + stats update + destroy + free the node.
 * Standard release path for ARC/HYBRID modes. */
void re0_gc_engine_destroy_obj(Re0GcEngine *eng, Re0GcObject *obj);

#endif
