#include "re0_event.h"
#include <stdlib.h>
#include <string.h>

void re0_event_bus_init(Re0EventBus *bus) {
    if (!bus) return;
    Re0HandlerVec_init(&bus->handlers);
    pthread_mutex_init(&bus->mtx, NULL);
    bus->cancelled = false;
}

void re0_event_bus_subscribe(Re0EventBus *bus, Re0EventHandler fn, void *ctx) {
    if (!bus || !fn) return;
    pthread_mutex_lock(&bus->mtx);
    Re0EventHandlerEntry h = { fn, ctx };
    Re0HandlerVec_push(&bus->handlers, h);
    pthread_mutex_unlock(&bus->mtx);
}

void re0_event_bus_emit(Re0EventBus *bus, Re0Event *ev) {
    if (!bus || !ev) return;
    
    /* 复制 handler 列表，避免在持有锁时调用回调（防止 deadlock） */
    pthread_mutex_lock(&bus->mtx);
    Re0HandlerVec handlers;
    Re0HandlerVec_init(&handlers);
    for (size_t i = 0; i < Re0HandlerVec_len(&bus->handlers); i++) {
        Re0HandlerVec_push(&handlers, bus->handlers.data[i]);
    }
    pthread_mutex_unlock(&bus->mtx);
    
    /* 在锁外调用回调 */
    for (size_t i = 0; i < Re0HandlerVec_len(&handlers); i++) {
        if (handlers.data[i].fn)
            handlers.data[i].fn(ev, handlers.data[i].ctx);
    }
    
    Re0HandlerVec_free(&handlers);
}

bool re0_event_bus_should_cancel(Re0EventBus *bus) {
    if (!bus) return false;
    return bus->cancelled;
}

void re0_event_bus_cancel(Re0EventBus *bus) {
    if (!bus) return;
    pthread_mutex_lock(&bus->mtx);
    bus->cancelled = true;
    pthread_mutex_unlock(&bus->mtx);
}

void re0_event_bus_reset(Re0EventBus *bus) {
    if (!bus) return;
    pthread_mutex_lock(&bus->mtx);
    bus->cancelled = false;
    pthread_mutex_unlock(&bus->mtx);
}

void re0_event_bus_free(Re0EventBus *bus) {
    if (!bus) return;
    Re0HandlerVec_free(&bus->handlers);
    pthread_mutex_destroy(&bus->mtx);
}
