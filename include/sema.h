#ifndef RE0_SEMA_H
#define RE0_SEMA_H
#include "arena.h"
#include "error.h"
#include "ast.h"
#include "scope.h"
#include "model.h"
#include "builtins.h"

typedef struct {
    Re0Arena          *arena;
    Re0ErrorList      *errors;
    Re0Scope          *global_scope;
    Re0Scope          *current_scope;
    Re0SemanticModel  *model;
    Re0BuiltinRegistry *builtins;
    Re0StmtVec         checked;
    bool               had_error;
} Re0Sema;

void re0_sema_init(Re0Sema *s, Re0Arena *arena, Re0ErrorList *errors,
                   Re0SemanticModel *model, Re0BuiltinRegistry *builtins);
bool re0_sema_check(Re0Sema *s, Re0StmtVec *stmts);
void re0_sema_destroy(Re0Sema *s);

#endif
