#include "lint.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_LINT_VARS 256
#define MAX_FUNC_LINES 50
#define MAX_NESTING 5

typedef struct { char name[128]; bool used; int line; } LintVar;
static LintVar lint_vars[MAX_LINT_VARS];
static int lint_var_count = 0;

static void lint_var_add(const char *name, int line) {
    if (lint_var_count >= MAX_LINT_VARS) return;
    strncpy(lint_vars[lint_var_count].name, name, 127);
    lint_vars[lint_var_count].name[127] = '\0';
    lint_vars[lint_var_count].used = false;
    lint_vars[lint_var_count].line = line;
    lint_var_count++;
}

static void lint_var_mark_used(const char *name) {
    for (int i = 0; i < lint_var_count; i++)
        if (strcmp(lint_vars[i].name, name) == 0) { lint_vars[i].used = true; return; }
}

static void lint_var_finish(Re0ErrorList *errors) {
    for (int i = 0; i < lint_var_count; i++)
        if (!lint_vars[i].used)
            re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                            "unused variable: %s", lint_vars[i].name);
    lint_var_count = 0;
}

/* naming convention: snake_case for fn/var, PascalCase for types */
static bool is_snake_case(const char *s) {
    if (!s || !*s) return true;
    for (const char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z') return false;
    return true;
}

static bool is_pascal_case(const char *s) {
    if (!s || !*s) return true;
    return s[0] >= 'A' && s[0] <= 'Z';
}

static void lint_check_name(Re0ErrorList *errors, const char *name,
                            bool expect_pascal, const char *kind) {
    bool ok = expect_pascal ? is_pascal_case(name) : is_snake_case(name);
    if (!ok)
        re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                        "%s '%s' should use %s naming", kind, name,
                        expect_pascal ? "PascalCase" : "snake_case");
}

/* mark identifiers as used in expressions */
static void lint_expr(Re0Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_IDENT: lint_var_mark_used(e->ident.name); break;
        case EXPR_BINARY: lint_expr(e->binary.left); lint_expr(e->binary.right); break;
        case EXPR_UNARY: lint_expr(e->unary.operand); break;
        case EXPR_CALL:
            if (e->call.callee->kind == EXPR_IDENT)
                lint_var_mark_used(e->call.callee->ident.name);
            lint_expr(e->call.callee);
            for (int i = 0; i < e->call.arg_count; i++) lint_expr(e->call.args[i]);
            break;
        case EXPR_SELECT: lint_expr(e->select.object); break;
        case EXPR_INDEX: lint_expr(e->index.target); lint_expr(e->index.index); break;
        case EXPR_IF:
            lint_expr(e->if_expr.cond);
            lint_expr(e->if_expr.then);
            if (e->if_expr.else_) lint_expr(e->if_expr.else_);
            break;
        case EXPR_ARRAY:
            for (int i = 0; i < e->array.count; i++) lint_expr(e->array.elems[i]);
            break;
        default: break;
    }
}

static void lint_stmts(Re0Stmt **stmts, int count, Re0ErrorList *errors, int depth);

static void lint_stmt(Re0Stmt *s, Re0ErrorList *errors, int depth) {
    if (!s) return;
    if (depth > MAX_NESTING)
        re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                        "nesting depth exceeds %d", MAX_NESTING);
    switch (s->kind) {
        case STMT_LET:
            /* shadowing 检测 */
            for (int i = 0; i < lint_var_count; i++)
                if (strcmp(lint_vars[i].name, s->let_stmt.name) == 0) {
                    re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                                    "variable '%s' shadows an earlier declaration",
                                    s->let_stmt.name);
                    break;
                }
            lint_var_add(s->let_stmt.name, (int)s->span.start.line);
            if (s->let_stmt.init) lint_expr(s->let_stmt.init);
            break;
        case STMT_ASSIGN:
            lint_var_mark_used(s->assign.name);
            lint_expr(s->assign.value);
            break;
        case STMT_FIELD_ASSIGN:
            lint_expr(s->field_assign.obj);
            lint_expr(s->field_assign.value);
            break;
        case STMT_EXPR:
            lint_expr(s->expr_stmt.expr);
            break;
        case STMT_RETURN:
            if (s->return_stmt.value) lint_expr(s->return_stmt.value);
            break;
        case STMT_IF: {
            for (int i = 0; i < s->if_stmt.branch_count; i++) {
                lint_expr(s->if_stmt.branches[i].cond);
                lint_stmts(s->if_stmt.branches[i].body, s->if_stmt.branches[i].body_count, errors, depth + 1);
            }
            if (s->if_stmt.else_body)
                lint_stmts(s->if_stmt.else_body, s->if_stmt.else_count, errors, depth + 1);
            break;
        }
        case STMT_WHILE:
            lint_expr(s->while_stmt.cond);
            lint_stmts(s->while_stmt.body, s->while_stmt.body_count, errors, depth + 1);
            break;
        case STMT_FOR:
            lint_var_add(s->for_stmt.var, (int)s->span.start.line);
            lint_var_mark_used(s->for_stmt.var);
            lint_expr(s->for_stmt.iter);
            lint_stmts(s->for_stmt.body, s->for_stmt.body_count, errors, depth + 1);
            break;
        case STMT_FUNCTION: {
            lint_check_name(errors, s->function.name, false, "function");
            if (s->function.body_count > MAX_FUNC_LINES)
                re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                                "function '%s' has %d statements (max %d)",
                                s->function.name, s->function.body_count, MAX_FUNC_LINES);
            lint_var_count = 0; /* fresh scope per function */
            for (int i = 0; i < s->function.param_count; i++) {
                lint_var_add(s->function.params[i].name, 0);
                lint_var_mark_used(s->function.params[i].name); /* params are implicitly used */
            }
            lint_stmts(s->function.body, s->function.body_count, errors, 0);
            lint_var_finish(errors);
            break;
        }
        case STMT_STRUCT:
            lint_check_name(errors, s->struct_decl.name, true, "struct");
            break;
        case STMT_ENUM:
            lint_check_name(errors, s->enum_decl.name, true, "enum");
            break;
        case STMT_TRAIT:
            lint_check_name(errors, s->trait_decl.name, true, "trait");
            break;
        case STMT_CONST:
            lint_check_name(errors, s->const_decl.name, false, "const");
            break;
        default: break;
    }
}

static void lint_stmts(Re0Stmt **stmts, int count, Re0ErrorList *errors, int depth) {
    bool unreachable = false;
    for (int i = 0; i < count; i++) {
        if (unreachable)
            re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                            "unreachable code after return/break/continue");
        lint_stmt(stmts[i], errors, depth);
        if (stmts[i] && (stmts[i]->kind == STMT_RETURN ||
                         stmts[i]->kind == STMT_BREAK ||
                         stmts[i]->kind == STMT_CONTINUE))
            unreachable = true;
    }
}

void re0_lint_run(Re0StmtVec *stmts, Re0ErrorList *errors) {
    lint_var_count = 0;
    for (size_t i = 0; i < Re0StmtVec_len(stmts); i++)
        lint_stmt(stmts->data[i], errors, 0);
    lint_var_finish(errors);
}
