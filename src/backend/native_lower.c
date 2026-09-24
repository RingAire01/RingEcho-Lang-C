#include "backend/native_internal.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2,
               "native literal encoding requires IEEE binary32/binary64 host types");

typedef struct { const char *name; NType type; size_t slot; } Local;
typedef struct {
    NModule *m;
    NFunction *f;
    Local locals[N_MAX_LOCALS];
    size_t local_count;
    size_t breaks[N_MAX_DEPTH], continues[N_MAX_DEPTH];
    unsigned loops, depth;
} Lower;

static NType type_name(const char *s) {
    if (!s || strcmp(s, "unit") == 0 || strcmp(s, "()") == 0) return N_UNIT;
    if (strcmp(s, "i64") == 0) return N_I64;
    if (strcmp(s, "u64") == 0) return N_U64;
    if (strcmp(s, "bool") == 0) return N_BOOL;
    if (strcmp(s, "i8") == 0) return N_I8;
    if (strcmp(s, "i16") == 0) return N_I16;
    if (strcmp(s, "i32") == 0) return N_I32;
    if (strcmp(s, "isize") == 0) return N_ISIZE;
    if (strcmp(s, "u8") == 0) return N_U8;
    if (strcmp(s, "u16") == 0) return N_U16;
    if (strcmp(s, "u32") == 0) return N_U32;
    if (strcmp(s, "usize") == 0) return N_USIZE;
    if (strcmp(s, "char") == 0) return N_CHAR;
    if (strcmp(s, "f32") == 0) return N_F32;
    if (strcmp(s, "f64") == 0) return N_F64;
    return N_INVALID;
}

static NType expr_type(Re0Expr *e) {
    if (!e || !e->resolved_type) return N_INVALID;
    switch (e->resolved_type->kind) {
        case RE0_TYPE_UNIT: return N_UNIT;
        case RE0_TYPE_I64: return N_I64;
        case RE0_TYPE_U64: return N_U64;
        case RE0_TYPE_BOOL: return N_BOOL;
        case RE0_TYPE_I8: return N_I8;
        case RE0_TYPE_I16: return N_I16;
        case RE0_TYPE_I32: return N_I32;
        case RE0_TYPE_ISIZE: return N_ISIZE;
        case RE0_TYPE_U8: return N_U8;
        case RE0_TYPE_U16: return N_U16;
        case RE0_TYPE_U32: return N_U32;
        case RE0_TYPE_USIZE: return N_USIZE;
        case RE0_TYPE_CHAR: return N_CHAR;
        case RE0_TYPE_F32: return N_F32;
        case RE0_TYPE_F64: return N_F64;
        default: return N_INVALID;
    }
}

static void fail(Lower *l, const char *msg) { n_error(l->m, l->f->ast->span, msg); }

static void emit(Lower *l, NOp op, NType type, uint64_t value, size_t arg) {
    NFunction *f = l->f;
    if (l->m->codegen->had_error) return;
    if (f->count == N_MAX_IR || l->m->total_ir == N_MAX_TOTAL_IR) {
        fail(l, "IR instruction limit exceeded"); return;
    }
    if (f->count == f->capacity) {
        size_t cap = f->capacity ? f->capacity * 2 : 64;
        NInst *p = realloc(f->ir, cap * sizeof(*p));
        if (!p) { fail(l, "cannot allocate IR"); return; }
        f->ir = p; f->capacity = cap;
    }
    f->ir[f->count++] = (NInst){op, type, value, arg, -1};
    l->m->total_ir++;
}

static size_t label(Lower *l) { return l->f->labels++; }
static void mark(Lower *l, size_t id) { emit(l, N_LABEL, N_UNIT, 0, id); }
static Local *lookup(Lower *l, const char *name) {
    for (size_t i = l->local_count; i > 0; i--)
        if (strcmp(l->locals[i - 1].name, name) == 0) return &l->locals[i - 1];
    return NULL;
}
static void local(Lower *l, const char *name, NType type) {
    if (l->f->slots == N_MAX_LOCALS || l->local_count == N_MAX_LOCALS) {
        fail(l, "local variable limit exceeded"); return;
    }
    l->locals[l->local_count++] = (Local){name, type, l->f->slots++};
}
static bool compatible(NType actual, NType expected, Re0Expr *e) {
    if (actual == expected && actual != N_INVALID) return true;
    if (e && e->kind == EXPR_INT && !e->int_lit.suffix && n_type_integer(expected))
        return re0_integer_fits(e->int_lit.integer, n_type_bits(expected), n_type_signed(expected));
    if (e && e->kind == EXPR_FLOAT && !e->float_lit.suffix && expected == N_F32)
        return e->float_lit.val >= -FLT_MAX && e->float_lit.val <= FLT_MAX &&
               (double)(float)e->float_lit.val == e->float_lit.val;
    Re0Type from = {.kind = n_type_kind(actual)}, to = {.kind = n_type_kind(expected)};
    return actual != N_INVALID && expected != N_INVALID && re0_type_coercible(&from, &to);
}
static NType expression(Lower *l, Re0Expr *e);
static void body(Lower *l, Re0Stmt **stmts, int count);

static NType binary(Lower *l, Re0Expr *e) {
    Re0BinOpKind op = e->binary.op;
    NType a = expr_type(e->binary.left), b = expr_type(e->binary.right);
    if (op == BINOP_AND || op == BINOP_OR) {
        if (a != N_BOOL || b != N_BOOL) { fail(l, "logical operands must be bool"); return N_INVALID; }
        size_t alternate = label(l), end = label(l);
        expression(l, e->binary.left);
        emit(l, N_JZ, N_UNIT, 0, alternate);
        if (op == BINOP_AND) expression(l, e->binary.right);
        else emit(l, N_CONST, N_BOOL, 1, 0);
        emit(l, N_JUMP, N_UNIT, 0, end);
        mark(l, alternate);
        if (op == BINOP_OR) expression(l, e->binary.right);
        else emit(l, N_CONST, N_BOOL, 0, 0);
        mark(l, end);
        return N_BOOL;
    }
    bool comparison = op >= BINOP_EQ && op <= BINOP_GE;
    NType operand = comparison ?
        (a == N_F64 || b == N_F64 ? N_F64 : a == N_F32 || b == N_F32 ? N_F32 : a) : expr_type(e);
    if (n_type_float(operand) && (n_type_integer(a) || n_type_float(a)) &&
        (n_type_integer(b) || n_type_float(b))) {
        if (!(comparison || op == BINOP_ADD || op == BINOP_SUB || op == BINOP_MUL || op == BINOP_DIV)) {
            fail(l, "unsupported floating-point operator"); return N_INVALID;
        }
        expression(l, e->binary.left);
        if (a != operand) emit(l, N_CONVERT, operand, 0, a);
        expression(l, e->binary.right);
        if (b != operand) emit(l, N_CONVERT, operand, 0, b);
        emit(l, N_BINARY, operand, 0, op);
        return comparison ? N_BOOL : operand;
    }
    if (!comparison && n_type_integer(operand) && n_type_integer(a) && n_type_integer(b)) {
        expression(l, e->binary.left);
        if (a != operand) emit(l, N_CONVERT, operand, 0, a);
        expression(l, e->binary.right);
        if (b != operand) emit(l, N_CONVERT, operand, 0, b);
        emit(l, N_BINARY, operand, 0, op);
        return operand;
    }
    if (a != b) {
        if (compatible(a, b, e->binary.left)) a = b;
        else if (compatible(b, a, e->binary.right)) b = a;
    }
    if (a != b || (!n_type_integer(a) &&
        !(a == N_BOOL && (op == BINOP_EQ || op == BINOP_NE)) && !(a == N_CHAR && comparison)) ||
        op == BINOP_RANGE || op == BINOP_ASSIGN_SENTINEL) {
        fail(l, "unsupported or incompatible binary operands"); return N_INVALID;
    }
    expression(l, e->binary.left); expression(l, e->binary.right);
    emit(l, N_BINARY, a, 0, (size_t)op);
    return comparison ? N_BOOL : a;
}

static NType expression_impl(Lower *l, Re0Expr *e) {
    NType type = expr_type(e);
    if (type == N_INVALID) { fail(l, "expression type is not supported by native code generation yet"); return type; }
    switch (e->kind) {
        case EXPR_INT:
            if (!re0_integer_fits(e->int_lit.integer, n_type_bits(type), n_type_signed(type))) {
                fail(l, "integer literal exceeds its type"); return N_INVALID;
            }
            emit(l, N_CONST, type, e->int_lit.integer.negative ?
                 UINT64_C(0) - e->int_lit.integer.low : e->int_lit.integer.low, 0);
            return type;
        case EXPR_BOOL: emit(l, N_CONST, N_BOOL, e->bool_lit.val, 0); return N_BOOL;
        case EXPR_CHAR: emit(l, N_CONST, N_CHAR, (unsigned char)e->char_lit.val, 0); return N_CHAR;
        case EXPR_FLOAT: {
            uint64_t bits = 0;
            if (type == N_F32) {
                float value = (float)e->float_lit.val;
                uint32_t small;
                memcpy(&small, &value, sizeof(small)); bits = small;
            } else memcpy(&bits, &e->float_lit.val, sizeof(bits));
            emit(l, N_CONST, type, bits, 0); return type;
        }
        case EXPR_UNIT: emit(l, N_CONST, N_UNIT, 0, 0); return N_UNIT;
        case EXPR_IDENT: {
            Local *v = lookup(l, e->ident.name);
            if (!v) { fail(l, "identifier is not a local variable"); return N_INVALID; }
            emit(l, N_LOAD, v->type, 0, v->slot); return v->type;
        }
        case EXPR_BINARY: return binary(l, e);
        case EXPR_UNARY: {
            NType operand = expression(l, e->unary.operand);
            Re0UnOpKind op = e->unary.op;
            if (!((op == UNOP_NOT && operand == N_BOOL) ||
                  ((op == UNOP_NEG || op == UNOP_BNOT) && n_type_integer(operand)) ||
                  (op == UNOP_NEG && n_type_float(operand)))) {
                fail(l, "unsupported unary operation"); return N_INVALID;
            }
            emit(l, N_UNARY, operand, 0, op); return type;
        }
        case EXPR_CAST: {
            if (e->cast.checked) { fail(l, "checked casts are not supported yet"); return N_INVALID; }
            NType from = expression(l, e->cast.inner);
            if ((!n_type_integer(from) && !n_type_float(from) && from != N_BOOL && from != N_CHAR) ||
                (!n_type_integer(type) && !n_type_float(type) && type != N_BOOL && type != N_CHAR)) {
                fail(l, "unsupported cast"); return N_INVALID;
            }
            emit(l, N_CONVERT, type, 0, from);
            return type;
        }
        case EXPR_CALL: {
            if (!e->call.callee || e->call.callee->kind != EXPR_IDENT) {
                fail(l, "only direct function calls are supported"); return N_INVALID;
            }
            const char *name = e->call.callee->ident.name;
            if (strcmp(name, "assert") == 0) {
                if (e->call.arg_count != 1 || expression(l, e->call.args[0]) != N_BOOL)
                    fail(l, "assert requires one bool argument");
                emit(l, N_ASSERT, N_UNIT, 0, 0);
                emit(l, N_CONST, N_UNIT, 0, 0); return N_UNIT;
            }
            size_t index = 0;
            while (index < l->m->count && strcmp(name, l->m->functions[index].name) != 0) index++;
            if (index == l->m->count) { fail(l, "call is not a supported function or intrinsic"); return N_INVALID; }
            NFunction *f = &l->m->functions[index];
            if (e->call.arg_count != f->param_count) { fail(l, "call argument count mismatch"); return N_INVALID; }
            for (int i = 0; i < f->param_count; i++) {
                NType arg = expression(l, e->call.args[i]);
                if (!compatible(arg, f->params[i], e->call.args[i])) fail(l, "call argument type mismatch");
                else if (arg != f->params[i]) emit(l, N_CONVERT, f->params[i], 0, arg);
            }
            emit(l, N_CALL, f->result, 0, index); return f->result;
        }
        case EXPR_IF: {
            size_t alternate = label(l), end = label(l);
            if (expression(l, e->if_expr.cond) != N_BOOL) fail(l, "if condition must be bool");
            emit(l, N_JZ, N_UNIT, 0, alternate);
            NType a = expression(l, e->if_expr.then);
            emit(l, N_JUMP, N_UNIT, 0, end); mark(l, alternate);
            NType b;
            if (e->if_expr.else_) b = expression(l, e->if_expr.else_);
            else { emit(l, N_CONST, N_UNIT, 0, 0); b = N_UNIT; }
            if (a != b) fail(l, "if expression arms must have the same supported type");
            mark(l, end); return a;
        }
        case EXPR_BLOCK: {
            NType result = N_UNIT;
            if (!e->block.count) emit(l, N_CONST, N_UNIT, 0, 0);
            for (int i = 0; i < e->block.count; i++) {
                result = expression(l, e->block.stmts[i]);
                if (i + 1 < e->block.count) emit(l, N_DROP, N_UNIT, 0, 0);
            }
            return result;
        }
        default: fail(l, "expression is not supported yet"); return N_INVALID;
    }
}

static NType expression(Lower *l, Re0Expr *e) {
    if (!e || l->m->codegen->had_error) return N_INVALID;
    if (++l->depth > N_MAX_DEPTH) { fail(l, "lowering depth limit exceeded"); l->depth--; return N_INVALID; }
    NType result = expression_impl(l, e);
    l->depth--;
    return result;
}

static void statement(Lower *l, Re0Stmt *s) {
    switch (s->kind) {
        case STMT_LET: {
            NType value = expression(l, s->let_stmt.init);
            NType type = s->let_stmt.type ? type_name(s->let_stmt.type) : value;
            if (type == N_UNIT || !compatible(value, type, s->let_stmt.init)) {
                fail(l, "unsupported local type or initializer"); break;
            }
            local(l, s->let_stmt.name, type);
            if (value != type) emit(l, N_CONVERT, type, 0, value);
            emit(l, N_STORE, type, 0, l->f->slots - 1); break;
        }
        case STMT_ASSIGN: {
            Local *v = lookup(l, s->assign.name);
            if (!v) { fail(l, "assignment requires a local variable"); break; }
            if (s->assign.op != BINOP_ASSIGN_SENTINEL) emit(l, N_LOAD, v->type, 0, v->slot);
            NType value = expression(l, s->assign.value);
            if (!compatible(value, v->type, s->assign.value)) fail(l, "assignment type mismatch");
            else if (value != v->type) emit(l, N_CONVERT, v->type, 0, value);
            if (s->assign.op != BINOP_ASSIGN_SENTINEL) {
                if (!n_type_integer(v->type) && !n_type_float(v->type)) fail(l, "compound assignment requires a number");
                emit(l, N_BINARY, v->type, 0, s->assign.op);
            }
            emit(l, N_STORE, v->type, 0, v->slot); break;
        }
        case STMT_EXPR: expression(l, s->expr_stmt.expr); emit(l, N_DROP, N_UNIT, 0, 0); break;
        case STMT_RETURN: {
            NType value = N_UNIT;
            if (s->return_stmt.value) value = expression(l, s->return_stmt.value);
            else emit(l, N_CONST, N_UNIT, 0, 0);
            if (!compatible(value, l->f->result, s->return_stmt.value)) fail(l, "return type mismatch");
            else if (value != l->f->result) emit(l, N_CONVERT, l->f->result, 0, value);
            emit(l, N_RETURN, l->f->result, 0, 0); break;
        }
        case STMT_IF: {
            size_t end = label(l);
            for (int i = 0; i < s->if_stmt.branch_count; i++) {
                size_t next = label(l);
                Re0IfBranch *b = &s->if_stmt.branches[i];
                if (expression(l, b->cond) != N_BOOL) fail(l, "if condition must be bool");
                emit(l, N_JZ, N_UNIT, 0, next);
                body(l, b->body, b->body_count);
                emit(l, N_JUMP, N_UNIT, 0, end); mark(l, next);
            }
            body(l, s->if_stmt.else_body, s->if_stmt.else_count); mark(l, end); break;
        }
        case STMT_WHILE: {
            if (l->loops == N_MAX_DEPTH) { fail(l, "loop nesting limit exceeded"); break; }
            size_t start = label(l), end = label(l);
            l->breaks[l->loops] = end; l->continues[l->loops++] = start;
            mark(l, start);
            if (expression(l, s->while_stmt.cond) != N_BOOL) fail(l, "while condition must be bool");
            emit(l, N_JZ, N_UNIT, 0, end);
            body(l, s->while_stmt.body, s->while_stmt.body_count);
            emit(l, N_JUMP, N_UNIT, 0, start); mark(l, end); l->loops--; break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (!l->loops || (s->kind == STMT_BREAK && s->break_stmt.value)) {
                fail(l, "unsupported break/continue"); break;
            }
            emit(l, N_JUMP, N_UNIT, 0, s->kind == STMT_BREAK ?
                 l->breaks[l->loops - 1] : l->continues[l->loops - 1]); break;
        default: fail(l, "statement is not supported yet"); break;
    }
}

static void body(Lower *l, Re0Stmt **stmts, int count) {
    if (++l->depth > N_MAX_DEPTH) { fail(l, "lowering depth limit exceeded"); l->depth--; return; }
    size_t saved = l->local_count;
    for (int i = 0; i < count && !l->m->codegen->had_error; i++) statement(l, stmts[i]);
    l->local_count = saved;
    l->depth--;
}

static void register_function(NModule *m, const char *name, const char *ret,
                              int count, Re0Stmt *ast, Re0ExternFnDecl *ext) {
    if (m->codegen->had_error) return;
    if (m->count == N_MAX_FUNCTIONS || count < 0 || count > N_MAX_PARAMS) {
        n_error(m, RE0_SPAN_ZERO, "function limit exceeded (1024 functions, 6 parameters)"); return;
    }
    if (!name || !*name || strcmp(name, "_start") == 0 || strcmp(name, "assert") == 0) {
        n_error(m, RE0_SPAN_ZERO, "invalid or reserved function name"); return;
    }
    for (size_t i = 0; i < m->count; i++) if (strcmp(m->functions[i].name, name) == 0) {
        n_error(m, RE0_SPAN_ZERO, "duplicate function symbol"); return;
    }
    NFunction *f = &m->functions[m->count++];
    f->name = name; f->result = type_name(ret); f->ast = ast; f->param_count = count;
    if (f->result == N_INVALID || (ext && ext->variadic))
        n_error(m, RE0_SPAN_ZERO, "unsupported native function signature");
    for (int i = 0; i < count; i++) {
        f->params[i] = type_name(ast ? ast->function.params[i].ptype : ext->params[i].ptype);
        if (f->params[i] == N_INVALID || f->params[i] == N_UNIT)
            n_error(m, RE0_SPAN_ZERO, "unsupported parameter type");
    }
}

bool n_lower(NModule *m, Re0StmtVec *checked) {
    for (size_t i = 0; i < checked->len && !m->codegen->had_error; i++) {
        Re0Stmt *s = checked->data[i];
        unsigned wrappers = 0;
        while (s && s->kind == STMT_PUB && wrappers++ < N_MAX_DEPTH) s = s->pub.inner;
        if (!s) { n_error(m, RE0_SPAN_ZERO, "missing declaration"); break; }
        if (s->kind == STMT_FUNCTION && !s->function.type_param_count && !s->function.is_async)
            register_function(m, s->function.name, s->function.ret_type, s->function.param_count, s, NULL);
        else if (s->kind == STMT_EXTERN) {
            for (int j = 0; j < s->extern_.func_count; j++) {
                Re0ExternFnDecl *e = &s->extern_.funcs[j];
                register_function(m, e->name, e->ret_type, e->param_count, NULL, e);
            }
        } else n_error(m, s->span, "top-level declaration is not supported yet");
    }
    Lower *l = calloc(1, sizeof(*l));
    if (!l) { n_error(m, RE0_SPAN_ZERO, "cannot allocate lowering state"); return false; }
    for (size_t i = 0; i < m->count && !m->codegen->had_error; i++) {
        NFunction *f = &m->functions[i];
        if (!f->ast) continue;
        memset(l, 0, sizeof(*l)); l->m = m; l->f = f;
        for (int j = 0; j < f->param_count; j++) local(l, f->ast->function.params[j].name, f->params[j]);
        body(l, f->ast->function.body, f->ast->function.body_count);
        emit(l, N_END, f->result, 0, 0);
    }
    free(l);
    return !m->codegen->had_error;
}

bool n_verify(NModule *m, NFunction *f) {
    size_t *labels = malloc((f->labels + 1) * sizeof(*labels));
    size_t *queue = malloc((f->count + 1) * sizeof(*queue));
    if (!labels || !queue) { free(labels); free(queue); n_error(m, f->ast->span, "cannot allocate IR verifier"); return false; }
    for (size_t i = 0; i < f->labels; i++) labels[i] = SIZE_MAX;
    for (size_t i = 0; i < f->count; i++) {
        NInst *in = &f->ir[i];
        if (in->op < N_CONST || in->op > N_CONVERT || in->type < N_UNIT || in->type >= N_INVALID)
            n_error(m, f->ast->span, "invalid IR opcode or type");
        if (in->op == N_CONVERT && (in->arg >= N_INVALID || in->arg == N_UNIT || in->type == N_UNIT))
            n_error(m, f->ast->span, "invalid IR conversion type");
        if (in->op == N_LABEL) {
            if (in->arg >= f->labels || labels[in->arg] != SIZE_MAX) n_error(m, f->ast->span, "invalid IR label");
            else labels[in->arg] = i;
        }
        if ((in->op == N_LOAD || in->op == N_STORE) && in->arg >= f->slots)
            n_error(m, f->ast->span, "invalid IR local slot");
        if (in->op == N_CALL && in->arg >= m->count) n_error(m, f->ast->span, "invalid IR call");
    }
    size_t head = 0, tail = 0;
    if (f->count) { f->ir[0].depth = 0; queue[tail++] = 0; }
    while (head < tail && !m->codegen->had_error) {
        size_t index = queue[head++]; NInst *in = &f->ir[index];
        int need = 0, change = 0;
        switch (in->op) {
            case N_CONST: case N_LOAD: change = 1; break;
            case N_STORE: case N_DROP: case N_JZ: case N_ASSERT: case N_RETURN: need = 1; change = -1; break;
            case N_BINARY: need = 2; change = -1; break;
            case N_UNARY: case N_CONVERT: need = 1; break;
            case N_CALL: need = m->functions[in->arg].param_count; change = 1 - need; break;
            default: break;
        }
        int depth = in->depth + change;
        if (in->depth < need || depth > N_MAX_IR) { n_error(m, f->ast->span, "invalid IR stack depth"); break; }
        if (in->op == N_RETURN || in->op == N_END) {
            if (depth != 0 || (in->op == N_END && f->result != N_UNIT))
                n_error(m, f->ast->span, "non-unit function can fall through without an explicit return");
            continue;
        }
        size_t targets[2] = {index + 1, SIZE_MAX};
        if (in->op == N_JUMP || in->op == N_JZ) {
            if (in->arg >= f->labels || labels[in->arg] == SIZE_MAX) {
                n_error(m, f->ast->span, "undefined IR label"); break;
            }
            targets[in->op == N_JUMP ? 0 : 1] = labels[in->arg];
        }
        for (unsigned j = 0; j < 2; j++) {
            size_t target = targets[j]; if (target == SIZE_MAX) continue;
            if (target >= f->count) { n_error(m, f->ast->span, "IR falls off function"); break; }
            if (f->ir[target].depth == -1) { f->ir[target].depth = depth; queue[tail++] = target; }
            else if (f->ir[target].depth != depth) n_error(m, f->ast->span, "inconsistent IR stack at control-flow join");
        }
    }
    free(labels); free(queue);
    return !m->codegen->had_error;
}
