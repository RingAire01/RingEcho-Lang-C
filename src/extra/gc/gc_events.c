#include "gc/gc_events.h"
#include <stdlib.h>
#include <string.h>

void re0_gc_listeners_init(Re0GcListeners *ls)
{
    if (!ls) return;
    ls->fns   = NULL;
    ls->ctxs  = NULL;
    ls->count = 0;
    ls->cap   = 0;
}

bool re0_gc_listeners_add(Re0GcListeners *ls, Re0GcCallback fn, void *ctx)
{
    if (!ls || !fn) return false;

    if (ls->count >= ls->cap) {
        int nc = ls->cap ? ls->cap * 2 : 8;
        Re0GcCallback *nf = (Re0GcCallback *)realloc(ls->fns, sizeof(Re0GcCallback) * nc);
        if (!nf) return false;
        ls->fns = nf;

        void **nc2 = (void **)realloc(ls->ctxs, sizeof(void *) * nc);
        if (!nc2) return false;
        ls->ctxs = nc2;

        ls->cap = nc;
    }

    ls->fns[ls->count]  = fn;
    ls->ctxs[ls->count] = ctx;
    ls->count++;
    return true;
}

void re0_gc_listeners_remove(Re0GcListeners *ls, Re0GcCallback fn, void *ctx)
{
    if (!ls || !fn) return;
    for (int i = 0; i < ls->count; i++) {
        if (ls->fns[i] == fn && ls->ctxs[i] == ctx) {
            ls->fns[i]  = ls->fns[ls->count - 1];
            ls->ctxs[i] = ls->ctxs[ls->count - 1];
            ls->count--;
            return;
        }
    }
}

void re0_gc_listeners_emit(Re0GcListeners *ls, const Re0GcEvent *ev)
{
    if (!ls || !ev) return;
    for (int i = 0; i < ls->count; i++) {
        if (ls->fns[i]) ls->fns[i](ev, ls->ctxs[i]);
    }
}

void re0_gc_listeners_free(Re0GcListeners *ls)
{
    if (!ls) return;
    free(ls->fns);
    free(ls->ctxs);
    ls->fns   = NULL;
    ls->ctxs  = NULL;
    ls->count = 0;
    ls->cap   = 0;
}

Re0GcEvent re0_gc_event_make(Re0GcEventKind kind, Re0GcMode mode, Re0GcAlgo algo)
{
    Re0GcEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.mode = mode;
    ev.algo = algo;
    return ev;
}
