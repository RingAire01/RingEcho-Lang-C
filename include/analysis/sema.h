#ifndef RE0_SEMA_H
#define RE0_SEMA_H
#include "base/arena.h"
#include "base/error.h"
#include "front/ast.h"
#include "analysis/scope.h"
#include "analysis/model.h"
#include "analysis/builtins.h"

typedef struct Re0ScopeVec {
    Re0Scope **data;
    size_t len;
    size_t cap;
} Re0ScopeVec;

typedef struct {
    Re0Arena          *arena;
    Re0ErrorList      *errors;
    Re0Scope          *global_scope;
    Re0Scope          *current_scope;
    Re0SemanticModel  *model;
    Re0BuiltinRegistry *builtins;
    Re0StmtVec         checked;
    /* All child scopes opened during checking. They form no tree we can
     * walk at destroy time (only the global scope is linked), so they are
     * tracked here and freed in re0_sema_destroy. Long-lived processes
     * (LSP) re-run sema per message; without this list every child scope
     * and its symbol strdups leaked. */
    Re0ScopeVec        child_scopes;
    /* Every heap-allocated type object produced via re0_type_make(..., NULL);
     * released together at destroy time (see re0_type_free). Types may be
     * shared, so this list deduplicates by pointer. */
    Re0TypeVec         owned_types;
    bool               supports_conversions; /* Target capability, checked before lowering. */
    bool               had_error;
    int                infer_depth;
    int                statement_depth;
    Re0Type           *current_fn_return;   /* current function return type (for B3 validation) */
    int                loop_depth;          /* current loop nesting depth (for break/continue validation) */
    int                fn_depth;            /* current function nesting depth (for return validation) */
} Re0Sema;

void re0_sema_init(Re0Sema *s, Re0Arena *arena, Re0ErrorList *errors,
                   Re0SemanticModel *model, Re0BuiltinRegistry *builtins);
bool re0_sema_check(Re0Sema *s, Re0StmtVec *stmts);
void re0_sema_destroy(Re0Sema *s);

/* open a child scope owned by s (tracked for later bulk free) */
Re0Scope *re0_sema_open_scope(Re0Sema *s, Re0Scope *parent);

/* Track a heap-owned (owned==true) type object for bulk release at
 * destroy time. Returns the same pointer for chaining; deduplicates by
 * pointer so a shared object is registered only once. */
Re0Type *re0_sema_own_type(Re0Sema *s, Re0Type *t);

#endif
