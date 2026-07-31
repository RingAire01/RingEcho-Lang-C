#include "safe.h"
#include "sema.h"
#include <stdlib.h>
#include <string.h>
#include "re0_limits.h"

/* 解析类型名：标准类型 → 类型别名 → 具名 struct/enum（避免具名类型塌缩为 UNIT/UNKNOWN） */
static Re0Type *resolve_type(Re0Sema *s, const char *name) {
    if (!name) return NULL;
    Re0Type *t = re0_model_std_type(name);
    if (t) return t;
    const char *resolved = re0_model_resolve_type_alias(s->model, name);
    if (resolved) {
        Re0Type *rt = re0_model_std_type(resolved);
        if (rt) return rt;
        name = resolved; /* 别名指向具名类型时继续按具名解析 */
    }
    if (re0_model_find_struct(s->model, name))
        return re0_type_make_named(RE0_TYPE_STRUCT, name, NULL);
    if (re0_model_find_enum(s->model, name))
        return re0_type_make_named(RE0_TYPE_ENUM, name, NULL);
    return NULL;
}

/* 类型可赋值性（from → to）：真类型校验。
 * numeric 可宽转；UNKNOWN/TYPEVAR/NEVER/UNIT 目标宽容；其余须结构相等。 */
static bool sema_assignable(Re0Type *from, Re0Type *to) {
    if (!from || !to) return true;
    if (from->kind == RE0_TYPE_UNKNOWN || to->kind == RE0_TYPE_UNKNOWN) return true;
    if (from->kind == RE0_TYPE_TYPEVAR || to->kind == RE0_TYPE_TYPEVAR) return true;
    if (from->kind == RE0_TYPE_NEVER) return true;
    if (to->kind == RE0_TYPE_UNIT) return true;
    return re0_type_coercible(from, to); /* equal 或 numeric 互转 */
}

static int sema_cap_count(Re0Sema *s, Re0Span span, int count, int limit, const char *what) {
    if (count > limit) {
        re0_error_append(s->errors, RE0_ERR_SEMANTIC, span, NULL,
                         "too many %s (limit %d), extras ignored", what, limit);
        return limit;
    }
    return count;
}

void re0_sema_init(Re0Sema *s, Re0Arena *arena, Re0ErrorList *errors,
                   Re0SemanticModel *model, Re0BuiltinRegistry *builtins) {
    s->arena = arena; s->errors = errors; s->model = model; s->builtins = builtins;
    s->global_scope = re0_scope_new(NULL); s->current_scope = s->global_scope;
    Re0StmtVec_init(&s->checked); s->had_error = false; s->infer_depth = 0;
    s->current_fn_return = NULL;
    s->loop_depth = 0; s->fn_depth = 0;

    /* 预注入 Option/Result 核心枚举 */
    if (!re0_model_find_enum(model, "Option")) {
        char *ov[] = {"None", "Some"};
        int op[] = {0, 1};
        re0_model_register_enum(model, "Option", ov, op, 2);
    }
    if (!re0_model_find_enum(model, "Result")) {
        char *rv[] = {"Ok", "Err"};
        int rp[] = {1, 1};
        re0_model_register_enum(model, "Result", rv, rp, 2);
    }
}

static Re0Type *infer_type(Re0Sema *s, Re0Expr *e);
static Re0Type *infer_type_impl(Re0Sema *s, Re0Expr *e);

static Re0Type *infer_type(Re0Sema *s, Re0Expr *e) {
    if (!e) return re0_type_make(RE0_TYPE_UNIT, NULL);
    if (s->infer_depth > RE0_MAX_SEMA_DEPTH) return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
    s->infer_depth++;
    Re0Type *t = infer_type_impl(s, e);
    s->infer_depth--;
    return t;
}

static Re0Type *infer_type_impl(Re0Sema *s, Re0Expr *e) {
    if (!e) return re0_type_make(RE0_TYPE_UNIT, NULL);
    switch (e->kind) {
        case EXPR_INT: return re0_type_make(RE0_TYPE_I64, NULL);
        case EXPR_FLOAT: return re0_type_make(RE0_TYPE_F64, NULL);
        case EXPR_BOOL: return re0_type_make(RE0_TYPE_BOOL, NULL);
        case EXPR_CHAR: return re0_type_make(RE0_TYPE_CHAR, NULL);
        case EXPR_STRING: return re0_type_make(RE0_TYPE_STR, NULL);
        case EXPR_UNIT: return re0_type_make(RE0_TYPE_UNIT, NULL);
        case EXPR_IDENT: {
            if (strchr(e->ident.name, ':')) {
                char enum_name[128];
                const char *colon = strchr(e->ident.name, ':');
                size_t len = (size_t)(colon - e->ident.name);
                if (len < sizeof(enum_name)) {
                    memcpy(enum_name, e->ident.name, len);
                    enum_name[len] = '\0';
                    if (re0_model_find_enum(s->model, enum_name)) {
                        Re0Type *t = re0_type_make(RE0_TYPE_ENUM, NULL);
                        t->named.name = strdup(enum_name);
                        return t;
                    }
                }
            }
            Re0Symbol *sym = re0_scope_lookup(s->current_scope, e->ident.name);
            if (sym) return sym->type;
            Re0BuiltinFn *bf = re0_builtin_lookup(s->builtins, e->ident.name);
            if (bf) return bf->ret_type;
            /* undefined variable */
            re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                            "undefined variable '%s'", e->ident.name);
            s->had_error = true;
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
        }
        case EXPR_BINARY: {
            Re0Type *lt = infer_type(s, e->binary.left);
            Re0Type *rt = infer_type(s, e->binary.right);
            switch (e->binary.op) {
                case BINOP_EQ: case BINOP_NE: case BINOP_LT:
                case BINOP_LE: case BINOP_GT: case BINOP_GE:
                case BINOP_AND: case BINOP_OR:
                    return re0_type_make(RE0_TYPE_BOOL, NULL);
                case BINOP_RANGE:
                    return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                default:
                    /* 算术/位运算：返回左操作数类型（左未知时用右），保持宽容 */
                    if (lt && lt->kind != RE0_TYPE_UNKNOWN) return lt;
                    if (rt) return rt;
                    return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
        }
        case EXPR_UNARY: {
            Re0Type *operand = infer_type(s, e->unary.operand);
            if (!operand) return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            switch (e->unary.op) {
                case UNOP_NEG:
                    if ((!re0_type_is_numeric(operand->kind) ||
                         !re0_type_is_signed(operand->kind)) &&
                        operand->kind != RE0_TYPE_UNKNOWN) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "cannot negate type '%s'",
                                         re0_type_kind_name(operand->kind));
                        s->had_error = true;
                    }
                    return operand;
                case UNOP_NOT:
                    if (operand->kind != RE0_TYPE_BOOL && operand->kind != RE0_TYPE_UNKNOWN) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "cannot apply 'not' to type '%s'",
                                         re0_type_kind_name(operand->kind));
                        s->had_error = true;
                    }
                    return re0_type_make(RE0_TYPE_BOOL, NULL);
                case UNOP_REF:
                case UNOP_REFMUT: {
                    if (e->unary.op == UNOP_REFMUT &&
                        (!e->unary.operand || e->unary.operand->kind != EXPR_IDENT)) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "mutable reference requires a named variable");
                        s->had_error = true;
                    }
                    Re0Type *ref = re0_type_make(RE0_TYPE_REFERENCE, NULL);
                    if (!ref) return NULL;
                    ref->ref_.inner = operand;
                    ref->ref_.mutable_ = e->unary.op == UNOP_REFMUT;
                    return ref;
                }
                case UNOP_DEREF:
                    if (operand->kind == RE0_TYPE_REFERENCE) return operand->ref_.inner;
                    if (operand->kind != RE0_TYPE_PTR && operand->kind != RE0_TYPE_UNKNOWN) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "cannot dereference non-pointer type '%s'",
                                         re0_type_kind_name(operand->kind));
                        s->had_error = true;
                    }
                    return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
        }
        case EXPR_CALL: {
            /* 方法调用: obj.method(args) → callee 是 EXPR_SELECT */
            if (e->call.callee->kind == EXPR_SELECT) {
                Re0Expr *sel = e->call.callee;
                Re0Type *obj_ty = infer_type(s, sel->select.object);
                if (obj_ty && obj_ty->kind == RE0_TYPE_STRUCT && obj_ty->named.name) {
                    const char *mangled = re0_model_lookup_method(
                        s->model, obj_ty->named.name, sel->select.field);
                    if (mangled) {
                        /* 查 mangled 符号的 FN 类型以取真实返回类型 */
                        Re0Symbol *msym = re0_scope_lookup(s->global_scope, mangled);
                        if (msym && msym->type && msym->type->kind == RE0_TYPE_FN &&
                            msym->type->func.ret)
                            return msym->type->func.ret;
                        return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                    }
                }
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            /* 枚举构造器: Enum::Variant(args) */
            if (e->call.callee->kind == EXPR_IDENT &&
                strchr(e->call.callee->ident.name, ':')) {
                char enum_name[128];
                const char *colon = strchr(e->call.callee->ident.name, ':');
                size_t elen = (size_t)(colon - e->call.callee->ident.name);
                if (elen < sizeof(enum_name)) {
                    memcpy(enum_name, e->call.callee->ident.name, elen);
                    enum_name[elen] = '\0';
                    Re0EnumDef *ed = re0_model_find_enum(s->model, enum_name);
                    if (ed) {
                        /* variant 名在 "::" 之后;校验 variant 存在性与 payload 数 */
                        const char *variant_name = colon + (colon[1] == ':' ? 2 : 1);
                        if (*variant_name) {
                            int tag = re0_model_variant_tag(ed, variant_name);
                            if (tag < 0) {
                                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                                 "enum '%s' has no variant '%s'",
                                                 enum_name, variant_name);
                                s->had_error = true;
                            } else {
                                int has_payload = ed->variant_has_payload[tag];
                                int argc = e->call.arg_count;
                                if (has_payload && argc == 0) {
                                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                                     "variant '%s::%s' expects a payload argument",
                                                     enum_name, variant_name);
                                    s->had_error = true;
                                } else if (!has_payload && argc > 0) {
                                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                                     "variant '%s::%s' takes no payload argument",
                                                     enum_name, variant_name);
                                    s->had_error = true;
                                }
                            }
                        }
                        for (int ai = 0; ai < e->call.arg_count; ai++)
                            infer_type(s, e->call.args[ai]);
                        Re0Type *t = re0_type_make(RE0_TYPE_ENUM, NULL);
                        t->named.name = strdup(enum_name);
                        return t;
                    }
                }
            }
            /* spawn/await 内置函数 */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                if (strcmp(fn, "__reo_spawn") == 0 || strcmp(fn, "__reo_await") == 0)
                    return re0_type_make(RE0_TYPE_I64, NULL);
            }
            if (e->call.callee->kind == EXPR_IDENT) {
                Re0BuiltinFn *bf = re0_builtin_lookup(s->builtins, e->call.callee->ident.name);
                if (bf) return bf->ret_type;
                /* 用户函数 / 局部 lambda：按 FN 类型解析（current_scope 链至 global） */
                Re0Symbol *sym = re0_scope_lookup(s->current_scope, e->call.callee->ident.name);
                if (sym && sym->type && sym->type->kind == RE0_TYPE_FN) {
                    int fixed_params = sym->type->func.param_count;
                    if ((!sym->type->func.variadic && e->call.arg_count != fixed_params) ||
                        (sym->type->func.variadic && e->call.arg_count < fixed_params)) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC,
                                         e->call.callee->span, NULL,
                                         "function '%s' expects %d argument(s), got %d",
                                         e->call.callee->ident.name, fixed_params,
                                         e->call.arg_count);
                        s->had_error = true;
                    }
                    /* B4: 逐参类型校验（TYPEVAR/UNKNOWN 宽容） */
                    for (int ai = 0; ai < e->call.arg_count &&
                                    ai < sym->type->func.param_count; ai++) {
                        Re0Type *at = infer_type(s, e->call.args[ai]);
                        Re0Type *pt = sym->type->func.params[ai];
                        if (at && pt && !sema_assignable(at, pt)) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC,
                                             e->call.callee->span, NULL,
                                             "argument %d of '%s': expected '%s', got '%s'",
                                             ai + 1, e->call.callee->ident.name,
                                             re0_type_kind_name(pt->kind),
                                             re0_type_kind_name(at->kind));
                            s->had_error = true;
                        }
                    }
                    return sym->type->func.ret
                        ? sym->type->func.ret : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                }
                re0_error_append(s->errors, RE0_ERR_SEMANTIC,
                                e->call.callee->span, NULL,
                                "undefined function '%s'",
                                e->call.callee->ident.name);
                s->had_error = true;
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            return infer_type(s, e->call.callee);
        }
        case EXPR_IF:
            return e->if_expr.else_ ? infer_type(s, e->if_expr.then)
                                   : re0_type_make(RE0_TYPE_UNIT, NULL);
        case EXPR_STRUCT_INIT: {
            Re0Type *t = re0_type_make(RE0_TYPE_STRUCT, NULL);
            t->named.name = strdup(e->struct_init.name);
            /* 字段校验(仅对已知非泛型 struct): 多余字段 / 缺失字段 / 类型不匹配 */
            Re0StructDef *sd = re0_model_find_struct(s->model, e->struct_init.name);
            if (sd) {
                for (int i = 0; i < e->struct_init.field_count; i++) {
                    const char *fn = e->struct_init.fields[i].field;
                    Re0Expr *val = e->struct_init.fields[i].value;
                    Re0Type *vt = val ? infer_type(s, val) : NULL;
                    int found = 0;
                    for (int j = 0; j < sd->field_count; j++) {
                        if (strcmp(sd->fields[j].name, fn) != 0) continue;
                        found = 1;
                        Re0Type *ft = sd->fields[j].type;
                        if (vt && ft && vt->kind != RE0_TYPE_UNKNOWN &&
                            ft->kind != RE0_TYPE_UNKNOWN && !sema_assignable(vt, ft)) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                "field '%s' of struct '%s': expected '%s', got '%s'",
                                fn, e->struct_init.name,
                                re0_type_kind_name(ft->kind), re0_type_kind_name(vt->kind));
                            s->had_error = true;
                        }
                        break;
                    }
                    if (!found) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                            "struct '%s' has no field '%s'", e->struct_init.name, fn);
                        s->had_error = true;
                    }
                }
                for (int j = 0; j < sd->field_count; j++) {
                    const char *dn = sd->fields[j].name;
                    int found = 0;
                    for (int i = 0; i < e->struct_init.field_count; i++)
                        if (strcmp(e->struct_init.fields[i].field, dn) == 0) { found = 1; break; }
                    if (!found) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                            "missing field '%s' in initializer for struct '%s'",
                            dn, e->struct_init.name);
                        s->had_error = true;
                    }
                }
            } else {
                for (int i = 0; i < e->struct_init.field_count; i++)
                    if (e->struct_init.fields[i].value)
                        infer_type(s, e->struct_init.fields[i].value);
            }
            return t;
        }
        case EXPR_MATCH:
            if (e->match_.arm_count > 0) return infer_type(s, e->match_.arms[0].body);
            return re0_type_make(RE0_TYPE_UNIT, NULL);
        case EXPR_SELECT: {
            /* 字段访问：返回字段真实类型（仅具体类型；泛型/未知落 UNKNOWN，保持宽容） */
            Re0Type *obj_type = infer_type(s, e->select.object);
            if (obj_type && obj_type->kind == RE0_TYPE_STRUCT && obj_type->named.name) {
                Re0StructDef *sd = re0_model_find_struct(s->model, obj_type->named.name);
                if (sd) {
                    for (int i = 0; i < sd->field_count; i++) {
                        if (strcmp(sd->fields[i].name, e->select.field) == 0) {
                            Re0Type *ft = sd->fields[i].type;
                            if (ft && ft->kind != RE0_TYPE_TYPEVAR &&
                                ft->kind != RE0_TYPE_UNKNOWN)
                                return ft;
                            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                        }
                    }
                }
            }
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
        }
        case EXPR_INDEX: {
            /* 索引：按集合类型推元素类型 */
            Re0Type *obj = infer_type(s, e->index.target);
            if (e->index.index) infer_type(s, e->index.index);
            if (obj) {
                switch (obj->kind) {
                    case RE0_TYPE_ARRAY:
                        return obj->array.inner ? obj->array.inner
                                                : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                    case RE0_TYPE_SLICE:
                        return obj->slice.inner ? obj->slice.inner
                                                : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                    case RE0_TYPE_VEC:
                        return obj->vec.inner ? obj->vec.inner
                                              : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                    case RE0_TYPE_STR:
                        return re0_type_make(RE0_TYPE_CHAR, NULL);
                    default: break;
                }
            }
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
        }
        case EXPR_ARRAY: {
            /* 数组字面量：逐元素推断，取首个具体类型为元素类型 */
            if (e->array.count == 0)
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            Re0Type *elem = NULL;
            for (int i = 0; i < e->array.count; i++) {
                Re0Type *t = infer_type(s, e->array.elems[i]);
                if (t && t->kind != RE0_TYPE_UNKNOWN) { elem = t; break; }
            }
            if (!elem) elem = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            return re0_type_make_array(elem, (size_t)e->array.count, NULL);
        }
        case EXPR_ARRAY_REPEAT: {
            /* [value; count]：count 为整型字面量时定长数组，否则切片 */
            Re0Type *elem = infer_type(s, e->array_repeat.value);
            if (!elem) elem = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            if (e->array_repeat.count && e->array_repeat.count->kind == EXPR_INT &&
                e->array_repeat.count->int_lit.val >= 0)
                return re0_type_make_array(elem,
                            (size_t)e->array_repeat.count->int_lit.val, NULL);
            return re0_type_make_slice(elem, NULL);
        }
        case EXPR_TUPLE: {
            Re0Type *elems[64];
            int n = e->tuple.count > 64 ? 64 : e->tuple.count;
            for (int i = 0; i < n; i++) {
                Re0Type *t = infer_type(s, e->tuple.elems[i]);
                elems[i] = t ? t : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            return re0_type_make_tuple(elems, n, NULL);
        }
        case EXPR_TRY: {
            /* expr? — 检查内部类型，返回 payload 类型（MVP: i64） */
            if (e->try_.inner) infer_type(s, e->try_.inner);
            return re0_type_make(RE0_TYPE_I64, NULL);
        }
        case EXPR_LAMBDA: {
            /* lambda: 开子作用域绑定参数，推断 Fn(params, ret) */
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_scope_new(s->current_scope);
            Re0Type *params[64];
            int pc = e->lambda.param_count > 64 ? 64 : e->lambda.param_count;
            for (int i = 0; i < pc; i++) {
                Re0Type *pt = e->lambda.params[i].type
                    ? resolve_type(s, e->lambda.params[i].type) : NULL;
                if (!pt) pt = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                params[i] = pt;
                re0_scope_define(s->current_scope, e->lambda.params[i].name, pt, false);
            }
            Re0Type *ret = e->lambda.body
                ? infer_type(s, e->lambda.body) : re0_type_make(RE0_TYPE_UNIT, NULL);
            s->current_scope = saved;
            return re0_type_make_func(params, pc, ret, false, NULL);
        }
        default: return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
    }
}

static void check_stmt_inner(Re0Sema *s, Re0Stmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case STMT_LET: {
            /* check for duplicate definition */
            if (re0_scope_lookup_local(s->current_scope, stmt->let_stmt.name)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                "duplicate definition of '%s'", stmt->let_stmt.name);
                s->had_error = true;
                break;
            }
            Re0Type *anno = stmt->let_stmt.type ? resolve_type(s, stmt->let_stmt.type) : NULL;
            Re0Type *init_ty = stmt->let_stmt.init ? infer_type(s, stmt->let_stmt.init) : NULL;
            /* B1: 注解 vs 初始化类型校验 */
            if (anno && init_ty && !sema_assignable(init_ty, anno)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                "type mismatch: '%s' annotated '%s' but initializer is '%s'",
                                stmt->let_stmt.name, re0_type_kind_name(anno->kind),
                                re0_type_kind_name(init_ty->kind));
                s->had_error = true;
            }
            Re0Type *type = anno ? anno : init_ty;
            if (!type) type = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            re0_scope_define(s->current_scope, stmt->let_stmt.name, type, true);
            break;
        }
        case STMT_ASSIGN: {
            /* check variable exists */
            Re0Symbol *sym = re0_scope_lookup(s->current_scope, stmt->assign.name);
            if (!sym) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                "assignment to undefined variable '%s'", stmt->assign.name);
                s->had_error = true;
            }
            if (stmt->assign.value) {
                Re0Type *vt = infer_type(s, stmt->assign.value);
                /* B2: 赋值类型校验 */
                if (sym && sym->type && vt && !sema_assignable(vt, sym->type)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                    "assignment type mismatch: '%s' is '%s', got '%s'",
                                    stmt->assign.name, re0_type_kind_name(sym->type->kind),
                                    re0_type_kind_name(vt->kind));
                    s->had_error = true;
                }
            }
            break;
        }
        case STMT_EXPR:
            if (stmt->expr_stmt.expr) infer_type(s, stmt->expr_stmt.expr);
            break;
        case STMT_IF: {
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_scope_new(s->current_scope);
            for (int i = 0; i < stmt->if_stmt.branch_count; i++) {
                infer_type(s, stmt->if_stmt.branches[i].cond);
                for (int j = 0; j < stmt->if_stmt.branches[i].body_count; j++)
                    check_stmt_inner(s, stmt->if_stmt.branches[i].body[j]);
            }
            if (stmt->if_stmt.else_body)
                for (int i = 0; i < stmt->if_stmt.else_count; i++)
                    check_stmt_inner(s, stmt->if_stmt.else_body[i]);
            s->current_scope = saved;
            break;
        }
        case STMT_WHILE: {
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_scope_new(s->current_scope);
            infer_type(s, stmt->while_stmt.cond);
            s->loop_depth++;
            for (int i = 0; i < stmt->while_stmt.body_count; i++)
                check_stmt_inner(s, stmt->while_stmt.body[i]);
            s->loop_depth--;
            s->current_scope = saved;
            break;
        }
        case STMT_FOR: {
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_scope_new(s->current_scope);
            /* 按迭代器推循环变量类型：range→i64，Vec/Array/Slice→元素，str→char，其余→i64 */
            Re0Expr *iter = stmt->for_stmt.iter;
            Re0Type *iter_ty = iter ? infer_type(s, iter) : NULL;
            Re0Type *var_ty = NULL;
            if (iter && iter->kind == EXPR_BINARY && iter->binary.op == BINOP_RANGE) {
                var_ty = re0_type_make(RE0_TYPE_I64, NULL);
            } else if (iter_ty) {
                switch (iter_ty->kind) {
                    case RE0_TYPE_ARRAY:
                        var_ty = iter_ty->array.inner; break;
                    case RE0_TYPE_SLICE:
                        var_ty = iter_ty->slice.inner; break;
                    case RE0_TYPE_VEC:
                        var_ty = iter_ty->vec.inner; break;
                    case RE0_TYPE_STR:
                        var_ty = re0_type_make(RE0_TYPE_CHAR, NULL); break;
                    default:
                        var_ty = re0_type_make(RE0_TYPE_I64, NULL); break;
                }
            }
            if (!var_ty) var_ty = re0_type_make(RE0_TYPE_I64, NULL);
            re0_scope_define(s->current_scope, stmt->for_stmt.var, var_ty, true);
            s->loop_depth++;
            for (int i = 0; i < stmt->for_stmt.body_count; i++)
                check_stmt_inner(s, stmt->for_stmt.body[i]);
            s->loop_depth--;
            s->current_scope = saved;
            break;
        }
        case STMT_RETURN:
            if (s->fn_depth == 0) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                 "return statement outside of a function");
                s->had_error = true;
            }
            if (stmt->return_stmt.value) {
                Re0Type *vt = infer_type(s, stmt->return_stmt.value);
                /* B3: 返回类型校验（对照当前函数返回类型） */
                if (s->current_fn_return && vt &&
                    !sema_assignable(vt, s->current_fn_return)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                    "return type mismatch: expected '%s', got '%s'",
                                    re0_type_kind_name(s->current_fn_return->kind),
                                    re0_type_kind_name(vt->kind));
                    s->had_error = true;
                }
            }
            break;
        case STMT_BREAK:
            if (s->loop_depth == 0) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                 "break statement outside of a loop");
                s->had_error = true;
            }
            if (stmt->break_stmt.value) infer_type(s, stmt->break_stmt.value);
            break;
        case STMT_CONTINUE:
            if (s->loop_depth == 0) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                 "continue statement outside of a loop");
                s->had_error = true;
            }
            break;
        case STMT_FUNCTION: {
            /* check for duplicate function definition */
            if (re0_scope_lookup_local(s->global_scope, stmt->function.name)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                "duplicate function definition '%s'", stmt->function.name);
                s->had_error = true;
                break;
            }
            Re0Scope *saved = s->current_scope;
            Re0Type *params[64];
            int param_count = stmt->function.param_count;
            param_count = sema_cap_count(s, stmt->span, param_count, 64, "parameters");
            for (int i = 0; i < param_count; i++) {
                params[i] = resolve_type(s, stmt->function.params[i].ptype);
                if (!params[i]) params[i] = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            Re0Type *ret = resolve_type(s, stmt->function.ret_type);
            if (!ret) ret = re0_type_make(RE0_TYPE_UNIT, NULL);
            Re0Type *ft = re0_type_make_func(params, param_count, ret, false, NULL);
            if (!ft) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                 "cannot allocate signature for function '%s'",
                                 stmt->function.name);
                s->had_error = true;
                s->current_scope = saved;
                break;
            }
            Re0Symbol *sym = re0_scope_lookup_local(s->global_scope, stmt->function.name);
            if (!sym) {
                re0_scope_define(s->global_scope, stmt->function.name, ft, false);
                sym = re0_scope_lookup_local(s->global_scope, stmt->function.name);
            }
            if (sym) sym->is_function = true;

            /* 注册函数签名到 model（供后续 trait/generics 使用） */
            re0_model_register_fn(s->model, stmt->function.name,
                                  NULL, param_count,
                                  stmt->function.ret_type,
                                  stmt->function.type_params,
                                  stmt->function.type_param_count);

            s->current_scope = re0_scope_new(s->global_scope);
            for (int i = 0; i < stmt->function.param_count; i++) {
                Re0Type *pt = resolve_type(s, stmt->function.params[i].ptype);
                if (!pt) pt = re0_type_make(RE0_TYPE_I64, NULL);
                re0_scope_define(s->current_scope, stmt->function.params[i].name, pt, false);
            }
            Re0Type *prev_fn_return = s->current_fn_return;
            s->current_fn_return = ret;
            s->fn_depth++;
            for (int i = 0; i < stmt->function.body_count; i++)
                check_stmt_inner(s, stmt->function.body[i]);
            s->fn_depth--;
            s->current_fn_return = prev_fn_return;
            s->current_scope = saved;
            break;
        }
        case STMT_STRUCT: {
            char *field_names[64], *field_types[64];
            int n = stmt->struct_decl.field_count;
            n = sema_cap_count(s, stmt->span, n, 64, "struct fields");
            for (int i = 0; i < n; i++) {
                field_names[i] = stmt->struct_decl.fields[i].name;
                field_types[i] = stmt->struct_decl.fields[i].type;
            }
            re0_model_register_struct(s->model, stmt->struct_decl.name,
                                     field_names, field_types, n);
            break;
        }
        case STMT_ENUM: {
            char *vnames[64]; int has_payload[64];
            int n = stmt->enum_decl.variant_count;
            n = sema_cap_count(s, stmt->span, n, 64, "enum variants");
            for (int i = 0; i < n; i++) {
                vnames[i] = stmt->enum_decl.variants[i].vname;
                has_payload[i] = stmt->enum_decl.variants[i].type_count > 0 ? 1 : 0;
            }
            re0_model_register_enum(s->model, stmt->enum_decl.name,
                                   vnames, has_payload, n);
            break;
        }
        case STMT_EXTERN:
            for (int i = 0; i < stmt->extern_.func_count; i++) {
                Re0ExternFnDecl *decl = &stmt->extern_.funcs[i];
                if (re0_scope_lookup_local(s->global_scope, decl->name)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                     "duplicate function definition '%s'", decl->name);
                    s->had_error = true;
                    continue;
                }

                Re0Type *params[64];
                int param_count = decl->param_count;
                param_count = sema_cap_count(s, stmt->span, param_count, 64, "parameters");
                for (int j = 0; j < param_count; j++) {
                    params[j] = re0_model_std_type(decl->params[j].ptype);
                    if (!params[j]) params[j] = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                }
                Re0Type *ret = decl->ret_type
                    ? re0_model_std_type(decl->ret_type) : NULL;
                if (!ret) ret = re0_type_make(RE0_TYPE_UNIT, NULL);
                Re0Type *fn_type = re0_type_make_func(params, param_count, ret,
                                                       decl->variadic, NULL);
                if (!fn_type) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                     "cannot allocate signature for external function '%s'",
                                     decl->name);
                    s->had_error = true;
                    continue;
                }
                re0_scope_define(s->global_scope, decl->name, fn_type, false);
                Re0Symbol *symbol = re0_scope_lookup_local(s->global_scope, decl->name);
                if (symbol) symbol->is_function = true;
            }
            break;
        case STMT_TYPE_ALIAS:
            re0_model_register_type_alias(s->model, stmt->type_alias.name,
                                          stmt->type_alias.target);
            break;
        case STMT_CONST: {
            Re0Type *type = NULL;
            if (stmt->const_decl.type) type = resolve_type(s, stmt->const_decl.type);
            if (stmt->const_decl.value) type = type ? type : infer_type(s, stmt->const_decl.value);
            if (!type) type = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            re0_scope_define(s->current_scope, stmt->const_decl.name, type, false);
            break;
        }
        case STMT_TRAIT: {
            Re0TraitMethod *tms = (Re0TraitMethod*)xcalloc(
                (size_t)(stmt->trait_decl.method_count > 0 ? stmt->trait_decl.method_count : 1),
                sizeof(Re0TraitMethod));
            for (int i = 0; i < stmt->trait_decl.method_count; i++) {
                Re0TraitMethodDecl *d = &stmt->trait_decl.methods[i];
                tms[i].name = d->mname;
                tms[i].param_count = d->param_count;
                tms[i].ret_type = d->ret_type;
                if (d->param_count > 0) {
                    tms[i].param_types = (char**)xcalloc((size_t)d->param_count, sizeof(char*));
                    for (int j = 0; j < d->param_count; j++)
                        tms[i].param_types[j] = d->params[j].ptype;
                }
            }
            re0_model_register_trait(s->model, stmt->trait_decl.name,
                                     tms, stmt->trait_decl.method_count);
            for (int i = 0; i < stmt->trait_decl.method_count; i++)
                free(tms[i].param_types);
            free(tms);
            break;
        }
        case STMT_IMPL: {
            const char *sn = stmt->impl.name;
            const char *tn = stmt->impl.trait_name;

            if (!re0_model_find_struct(s->model, sn)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                 "cannot impl for undefined struct '%s'", sn);
                s->had_error = true;
                break;
            }
            if (tn) {
                if (!re0_model_find_trait(s->model, tn)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                     "undefined trait '%s'", tn);
                    s->had_error = true;
                    break;
                }
                if (re0_model_has_impl(s->model, sn, tn)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                     "duplicate impl of '%s' for '%s'", tn, sn);
                    s->had_error = true;
                    break;
                }
            }
            re0_model_register_impl(s->model, sn, tn);

            /* 注册方法到派发表 + 全局作用域 */
            for (int i = 0; i < stmt->impl.method_count; i++) {
                Re0Stmt *m = stmt->impl.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                char mangled_buf[256];
                const char *mangled = re0_model_method_symbol(tn, sn, m->function.name, mangled_buf, sizeof(mangled_buf));
                re0_model_register_method(s->model, sn, m->function.name, mangled);
                if (!re0_scope_lookup_local(s->global_scope, mangled)) {
                    Re0Type *params[64];
                    int pc = m->function.param_count;
                    pc = sema_cap_count(s, stmt->span, pc, 64, "parameters");
                    for (int j = 0; j < pc; j++) {
                        params[j] = resolve_type(s, m->function.params[j].ptype);
                        if (!params[j]) params[j] = re0_type_make(RE0_TYPE_I64, NULL);
                    }
                    Re0Type *ret = resolve_type(s, m->function.ret_type);
                    if (!ret) ret = re0_type_make(RE0_TYPE_UNIT, NULL);
                    Re0Type *ft = re0_type_make_func(params, pc, ret, false, NULL);
                    if (ft) {
                        re0_scope_define(s->global_scope, (char*)mangled, ft, false);
                        Re0Symbol *sym = re0_scope_lookup_local(s->global_scope, mangled);
                        if (sym) sym->is_function = true;
                    }
                }
            }

            /* trait 方法完整性 + 签名检查 */
            if (tn) {
                Re0TraitDef *td = re0_model_find_trait(s->model, tn);
                if (td) {
                    for (int i = 0; i < td->method_count; i++) {
                        Re0TraitMethod *tm = &td->methods[i];
                        Re0Stmt *impl_m = NULL;
                        for (int j = 0; j < stmt->impl.method_count; j++) {
                            Re0Stmt *m = stmt->impl.methods[j];
                            if (m && m->kind == STMT_FUNCTION &&
                                strcmp(m->function.name, tm->name) == 0) {
                                impl_m = m;
                                break;
                            }
                        }
                        if (!impl_m) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                             "missing method '%s' in impl of '%s' for '%s'",
                                             tm->name, tn, sn);
                            s->had_error = true;
                            continue;
                        }
                        /* 参数个数 */
                        if (impl_m->function.param_count != tm->param_count) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                             "method '%s' of trait '%s': expected %d parameter(s), got %d",
                                             tm->name, tn, tm->param_count, impl_m->function.param_count);
                            s->had_error = true;
                        } else {
                            /* 逐参数类型(self 等无显式类型的参数两侧皆 NULL,自动跳过) */
                            for (int k = 0; k < tm->param_count; k++) {
                                const char *tp = tm->param_types ? tm->param_types[k] : NULL;
                                const char *mp = impl_m->function.params[k].ptype;
                                if (tp && mp && strcmp(tp, mp) != 0) {
                                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                                     "method '%s' parameter %d: expected '%s', got '%s'",
                                                     tm->name, k + 1, tp, mp);
                                    s->had_error = true;
                                }
                            }
                        }
                        /* 返回类型(NULL 视为 unit) */
                        const char *tr = tm->ret_type ? tm->ret_type : "unit";
                        const char *mr = impl_m->function.ret_type ? impl_m->function.ret_type : "unit";
                        if (strcmp(tr, mr) != 0) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                             "method '%s' return type: expected '%s', got '%s'",
                                             tm->name, tr, mr);
                            s->had_error = true;
                        }
                    }
                }
            }

            /* 检查方法体（绑定 self） */
            Re0Scope *saved = s->current_scope;
            for (int i = 0; i < stmt->impl.method_count; i++) {
                Re0Stmt *m = stmt->impl.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                s->current_scope = re0_scope_new(s->global_scope);
                Re0Type *self_ty = re0_type_make_named(RE0_TYPE_STRUCT, sn, NULL);
                re0_scope_define(s->current_scope, "self", self_ty, false);
                for (int j = 0; j < m->function.param_count; j++) {
                    if (j == 0 && strcmp(m->function.params[j].name, "self") == 0) continue;
                    Re0Type *pt = resolve_type(s, m->function.params[j].ptype);
                    if (!pt) pt = re0_type_make(RE0_TYPE_I64, NULL);
                    re0_scope_define(s->current_scope, m->function.params[j].name, pt, false);
                }
                s->fn_depth++;
                for (int j = 0; j < m->function.body_count; j++)
                    check_stmt_inner(s, m->function.body[j]);
                s->fn_depth--;
            }
            s->current_scope = saved;
            break;
        }
        case STMT_COMPONENT: {
            /* 注册为 struct */
            char *fnames[32]; char *ftypes[32];
            int sc = stmt->component.state_count;
            sc = sema_cap_count(s, stmt->span, sc, 32, "component state fields");
            for (int i = 0; i < sc; i++) {
                fnames[i] = stmt->component.state[i].name;
                ftypes[i] = stmt->component.state[i].type;
            }
            re0_model_register_struct(s->model, stmt->component.name, fnames, ftypes, sc);
            /* 注册方法到派发表 */
            for (int i = 0; i < stmt->component.method_count; i++) {
                Re0Stmt *m = stmt->component.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                char mangled_buf[256];
                const char *mangled = re0_model_method_symbol(
                    NULL, stmt->component.name, m->function.name, mangled_buf, sizeof(mangled_buf));
                re0_model_register_method(s->model, stmt->component.name,
                                          m->function.name, mangled);
            }
            break;
        }
        default: break;
    }
}

bool re0_sema_check(Re0Sema *s, Re0StmtVec *stmts) {
    for (size_t i = 0; i < Re0StmtVec_len(stmts); i++) {
        Re0Stmt *stmt = stmts->data[i];
        check_stmt_inner(s, stmt);
        Re0StmtVec_push(&s->checked, stmt);
    }
    return !s->had_error;
}

void re0_sema_destroy(Re0Sema *s) {
    Re0StmtVec_free(&s->checked);
    re0_scope_free(s->global_scope);
}
