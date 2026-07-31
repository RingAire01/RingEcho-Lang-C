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
    int                infer_depth;
    Re0Type           *current_fn_return;   /* 当前函数返回类型（B3 校验用） */
    int                loop_depth;          /* 当前循环嵌套深度（break/continue 校验用） */
    int                fn_depth;            /* 当前函数嵌套深度（return 校验用） */
} Re0Sema;

void re0_sema_init(Re0Sema *s, Re0Arena *arena, Re0ErrorList *errors,
                   Re0SemanticModel *model, Re0BuiltinRegistry *builtins);
bool re0_sema_check(Re0Sema *s, Re0StmtVec *stmts);
void re0_sema_destroy(Re0Sema *s);

#endif
