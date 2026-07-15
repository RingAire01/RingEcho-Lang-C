#ifndef RE0_EVENT_H
#define RE0_EVENT_H
#include "vec.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RE0_EV_LEXER_START,
    RE0_EV_LEXER_DONE,
    RE0_EV_PARSER_START,
    RE0_EV_PARSER_DONE,
    RE0_EV_SEMA_START,
    RE0_EV_SEMA_DONE,
    RE0_EV_CODEGEN_START,
    RE0_EV_CODEGEN_DONE,
    RE0_EV_BUILD_START,
    RE0_EV_BUILD_DONE,
    RE0_EV_ERROR,
    RE0_EV_WARNING,
    RE0_EV_PROGRESS,
    RE0_EV_SHUTDOWN,
} Re0EventKind;

typedef struct {
    Re0EventKind kind;
    void *payload;
    char *message;
    int progress_pct;
    bool cancel;
} Re0Event;

typedef void (*Re0EventHandler)(Re0Event *ev, void *ctx);
typedef struct { Re0EventHandler fn; void *ctx; } Re0EventHandlerEntry;
VEC_DECLARE(Re0HandlerVec, Re0EventHandlerEntry)

typedef struct {
    Re0HandlerVec handlers;
} Re0EventBus;

void re0_event_bus_init(Re0EventBus *bus);
void re0_event_bus_subscribe(Re0EventBus *bus, Re0EventHandler fn, void *ctx);
void re0_event_bus_emit(Re0EventBus *bus, Re0Event *ev);
bool re0_event_bus_should_cancel(Re0EventBus *bus);
void re0_event_bus_free(Re0EventBus *bus);

#endif
