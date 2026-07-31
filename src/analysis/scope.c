#include "safe.h"
#include "scope.h"
#include <stdlib.h>
#include <string.h>

Re0Scope *re0_scope_new(Re0Scope *parent) {
    Re0Scope *s = (Re0Scope*)xcalloc(1, sizeof(Re0Scope));
    if (s) {
        Re0SymbolVec_init(&s->symbols);
        s->parent = parent;
        s->depth = parent ? parent->depth + 1 : 0;
    }
    return s;
}

void re0_scope_define(Re0Scope *s, const char *name, Re0Type *type, bool is_mut) {
    if (!s || !name) return;
    Re0Symbol sym;
    sym.name = strdup(name);
    if (!sym.name) return;
    sym.type = type;
    sym.is_mutable = is_mut;
    sym.is_function = false;
    Re0SymbolVec_push(&s->symbols, sym);
}

Re0Symbol *re0_scope_lookup(Re0Scope *s, const char *name) {
    while (s) {
        for (size_t i = 0; i < Re0SymbolVec_len(&s->symbols); i++) {
            if (strcmp(s->symbols.data[i].name, name) == 0)
                return &s->symbols.data[i];
        }
        s = s->parent;
    }
    return NULL;
}

Re0Symbol *re0_scope_lookup_local(Re0Scope *s, const char *name) {
    if (!s) return NULL;
    for (size_t i = 0; i < Re0SymbolVec_len(&s->symbols); i++) {
        if (strcmp(s->symbols.data[i].name, name) == 0)
            return &s->symbols.data[i];
    }
    return NULL;
}

void re0_scope_free(Re0Scope *s) {
    if (!s) return;
    for (size_t i = 0; i < Re0SymbolVec_len(&s->symbols); i++)
        free(s->symbols.data[i].name);
    Re0SymbolVec_free(&s->symbols);
    free(s);
}
