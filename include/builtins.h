#ifndef RE0_BUILTINS_H
#define RE0_BUILTINS_H
#include "types.h"
#include "vec.h"

typedef struct { char *name; Re0Type *type; } Re0BuiltinParam;
typedef struct {
    char *name;
    Re0Type *ret_type;
    Re0BuiltinParam *params;
    int param_count;
} Re0BuiltinFn;
VEC_DECLARE(Re0BuiltinVec, Re0BuiltinFn)

typedef struct { Re0BuiltinVec fns; } Re0BuiltinRegistry;

void          re0_builtin_init(Re0BuiltinRegistry *r);
Re0BuiltinFn *re0_builtin_lookup(Re0BuiltinRegistry *r, const char *name);
void          re0_builtin_free(Re0BuiltinRegistry *r);
#endif
