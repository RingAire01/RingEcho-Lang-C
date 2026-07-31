#ifndef RE0_MANAGER_H
#define RE0_MANAGER_H
#include "re0_gc.h"
#include "re0_event.h"
#include "error.h"
#include <stdbool.h>

#define RE0_MAX_SUBMANAGERS 16

typedef struct Re0Manager Re0Manager;

struct Re0Manager {
    const char *name;
    Re0EventBus *bus;
    Re0GcPool *gc;
    Re0ErrorList *errors;
    Re0Manager *parent;
    Re0Manager *children[RE0_MAX_SUBMANAGERS];
    int child_count;
    bool active;
    bool (*init)(Re0Manager *m);
    bool (*prepare)(Re0Manager *m);
    bool (*run)(Re0Manager *m);
    bool (*finish)(Re0Manager *m);
};

#define re0_manager_cast(mgr, T) ((T *)(mgr))

bool re0_manager_init(Re0Manager *m);
bool re0_manager_execute(Re0Manager *m);
void re0_manager_register_child(Re0Manager *parent, Re0Manager *child);
bool re0_manager_should_cancel(Re0Manager *m);
void re0_manager_emit_event(Re0Manager *m, Re0EventKind kind, void *payload, const char *msg);
void re0_manager_emit_progress(Re0Manager *m, int pct);

#endif
