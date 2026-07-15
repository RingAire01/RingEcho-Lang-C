#include "gc/gc_roots.h"
#include <stdlib.h>

void re0_gc_roots_init(Re0GcRootSet *rs)
{
    if (!rs) return;
    rs->items = NULL;
    rs->count = 0;
    rs->cap   = 0;
}

bool re0_gc_roots_add(Re0GcRootSet *rs, Re0GcObject *obj)
{
    if (!rs || !obj) return false;

    for (int i = 0; i < rs->count; i++) {
        if (rs->items[i] == obj) return true;
    }

    if (rs->count >= rs->cap) {
        int nc = rs->cap ? rs->cap * 2 : RE0_GC_ROOT_CAP_INIT;
        Re0GcObject **ni = (Re0GcObject **)realloc(rs->items,
                                    sizeof(Re0GcObject *) * nc);
        if (!ni) return false;
        rs->items = ni;
        rs->cap   = nc;
    }

    rs->items[rs->count++] = obj;
    return true;
}

bool re0_gc_roots_remove(Re0GcRootSet *rs, Re0GcObject *obj)
{
    if (!rs || !obj) return false;
    for (int i = 0; i < rs->count; i++) {
        if (rs->items[i] == obj) {
            rs->items[i] = rs->items[rs->count - 1];
            rs->count--;
            return true;
        }
    }
    return false;
}

void re0_gc_roots_clear(Re0GcRootSet *rs)
{
    if (!rs) return;
    rs->count = 0;
}

void re0_gc_roots_foreach(Re0GcRootSet *rs, Re0GcRootVisitor fn, void *ctx)
{
    if (!rs || !fn) return;
    for (int i = 0; i < rs->count; i++) {
        if (rs->items[i]) fn(rs->items[i], ctx);
    }
}

void re0_gc_roots_free(Re0GcRootSet *rs)
{
    if (!rs) return;
    free(rs->items);
    rs->items = NULL;
    rs->count = 0;
    rs->cap   = 0;
}
