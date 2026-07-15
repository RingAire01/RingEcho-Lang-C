#include "re0_event.h"
#include <stdlib.h>
#include <string.h>

void re0_event_bus_init(Re0EventBus *bus) {
    Re0HandlerVec_init(&bus->handlers);
}

void re0_event_bus_subscribe(Re0EventBus *bus, Re0EventHandler fn, void *ctx) {
    Re0EventHandlerEntry h = { fn, ctx };
    Re0HandlerVec_push(&bus->handlers, h);
}

void re0_event_bus_emit(Re0EventBus *bus, Re0Event *ev) {
    if (!bus || !ev) return;
    for (size_t i = 0; i < Re0HandlerVec_len(&bus->handlers); i++) {
        if (bus->handlers.data[i].fn)
            bus->handlers.data[i].fn(ev, bus->handlers.data[i].ctx);
    }
}

bool re0_event_bus_should_cancel(Re0EventBus *bus) {
    (void)bus;
    return false;
}

void re0_event_bus_free(Re0EventBus *bus) {
    Re0HandlerVec_free(&bus->handlers);
}
