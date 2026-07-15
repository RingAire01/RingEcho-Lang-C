#include "re0_manager.h"
#include <stdio.h>

bool re0_manager_init(Re0Manager *m) {
    if (!m || !m->init) return false;
    return m->init(m);
}

bool re0_manager_execute(Re0Manager *m) {
    if (!m) return false;
    bool ok = true;
    if (m->prepare) ok = m->prepare(m);
    if (ok && m->run) ok = m->run(m);
    if (m->finish) m->finish(m);
    return ok;
}

void re0_manager_emit_event(Re0Manager *m, Re0EventKind kind, void *payload, const char *msg) {
    if (!m || !m->bus) return;
    Re0Event ev;
    ev.kind = kind;
    ev.payload = payload;
    ev.message = (char *)msg;
    ev.progress_pct = 0;
    ev.cancel = false;
    re0_event_bus_emit(m->bus, &ev);
}

void re0_manager_emit_progress(Re0Manager *m, int pct) {
    if (!m || !m->bus) return;
    Re0Event ev;
    ev.kind = RE0_EV_PROGRESS;
    ev.payload = NULL;
    ev.message = NULL;
    ev.progress_pct = pct;
    ev.cancel = false;
    re0_event_bus_emit(m->bus, &ev);
}
