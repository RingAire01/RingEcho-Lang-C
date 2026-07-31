#include "backend.h"
#include "re0_limits.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define R_ZERO    0
#define R_VAR_FIRST 1
#define R_VAR_LAST 18
#define R_TMP_FIRST 19
#define R_TMP_LAST 25
#define R_FP      28
#define R_SP      30
#define R_RA      31
#define MAX_VARS  RE0_MAX_REO_VARS
#define MAX_STRUCTS RE0_MAX_REO_STRUCTS

/* ── struct field offset tracking ── */
static struct { char *name; int offsets[16]; int count; } reo_structs[MAX_STRUCTS];
static int reo_struct_count = 0;

static void reo_register_struct(const char *name, int count) {
    if (reo_struct_count >= MAX_STRUCTS) return;
    reo_structs[reo_struct_count].name = strdup(name);
    reo_structs[reo_struct_count].count = count;
    for (int i = 0; i < count; i++) reo_structs[reo_struct_count].offsets[i] = i * 8;
    reo_struct_count++;
}

typedef struct {
    char *name;
    int reg;
} VarMap;

static VarMap var_map[MAX_VARS];
static int var_count = 0;

static int lookup_var(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_map[i].name, name) == 0) return var_map[i].reg;
    return -1;
}

static int alloc_var(Re0Codegen *c, const char *name) {
    int r = lookup_var(name);
    if (r >= 0) return r;
    if (var_count >= MAX_VARS) {
        if (c && c->errors)
            re0_error_append(c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                             "reo backend: variable register table full (%d)", MAX_VARS);
        return R_TMP_FIRST;
    }
    int reg = (var_count % (R_VAR_LAST - R_VAR_FIRST + 1)) + R_VAR_FIRST;
    char *dup = strdup(name);
    if (!dup) {
        if (c && c->errors)
            re0_error_append(c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                             "reo backend: out of memory in alloc_var");
        return R_TMP_FIRST;
    }
    var_map[var_count].name = dup;
    var_map[var_count].reg = reg;
    var_count++;
    return reg;
}

static void clear_vars(void) {
    for (int i = 0; i < var_count; i++) free(var_map[i].name);
    var_count = 0;
}

static int eval_expr(Re0Codegen *c, Re0Expr *e);
static void emit_stmt(Re0Codegen *c, Re0Stmt *s, int depth);

static Re0Buffer *B(Re0Codegen *c) { return &c->output; }

static void emit(Re0Codegen *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    re0_buffer_write_vfmt(B(c), fmt, ap);
    va_end(ap);
    re0_buffer_write_char(B(c), '\n');
}

static int eval_expr(Re0Codegen *c, Re0Expr *e) {
    if (!e) return R_ZERO;
    switch (e->kind) {
        case EXPR_INT: {
            int r = re0_codegen_new_reg(c);
            emit(c, "    LI R%d, %lld", r, (long long)e->int_lit.val);
            return r;
        }
        case EXPR_FLOAT:
        case EXPR_BOOL: {
            int r = re0_codegen_new_reg(c);
            emit(c, "    LI R%d, %d", r, e->bool_lit.val ? 1 : 0);
            return r;
        }
        case EXPR_CHAR: {
            int r = re0_codegen_new_reg(c);
            emit(c, "    LI R%d, %d", r, (int)(unsigned char)e->char_lit.val);
            return r;
        }
        case EXPR_IDENT: {
            int r = lookup_var(e->ident.name);
            if (r < 0) { r = re0_codegen_new_reg(c); emit(c, "    LI R%d, 0", r); }
            return r;
        }
        case EXPR_BINARY: {
            int lr = eval_expr(c, e->binary.left);
            int rr = eval_expr(c, e->binary.right);
            int res = re0_codegen_new_reg(c);
            const char *op = NULL;
            switch (e->binary.op) {
                case BINOP_ADD: op = "ADD"; break;
                case BINOP_SUB: op = "SUB"; break;
                case BINOP_MUL: op = "MUL"; break;
                case BINOP_DIV: op = "DIV"; break;
                case BINOP_MOD: op = "REM"; break;
                case BINOP_EQ: case BINOP_NE: case BINOP_LT:
                case BINOP_LE: case BINOP_GT: case BINOP_GE:
                case BINOP_AND: case BINOP_OR: {
                    emit(c, "    SUB R%d, R%d, R%d", c->label_counter + 30, lr, rr);
                    int l = re0_codegen_new_label(c);
                    emit(c, "    LI R%d, 0", res);
                    switch (e->binary.op) {
                        case BINOP_EQ: emit(c, "    BNE R%d, R%d, L%d", c->label_counter + 30, R_ZERO, l); break;
                        case BINOP_NE: emit(c, "    BEQ R%d, R%d, L%d", c->label_counter + 30, R_ZERO, l); break;
                        case BINOP_LT: emit(c, "    BGE R%d, R%d, L%d", lr, rr, l); break;
                        case BINOP_LE: emit(c, "    BLT R%d, R%d, L%d", rr, lr, l); break;
                        case BINOP_GT: emit(c, "    BLT R%d, R%d, L%d", lr, rr, l); break;
                        case BINOP_GE: emit(c, "    BGE R%d, R%d, L%d", lr, rr, l); break;
                        case BINOP_AND: emit(c, "    BEQ R%d, R%d, L%d", lr, R_ZERO, l); emit(c, "    BEQ R%d, R%d, L%d", rr, R_ZERO, l); break;
                        case BINOP_OR: emit(c, "    BNE R%d, R%d, L%d", lr, R_ZERO, l); break;
                        default: break;
                    }
                    emit(c, "    LI R%d, 1", res);
                    emit(c, "L%d:", l);
                    return res;
                }
                default: op = "ADD"; break;
            }
            if (op) emit(c, "    %s R%d, R%d, R%d", op, res, lr, rr);
            return res;
        }
        case EXPR_UNARY: {
            int or_ = eval_expr(c, e->unary.operand);
            if (e->unary.op == UNOP_NEG) {
                int res = re0_codegen_new_reg(c);
                emit(c, "    SUB R%d, R%d, R%d", res, R_ZERO, or_);
                return res;
            }
            return or_;
        }
        case EXPR_CALL: {
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                if (strcmp(fn, "println") == 0 || strcmp(fn, "print") == 0 ||
                    strcmp(fn, "panic") == 0 || strcmp(fn, "assert") == 0) {
                    emit(c, "    SYSCALL 0");
                    return R_ZERO;
                }
            }
            for (int i = 0; i < e->call.arg_count; i++) {
                int ar = eval_expr(c, e->call.args[i]);
                emit(c, "    PUSH R%d", ar);
            }
            emit(c, "    CALL %s", e->call.callee->ident.name);
            for (int i = e->call.arg_count - 1; i >= 0; i--)
                emit(c, "    ADDI R%d, R%d, 0", R_SP, 8);
            int res = re0_codegen_new_reg(c);
            emit(c, "    POP R%d", res);
            return res;
        }
        case EXPR_IF: {
            int cr = eval_expr(c, e->if_expr.cond);
            int l_else = re0_codegen_new_label(c);
            int l_end = re0_codegen_new_label(c);
            emit(c, "    BEQ R%d, R%d, L%d", cr, R_ZERO, l_else);
            int tr = eval_expr(c, e->if_expr.then);
            int res = re0_codegen_new_reg(c);
            emit(c, "    ADDI R%d, R%d, 0", res, tr);
            emit(c, "    JMP L%d", l_end);
            emit(c, "L%d:", l_else);
            if (e->if_expr.else_) {
                int er = eval_expr(c, e->if_expr.else_);
                emit(c, "    ADDI R%d, R%d, 0", res, er);
            }
            emit(c, "L%d:", l_end);
            return res;
        }
        case EXPR_SELECT: {
            int or_ = eval_expr(c, e->select.object);
            int res = re0_codegen_new_reg(c);
            emit(c, "    LOAD.W8 R%d, [R%d]", res, or_);
            return res;
        }
        case EXPR_STRUCT_INIT: {
            int ptr = re0_codegen_new_reg(c);
            for (int i = 0; i < e->struct_init.field_count; i++) {
                int vr = e->struct_init.fields[i].value ? eval_expr(c, e->struct_init.fields[i].value) : R_ZERO;
                emit(c, "    STORE.W8 R%d, [R%d + %d]", vr, ptr, i * 8);
            }
            return ptr;
        }
        default: {
            int r = re0_codegen_new_reg(c);
            emit(c, "    LI R%d, 0", r);
            return r;
        }
    }
}

static void emit_body(Re0Codegen *c, Re0Stmt **body, int count);

static void emit_stmt(Re0Codegen *c, Re0Stmt *s, int depth) {
    (void)depth;
    if (!s) return;
    switch (s->kind) {
        case STMT_LET: {
            int reg = alloc_var(c, s->let_stmt.name);
            if (s->let_stmt.init) {
                int vr = eval_expr(c, s->let_stmt.init);
                emit(c, "    ADDI R%d, R%d, 0", reg, vr);
            } else {
                emit(c, "    LI R%d, 0", reg);
            }
            break;
        }
        case STMT_ASSIGN: {
            int reg = lookup_var(s->assign.name);
            if (reg < 0) reg = alloc_var(c, s->assign.name);
            int vr = eval_expr(c, s->assign.value);
            emit(c, "    ADDI R%d, R%d, 0", reg, vr);
            break;
        }
        case STMT_EXPR:
            eval_expr(c, s->expr_stmt.expr);
            break;
        case STMT_RETURN: {
            if (s->return_stmt.value) {
                int vr = eval_expr(c, s->return_stmt.value);
                emit(c, "    PUSH R%d", vr);
            }
            emit(c, "    RET");
            break;
        }
        case STMT_IF: {
            int cr = eval_expr(c, s->if_stmt.branches[0].cond);
            int l_skip = re0_codegen_new_label(c);
            emit(c, "    BEQ R%d, R%d, L%d_else", cr, R_ZERO, l_skip);
            emit_body(c, s->if_stmt.branches[0].body, s->if_stmt.branches[0].body_count);
            if (s->if_stmt.else_body && s->if_stmt.else_count > 0) {
                int l_end = re0_codegen_new_label(c);
                emit(c, "    JMP L%d_end", l_end);
                emit(c, "L%d_else:", l_skip);
                emit_body(c, s->if_stmt.else_body, s->if_stmt.else_count);
                emit(c, "L%d_end:", l_end);
            } else {
                emit(c, "L%d_else:", l_skip);
            }
            break;
        }
        case STMT_WHILE: {
            int l_start = re0_codegen_new_label(c);
            int l_end = re0_codegen_new_label(c);
            emit(c, "L%d_loop:", l_start);
            int cr = eval_expr(c, s->while_stmt.cond);
            emit(c, "    BEQ R%d, R%d, L%d_end", cr, R_ZERO, l_end);
            emit_body(c, s->while_stmt.body, s->while_stmt.body_count);
            emit(c, "    JMP L%d_loop", l_start);
            emit(c, "L%d_end:", l_end);
            break;
        }
        case STMT_FOR: {
            int vr = alloc_var(c, s->for_stmt.var);
            emit(c, "    LI R%d, 0", vr);
            int l_start = re0_codegen_new_label(c);
            int l_end = re0_codegen_new_label(c);
            int limit = R_TMP_FIRST;
            if (s->for_stmt.iter->kind == EXPR_BINARY && s->for_stmt.iter->binary.op == BINOP_RANGE) {
                limit = eval_expr(c, s->for_stmt.iter->binary.right);
            } else {
                limit = eval_expr(c, s->for_stmt.iter);
            }
            emit(c, "L%d_for:", l_start);
            emit(c, "    BGE R%d, R%d, L%d_end", vr, limit, l_end);
            emit_body(c, s->for_stmt.body, s->for_stmt.body_count);
            emit(c, "    ADDI R%d, R%d, 1", vr, vr);
            emit(c, "    JMP L%d_for", l_start);
            emit(c, "L%d_end:", l_end);
            break;
        }
        case STMT_FUNCTION: {
            emit(c, "%s:", s->function.name);
            for (int i = 0; i < s->function.param_count; i++)
                alloc_var(c, s->function.params[i].name);
            emit_body(c, s->function.body, s->function.body_count);
            emit(c, "    LI R%d, 0", R_TMP_FIRST);
            emit(c, "    PUSH R%d", R_TMP_FIRST);
            emit(c, "    RET");
            emit(c, "");
            break;
        }
        case STMT_BREAK:
            emit(c, "    JMP L%d_end", c->temp_counter);
            break;
        case STMT_CONTINUE:
            emit(c, "    JMP L%d_loop", c->temp_counter);
            break;
        case STMT_STRUCT:
            reo_register_struct(s->struct_decl.name, s->struct_decl.field_count);
            break;
        default:
            break;
    }
}

static void emit_body(Re0Codegen *c, Re0Stmt **body, int count) {
    for (int i = 0; i < count; i++)
        emit_stmt(c, body[i], 0);
}

static void reo_begin(Re0Codegen *c) {
    clear_vars();
    emit(c, "; RingEcho ISA generated code");
    emit(c, "");
    emit(c, "    JMP main_");
    emit(c, "");
}

static void reo_end(Re0Codegen *c) {
    emit(c, "main_:");
    emit(c, "    CALL main");
    emit(c, "    HALT");
    clear_vars();
}

Re0Backend re0_backend_reo = { "reo", reo_begin, reo_end, eval_expr, emit_stmt };
