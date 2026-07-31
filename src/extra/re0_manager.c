#include "re0_manager.h"

bool re0_manager_init(Re0Manager *m) {
    if (!m || !m->init) return false;
    bool ok = m->init(m);
    if (ok) m->active = true;
    return ok;
}

void re0_manager_register_child(Re0Manager *parent, Re0Manager *child) {
    if (!parent || !child || parent->child_count >= RE0_MAX_SUBMANAGERS) return;
    child->parent = parent;
    child->bus = parent->bus;
    child->gc = parent->gc;
    child->errors = parent->errors;
    parent->children[parent->child_count++] = child;
}

bool re0_manager_should_cancel(Re0Manager *m) {
    return (m && m->bus) ? re0_event_bus_should_cancel(m->bus) : false;
}

bool re0_manager_execute(Re0Manager *m) {
    if (!m) return false;
    if (re0_manager_should_cancel(m)) {
        if (m->finish) m->finish(m);
        return false;
    }
    bool ok = true;
    if (m->prepare) ok = m->prepare(m);
    if (ok && m->run) ok = m->run(m);
    for (int i = 0; i < m->child_count; i++) {
        if (re0_manager_should_cancel(m)) break;
        if (!re0_manager_execute(m->children[i])) { ok = false; break; }
    }
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
