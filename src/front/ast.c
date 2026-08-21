#include "base/safe.h"
#include "front/ast.h"
#include "base/arena.h"
#include <stdlib.h>

/* The AST allocators take no arena parameter (hundreds of call sites),
 * so the arena is bound per-parser instead (see re0_ast_bind_arena).
 * AST nodes are plain data owned by the compiler arena and freed in bulk
 * by re0_arena_free — per-node destroy does not exist by design.
 * Frontend construction is single-threaded per compiler instance;
 * re0_ast_bind_arena is not thread-safe across concurrent parsers. */
static Re0Arena *ast_arena = NULL;

void re0_ast_bind_arena(Re0Arena *arena) {
    ast_arena = arena;
}

Re0Expr *re0_expr_make(Re0ExprKind kind, Re0Span span) {
    if (ast_arena) {
        Re0Expr *e = (Re0Expr*)re0_arena_alloc_zero(ast_arena, sizeof(Re0Expr));
        if (!e) return NULL;
        e->kind = kind; e->span = span;
        return e;
    }
    Re0Expr *e = (Re0Expr*)xcalloc(1, sizeof(Re0Expr));
    if (e) { e->kind = kind; e->span = span; }
    return e;
}

Re0Stmt *re0_stmt_make(Re0StmtKind kind, Re0Span span) {
    if (ast_arena) {
        Re0Stmt *s = (Re0Stmt*)re0_arena_alloc_zero(ast_arena, sizeof(Re0Stmt));
        if (!s) return NULL;
        s->kind = kind; s->span = span;
        return s;
    }
    Re0Stmt *s = (Re0Stmt*)xcalloc(1, sizeof(Re0Stmt));
    if (s) { s->kind = kind; s->span = span; }
    return s;
}
