#include "safe.h"
#include "ast.h"
#include "arena.h"
#include <stdlib.h>

Re0Expr *re0_expr_make(Re0ExprKind kind, Re0Span span) {
    Re0Expr *e = (Re0Expr*)xcalloc(1, sizeof(Re0Expr));
    if (e) { e->kind = kind; e->span = span; }
    return e;
}

Re0Stmt *re0_stmt_make(Re0StmtKind kind, Re0Span span) {
    Re0Stmt *s = (Re0Stmt*)xcalloc(1, sizeof(Re0Stmt));
    if (s) { s->kind = kind; s->span = span; }
    return s;
}
