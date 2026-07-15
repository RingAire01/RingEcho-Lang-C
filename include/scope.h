#ifndef RE0_SCOPE_H
#define RE0_SCOPE_H
#include "types.h"
#include "vec.h"
#include <stdbool.h>

typedef struct { char *name; Re0Type *type; bool is_mutable; bool is_function; } Re0Symbol;
VEC_DECLARE(Re0SymbolVec, Re0Symbol)

typedef struct Re0Scope { Re0SymbolVec symbols; struct Re0Scope *parent; int depth; } Re0Scope;

Re0Scope  *re0_scope_new(Re0Scope *parent);
void       re0_scope_define(Re0Scope *s, const char *name, Re0Type *type, bool is_mut);
Re0Symbol *re0_scope_lookup(Re0Scope *s, const char *name);
Re0Symbol *re0_scope_lookup_local(Re0Scope *s, const char *name);
void       re0_scope_free(Re0Scope *s);
#endif
