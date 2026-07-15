#include "sema.h"
#include <stdlib.h>
#include <string.h>

/* 解析类型名：先查标准类型，再查类型别名 */
static Re0Type *resolve_type(Re0Sema *s, const char *name) {
    if (!name) return NULL;
    Re0Type *t = re0_model_std_type(name);
    if (t) return t;
    const char *resolved = re0_model_resolve_type_alias(s->model, name);
    if (resolved) return re0_model_std_type(resolved);
    return NULL;
}

void re0_sema_init(Re0Sema *s, Re0Arena *arena, Re0ErrorList *errors,
                   Re0SemanticModel *model, Re0BuiltinRegistry *builtins) {
    s->arena = arena; s->errors = errors; s->model = model; s->builtins = builtins;
    s->global_scope = re0_scope_new(NULL); s->current_scope = s->global_scope;
    Re0StmtVec_init(&s->checked); s->had_error = false;

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

static Re0Type *infer_type(Re0Sema *s, Re0Expr *e) {
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
        case EXPR_BINARY: return infer_type(s, e->binary.left);
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
                    if (mangled)
                        return re0_type_make(RE0_TYPE_I64, NULL);
                }
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
                    if (re0_model_find_enum(s->model, enum_name)) {
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
                /* check if it's a user-defined function */
                Re0Symbol *sym = re0_scope_lookup(s->global_scope, e->call.callee->ident.name);
                if (sym && sym->is_function && sym->type && sym->type->kind == RE0_TYPE_FN) {
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
                    return sym->type->func.ret;
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
            return t;
        }
        case EXPR_MATCH:
            if (e->match_.arm_count > 0) return infer_type(s, e->match_.arms[0].body);
            return re0_type_make(RE0_TYPE_UNIT, NULL);
        case EXPR_SELECT: {
            /* field access on struct variable */
            Re0Type *obj_type = infer_type(s, e->select.object);
            if (obj_type && obj_type->kind == RE0_TYPE_UNKNOWN) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                "cannot access field '%s' on unknown type", e->select.field);
                s->had_error = true;
            }
            return re0_type_make(RE0_TYPE_I64, NULL);
        }
        case EXPR_INDEX:
            return re0_type_make(RE0_TYPE_I64, NULL);
        case EXPR_TRY: {
            /* expr? — 检查内部类型，返回 payload 类型（MVP: i64） */
            if (e->try_.inner) infer_type(s, e->try_.inner);
            return re0_type_make(RE0_TYPE_I64, NULL);
        }
        case EXPR_LAMBDA:
            /* lambda: 开子作用域绑定参数，推断返回类型 */
            return re0_type_make(RE0_TYPE_I64, NULL);
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
            Re0Type *type = NULL;
            if (stmt->let_stmt.type) type = resolve_type(s, stmt->let_stmt.type);
            if (stmt->let_stmt.init) type = type ? type : infer_type(s, stmt->let_stmt.init);
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
            if (stmt->assign.value) infer_type(s, stmt->assign.value);
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
            for (int i = 0; i < stmt->while_stmt.body_count; i++)
                check_stmt_inner(s, stmt->while_stmt.body[i]);
            s->current_scope = saved;
            break;
        }
        case STMT_FOR: {
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_scope_new(s->current_scope);
            re0_scope_define(s->current_scope, stmt->for_stmt.var,
                           re0_type_make(RE0_TYPE_I64, NULL), true);
            infer_type(s, stmt->for_stmt.iter);
            for (int i = 0; i < stmt->for_stmt.body_count; i++)
                check_stmt_inner(s, stmt->for_stmt.body[i]);
            s->current_scope = saved;
            break;
        }
        case STMT_RETURN:
            if (stmt->return_stmt.value) infer_type(s, stmt->return_stmt.value);
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
            if (param_count > 64) param_count = 64;
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
            for (int i = 0; i < stmt->function.body_count; i++)
                check_stmt_inner(s, stmt->function.body[i]);
            s->current_scope = saved;
            break;
        }
        case STMT_STRUCT: {
            char *field_names[64], *field_types[64];
            int n = stmt->struct_decl.field_count;
            if (n > 64) n = 64;
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
            if (n > 64) n = 64;
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
                if (param_count > 64) param_count = 64;
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
            Re0TraitMethod *tms = (Re0TraitMethod*)calloc(
                (size_t)(stmt->trait_decl.method_count > 0 ? stmt->trait_decl.method_count : 1),
                sizeof(Re0TraitMethod));
            for (int i = 0; i < stmt->trait_decl.method_count; i++) {
                Re0TraitMethodDecl *d = &stmt->trait_decl.methods[i];
                tms[i].name = d->mname;
                tms[i].param_count = d->param_count;
                tms[i].ret_type = d->ret_type;
                if (d->param_count > 0) {
                    tms[i].param_types = (char**)calloc((size_t)d->param_count, sizeof(char*));
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
                const char *mangled = re0_model_method_symbol(tn, sn, m->function.name);
                re0_model_register_method(s->model, sn, m->function.name, mangled);
                if (!re0_scope_lookup_local(s->global_scope, mangled)) {
                    Re0Type *params[64];
                    int pc = m->function.param_count;
                    if (pc > 64) pc = 64;
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

            /* trait 方法完整性检查 */
            if (tn) {
                Re0TraitDef *td = re0_model_find_trait(s->model, tn);
                if (td) {
                    for (int i = 0; i < td->method_count; i++) {
                        bool found = false;
                        for (int j = 0; j < stmt->impl.method_count; j++) {
                            Re0Stmt *m = stmt->impl.methods[j];
                            if (m && m->kind == STMT_FUNCTION &&
                                strcmp(m->function.name, td->methods[i].name) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                             "missing method '%s' in impl of '%s' for '%s'",
                                             td->methods[i].name, tn, sn);
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
                for (int j = 0; j < m->function.body_count; j++)
                    check_stmt_inner(s, m->function.body[j]);
            }
            s->current_scope = saved;
            break;
        }
        case STMT_COMPONENT: {
            /* 注册为 struct */
            char *fnames[32]; char *ftypes[32];
            int sc = stmt->component.state_count;
            if (sc > 32) sc = 32;
            for (int i = 0; i < sc; i++) {
                fnames[i] = stmt->component.state[i].name;
                ftypes[i] = stmt->component.state[i].type;
            }
            re0_model_register_struct(s->model, stmt->component.name, fnames, ftypes, sc);
            /* 注册方法到派发表 */
            for (int i = 0; i < stmt->component.method_count; i++) {
                Re0Stmt *m = stmt->component.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                const char *mangled = re0_model_method_symbol(
                    NULL, stmt->component.name, m->function.name);
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
