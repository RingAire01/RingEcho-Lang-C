#include "base/safe.h"
#include "analysis/sema.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include "base/re0_limits.h"

/* Resolve type name: standard type -> type alias -> named struct/enum
 * (prevents named types from collapsing to UNIT/UNKNOWN) */
static Re0Type *resolve_type(Re0Sema *s, const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "ConversionError") == 0) name = "i64";
    Re0Type *t = re0_sema_own_type(s, re0_model_std_type(name));
    if (t && !re0_model_resolve_type_alias(s->model, name)) return t;
    const char *resolved = name;
    size_t steps = 0;
    const char *next;
    while ((next = re0_model_resolve_type_alias(s->model, resolved)) != NULL) {
        if (++steps > s->model->type_aliases.len) {
            re0_error_append(s->errors, RE0_ERR_SEMANTIC, RE0_SPAN_ZERO, NULL, "cyclic type alias: %s", name);
            s->had_error = true;
            return NULL;
        }
        resolved = next;
    }
    if (resolved == name) resolved = NULL;
    if (resolved) {
        Re0Type *rt = re0_sema_own_type(s, re0_model_std_type(resolved));
        if (rt) return rt;
        name = resolved; /* if the alias points to a named type, keep resolving as named */
    }
    if (re0_model_find_struct(s->model, name))
        return re0_sema_own_type(s, re0_type_make_named(RE0_TYPE_STRUCT, name, NULL));
    if (re0_model_find_enum(s->model, name))
        return re0_sema_own_type(s, re0_type_make_named(RE0_TYPE_ENUM, name, NULL));
    return NULL;
}

/* assignability (from -> to): real type checking.
 * numeric widens; UNKNOWN/TYPEVAR/NEVER/UNIT targets are lenient;
 * everything else must be structurally equal. */
static bool sema_assignable(Re0Type *from, Re0Type *to) {
    if (!from || !to) return true;
    if (from->kind == RE0_TYPE_UNKNOWN || to->kind == RE0_TYPE_UNKNOWN) return true;
    if (from->kind == RE0_TYPE_TYPEVAR || to->kind == RE0_TYPE_TYPEVAR) return true;
    if (from->kind == RE0_TYPE_NEVER) return true;
    if (to->kind == RE0_TYPE_UNIT) return true;
    return re0_type_coercible(from, to); /* equal or numeric conversion */
}

static bool sema_assignable_expr(Re0Type *from, Re0Type *to, Re0Expr *expr) {
    if (expr && to && expr->kind == EXPR_INT && !expr->int_lit.suffix &&
        re0_type_is_integer(to->kind))
        return re0_integer_fits(expr->int_lit.integer, (unsigned)re0_type_sizeof(to->kind) * 8,
                               re0_type_is_signed(to->kind));
    if (expr && to && expr->kind == EXPR_FLOAT && !expr->float_lit.suffix &&
        to->kind == RE0_TYPE_F32) {
        double value = expr->float_lit.val;
        return value <= FLT_MAX && value >= -FLT_MAX && (double)(float)value == value;
    }
    return sema_assignable(from, to);
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
    s->supports_conversions = true;
    s->current_fn_return = NULL;
    s->loop_depth = 0; s->fn_depth = 0;
    s->child_scopes.data = NULL; s->child_scopes.len = 0; s->child_scopes.cap = 0;
    s->owned_types.data = NULL; s->owned_types.len = 0; s->owned_types.cap = 0;

    /* pre-inject Option/Result core enums */
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

    /* Track builtin type objects. Builtin ret_type/param types are heap-
     * owned (re0_model_std_type -> re0_type_parse) and are shared with the
     * sema pass: infer_type returns them directly to scope symbols. Register
     * them here so re0_sema_destroy releases each shared object exactly once
     * via the deduplicating owned_types list. */
    for (size_t i = 0; i < Re0BuiltinVec_len(&builtins->fns); i++) {
        Re0BuiltinFn *bf = &builtins->fns.data[i];
        re0_sema_own_type(s, bf->ret_type);
        for (int j = 0; j < bf->param_count; j++)
            re0_sema_own_type(s, bf->params[j].type);
    }
}

/* open a child scope owned by the sema instance; re0_sema_destroy frees
 * every scope opened this way (symbol strdups included). Returns NULL on
 * allocation failure, in which case the caller keeps the parent scope. */
Re0Scope *re0_sema_open_scope(Re0Sema *s, Re0Scope *parent) {
    Re0Scope *sc = re0_scope_new(parent);
    if (!sc) return NULL;
    if (s->child_scopes.len >= s->child_scopes.cap) {
        size_t nc = s->child_scopes.cap ? s->child_scopes.cap * 2 : 16;
        Re0Scope **nd = (Re0Scope**)realloc(s->child_scopes.data,
                                            nc * sizeof(Re0Scope*));
        if (!nd) { re0_scope_free(sc); return NULL; }
        s->child_scopes.data = nd;
        s->child_scopes.cap = nc;
    }
    s->child_scopes.data[s->child_scopes.len++] = sc;
    return sc;
}

/* Track a heap-owned (owned==true) type object for bulk release at
 * destroy time. Deduplicates by pointer. Also registers directly-referenced
 * child type objects (func.ret, func.params[i], array/slice/vec/reference
 * inner, tuple.elems[i], generic.args[i]) so they are released exactly once
 * without recursive re0_type_free. */
Re0Type *re0_sema_own_type(Re0Sema *s, Re0Type *t) {
    if (!s || !t || !t->owned) return t;
    /* Dedup: the same object may be shared across call sites (e.g. a
     * builtin's ret_type). */
    for (size_t i = 0; i < s->owned_types.len; i++)
        if (s->owned_types.data[i] == t) return t;
    if (s->owned_types.len >= s->owned_types.cap) {
        size_t nc = s->owned_types.cap ? s->owned_types.cap * 2 : 64;
        Re0Type **nd = (Re0Type**)realloc(s->owned_types.data,
                                           nc * sizeof(Re0Type*));
        if (!nd) return t; /* OOM: skip tracking, leak rather than crash */
        s->owned_types.data = nd;
        s->owned_types.cap = nc;
    }
    s->owned_types.data[s->owned_types.len++] = t;

    /* Register directly-referenced children so re0_type_free (which frees
     * only a node's own heap fields, never recursing) still releases them. */
    switch (t->kind) {
        case RE0_TYPE_ARRAY:
            re0_sema_own_type(s, t->array.inner);
            break;
        case RE0_TYPE_SLICE:
            re0_sema_own_type(s, t->slice.inner);
            break;
        case RE0_TYPE_VEC:
            re0_sema_own_type(s, t->vec.inner);
            break;
        case RE0_TYPE_REFERENCE:
            re0_sema_own_type(s, t->ref_.inner);
            break;
        case RE0_TYPE_TUPLE:
            for (int i = 0; i < t->tuple.count; i++)
                re0_sema_own_type(s, t->tuple.elems[i]);
            break;
        case RE0_TYPE_FN:
            re0_sema_own_type(s, t->func.ret);
            for (int i = 0; i < t->func.param_count; i++)
                re0_sema_own_type(s, t->func.params[i]);
            break;
        case RE0_TYPE_GENERIC:
            for (int i = 0; i < t->generic.arg_count; i++)
                re0_sema_own_type(s, t->generic.args[i]);
            break;
        default:
            break;
    }
    return t;
}

static Re0Type *infer_type(Re0Sema *s, Re0Expr *e);
static Re0Type *infer_type_impl(Re0Sema *s, Re0Expr *e);

static Re0Type *infer_type(Re0Sema *s, Re0Expr *e) {
    if (!e) return re0_sema_own_type(s, re0_type_make(RE0_TYPE_UNIT, NULL));
    if (s->infer_depth > RE0_MAX_SEMA_DEPTH) {
        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL, "expression type nesting exceeds limit");
        s->had_error = true;
        return re0_sema_own_type(s, re0_type_make(RE0_TYPE_UNKNOWN, NULL));
    }
    s->infer_depth++;
    Re0Type *t = infer_type_impl(s, e);
    s->infer_depth--;
    e->resolved_type = re0_sema_own_type(s, t);
    return e->resolved_type;
}

static Re0Type *infer_type_impl(Re0Sema *s, Re0Expr *e) {
    if (!e) return re0_type_make(RE0_TYPE_UNIT, NULL);
    switch (e->kind) {
        case EXPR_INT: {
            if (e->int_lit.suffix) {
                Re0Type *st = re0_type_parse(e->int_lit.suffix);
                if (st && re0_type_is_integer(st->kind)) {
                    if (!re0_integer_fits(e->int_lit.integer, (unsigned)re0_type_sizeof(st->kind) * 8,
                                          re0_type_is_signed(st->kind))) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "integer literal is outside the range of %s", e->int_lit.suffix);
                        s->had_error = true;
                    }
                    return st;
                }
                re0_type_free(st);
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL, "invalid integer suffix");
                s->had_error = true;
            }
            if (!re0_integer_fits(e->int_lit.integer, 64, true))
                return re0_type_make(re0_integer_fits(e->int_lit.integer, 128, true)
                                     ? RE0_TYPE_I128 : RE0_TYPE_U128, NULL);
            return re0_type_make(RE0_TYPE_I64, NULL);
        }
        case EXPR_FLOAT: {
            if (e->float_lit.suffix) {
                Re0Type *st = re0_type_parse(e->float_lit.suffix);
                if (st && re0_type_is_float(st->kind)) return st;
                if (strcmp(e->float_lit.suffix, "f") == 0) {
                    re0_type_free(st);
                    return re0_type_make(RE0_TYPE_F64, NULL);
                }
                re0_type_free(st);
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL, "invalid floating suffix");
                s->had_error = true;
            }
            return re0_type_make(RE0_TYPE_F64, NULL);
        }
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
                    /* arithmetic/bitwise: numeric promotion.
                     * int + float must promote to float (C semantics),
                     * otherwise `let c = 7 + 2.0` infers int64 and
                     * truncates the result. Untyped integer literals
                     * inherit the other side's type (Rust-style), so
                     * `i8 a + 1` stays i8. */
                    {
                        bool l_lit = e->binary.left && e->binary.left->kind == EXPR_INT;
                        bool r_lit = e->binary.right && e->binary.right->kind == EXPR_INT;
                        if (l_lit && r_lit && lt && rt)
                            return re0_type_sizeof(lt->kind) >= re0_type_sizeof(rt->kind) ? lt : rt;
                        if (l_lit && rt) return rt;
                        if (r_lit && lt) return lt;
                    }
                    if (lt && rt &&
                        re0_type_is_numeric(lt->kind) &&
                        re0_type_is_numeric(rt->kind)) {
                        if (re0_type_is_float(lt->kind)) return lt;
                        if (re0_type_is_float(rt->kind)) return rt;
                        /* both integer: prefer the wider of the two */
                        if (re0_type_sizeof(lt->kind) >= re0_type_sizeof(rt->kind))
                            return lt;
                        return rt;
                    }
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
                case UNOP_BNOT:
                    if (!re0_type_is_integer(operand->kind) && operand->kind != RE0_TYPE_UNKNOWN) {
                        re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                         "cannot apply bitwise not to type '%s'",
                                         re0_type_kind_name(operand->kind));
                        s->had_error = true;
                    }
                    return operand;
            }
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
        }
        case EXPR_CALL: {
            /* Validate every argument before any specialized call returns. */
            for (int ai = 0; ai < e->call.arg_count; ai++)
                infer_type(s, e->call.args[ai]);
            /* method call: obj.method(args) -> callee is EXPR_SELECT */
            if (e->call.callee->kind == EXPR_SELECT) {
                Re0Expr *sel = e->call.callee;
                Re0Type *obj_ty = infer_type(s, sel->select.object);
                if (obj_ty && obj_ty->kind == RE0_TYPE_STRUCT && obj_ty->named.name) {
                    const char *mangled = re0_model_lookup_method(
                        s->model, obj_ty->named.name, sel->select.field);
                    if (mangled) {
                        /* look up the mangled symbol's FN type to get the real return type */
                        Re0Symbol *msym = re0_scope_lookup(s->global_scope, mangled);
                        if (msym && msym->type && msym->type->kind == RE0_TYPE_FN &&
                            msym->type->func.ret)
                            return msym->type->func.ret;
                    }
                }
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            /* enum constructor: Enum::Variant(args) */
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
                        /* variant name follows "::"; validate variant
                         * existence and payload arity */
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
            /* spawn/await builtin functions */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                if (strcmp(fn, "__reo_spawn") == 0 || strcmp(fn, "__reo_await") == 0)
                    return re0_type_make(RE0_TYPE_I64, NULL);
            }
            if (e->call.callee->kind == EXPR_IDENT) {
                Re0BuiltinFn *bf = re0_builtin_lookup(s->builtins, e->call.callee->ident.name);
                if (bf) return bf->ret_type;
                /* user function / local lambda: resolve via FN type
                 * (current_scope chain reaches global) */
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
                    /* B4: per-argument type checking (lenient for TYPEVAR/UNKNOWN) */
                    for (int ai = 0; ai < e->call.arg_count &&
                                    ai < sym->type->func.param_count; ai++) {
                        Re0Type *at = infer_type(s, e->call.args[ai]);
                        Re0Type *pt = sym->type->func.params[ai];
                        if (at && pt && !sema_assignable_expr(at, pt, e->call.args[ai])) {
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
        case EXPR_IF: {
            infer_type(s, e->if_expr.cond);
            Re0Type *then_type = infer_type(s, e->if_expr.then);
            if (!e->if_expr.else_) return re0_type_make(RE0_TYPE_UNIT, NULL);
            infer_type(s, e->if_expr.else_);
            return then_type;
        }
        case EXPR_BLOCK: {
            Re0Type *last = NULL;
            for (int i = 0; i < e->block.count; i++) last = infer_type(s, e->block.stmts[i]);
            return last ? last : re0_type_make(RE0_TYPE_UNIT, NULL);
        }
        case EXPR_STRUCT_INIT: {
            Re0Type *t = re0_type_make(RE0_TYPE_STRUCT, NULL);
            t->named.name = strdup(e->struct_init.name);
            /* field validation (known non-generic structs only):
             * extra fields / missing fields / type mismatches */
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
                            ft->kind != RE0_TYPE_UNKNOWN && !sema_assignable_expr(vt, ft, val)) {
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
        case EXPR_MATCH: {
            infer_type(s, e->match_.scrutinee);
            Re0Type *first = NULL;
            for (int i = 0; i < e->match_.arm_count; i++) {
                Re0Type *arm_type = infer_type(s, e->match_.arms[i].body);
                if (!first) first = arm_type;
            }
            return first ? first : re0_type_make(RE0_TYPE_UNIT, NULL);
        }
        case EXPR_SELECT: {
            /* field access: return the real field type (concrete types
             * only; generic/unknown falls back to UNKNOWN, stay lenient) */
            Re0Type *obj_type = infer_type(s, e->select.object);
            if (obj_type && obj_type->kind == RE0_TYPE_GENERIC &&
                strcmp(obj_type->generic.name, "Result") == 0 && obj_type->generic.arg_count == 2) {
                if (strcmp(e->select.field, "value") == 0) return obj_type->generic.args[0];
                if (strcmp(e->select.field, "error") == 0 || strcmp(e->select.field, "tag") == 0 ||
                    strcmp(e->select.field, "index") == 0)
                    return re0_type_make(RE0_TYPE_I64, NULL);
            }
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
            /* indexing: infer element type from the collection type */
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
            /* array literal: infer element-wise, take the first
             * concrete type as the element type */
            if (e->array.count == 0)
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            Re0Type *elem = NULL;
            for (int i = 0; i < e->array.count; i++) {
                Re0Type *t = infer_type(s, e->array.elems[i]);
                if (!elem && t && t->kind != RE0_TYPE_UNKNOWN) elem = t;
            }
            if (!elem) elem = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            return re0_type_make_array(elem, (size_t)e->array.count, NULL);
        }
        case EXPR_ARRAY_REPEAT: {
            /* [value; count]: fixed-length array if count is an
             * integer literal, otherwise a slice */
            Re0Type *elem = infer_type(s, e->array_repeat.value);
            infer_type(s, e->array_repeat.count);
            if (!elem) elem = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            if (e->array_repeat.count && e->array_repeat.count->kind == EXPR_INT &&
                e->array_repeat.count->int_lit.val >= 0)
                return re0_type_make_array(elem,
                            (size_t)e->array_repeat.count->int_lit.val, NULL);
            return re0_type_make_slice(elem, NULL);
        }
        case EXPR_TUPLE: {
            Re0Type *elems[64];
            int n = sema_cap_count(s, e->span, e->tuple.count, 64, "tuple elements");
            for (int i = 0; i < n; i++) {
                Re0Type *t = infer_type(s, e->tuple.elems[i]);
                elems[i] = t ? t : re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            return re0_type_make_tuple(elems, n, NULL);
        }
        case EXPR_TRY: {
            /* expr? -- the '?' operator unwraps Option/Result only.
             * Anything else produces invalid C (a stmt-expr returning the
             * whole enum into a scalar slot) and must be rejected here. */
            Re0Type *inner = e->try_.inner ? infer_type(s, e->try_.inner) : NULL;
            if (inner && inner->kind == RE0_TYPE_GENERIC &&
                strcmp(inner->generic.name, "Result") == 0 && inner->generic.arg_count == 2) {
                if (!s->current_fn_return || s->current_fn_return->kind != RE0_TYPE_GENERIC ||
                    strcmp(s->current_fn_return->generic.name, "Result") != 0 ||
                    s->current_fn_return->generic.arg_count != 2 ||
                    !re0_type_equal(inner->generic.args[1], s->current_fn_return->generic.args[1])) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                     "checked conversion '?' requires a matching Result return type");
                    s->had_error = true;
                }
                e->try_.return_type = s->current_fn_return;
                return inner->generic.args[0];
            }
            if (inner && inner->kind != RE0_TYPE_ENUM &&
                inner->kind != RE0_TYPE_UNKNOWN &&
                !(inner->kind == RE0_TYPE_STRUCT &&
                  inner->named.name &&
                  (strcmp(inner->named.name, "Option") == 0 ||
                   strcmp(inner->named.name, "Result") == 0))) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                 "cannot apply '?' to non-Option/Result type '%s'",
                                 re0_type_kind_name(inner->kind));
                s->had_error = true;
            }
            return re0_type_make(RE0_TYPE_I64, NULL);
        }
        case EXPR_LAMBDA: {
            /* lambda: open a child scope to bind params, infer Fn(params, ret) */
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_sema_open_scope(s, s->current_scope);
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
        case EXPR_CAST: {
            /* Scalar conversions share the same legality matrix; checked
             * conversions preserve failures in a typed Result payload. */
            Re0Type *src = e->cast.inner ? infer_type(s, e->cast.inner) : NULL;
            if (!s->supports_conversions) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                 "target backend does not support this conversion; use the C backend");
                s->had_error = true;
                return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
            }
            if (e->cast.target_type) {
                Re0Type *target = resolve_type(s, e->cast.target_type);
                if (!target) target = re0_sema_own_type(s, re0_type_parse(e->cast.target_type));
                if (target && target->kind <= RE0_TYPE_PTR)
                    e->cast.target_type = re0_arena_strdup(s->arena, re0_type_kind_name(target->kind));
                if (!target) return re0_type_make(RE0_TYPE_UNKNOWN, NULL);

                Re0TypeKind sk = src ? src->kind : RE0_TYPE_UNKNOWN;
                Re0TypeKind dk = target->kind;
                bool s_num = re0_type_is_numeric(sk);
                bool d_num = re0_type_is_numeric(dk);
                bool s_int_like = s_num || sk == RE0_TYPE_BOOL || sk == RE0_TYPE_CHAR;
                bool d_int_like = d_num || dk == RE0_TYPE_BOOL || dk == RE0_TYPE_CHAR;

                bool legal = re0_type_equal(src, target)
                          || (s_num && d_num)                        /* numeric <-> numeric */
                          || (s_int_like && d_int_like)              /* bool/char <-> integer */
                          || (sk == RE0_TYPE_STR && d_int_like)           /* str -> numeric */
                          || (s_int_like && dk == RE0_TYPE_STR);          /* numeric -> str */
                if (e->cast.checked && src && src->kind == RE0_TYPE_ARRAY && target->kind == RE0_TYPE_ARRAY &&
                    src->array.inner && target->array.inner && src->array.inner->kind < RE0_TYPE_STR && target->array.inner->kind < RE0_TYPE_STR)
                    legal = true;
                if (src && !legal) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                                     "invalid cast from '%s' to '%s'",
                                     re0_type_kind_name(sk), re0_type_kind_name(dk));
                    s->had_error = true;
                }
                if (e->cast.checked) {
                    Re0Type *args[] = {target, re0_type_make(RE0_TYPE_I64, NULL)};
                    return re0_type_make_generic("Result", args, 2, NULL);
                }
                return target;
            }
            return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
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
            /* B1: annotation vs initializer type checking */
            if (anno && init_ty && !sema_assignable_expr(init_ty, anno, stmt->let_stmt.init)) {
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
                /* B2: assignment type checking */
                if (sym && sym->type && vt && !sema_assignable_expr(vt, sym->type, stmt->assign.value)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                    "assignment type mismatch: '%s' is '%s', got '%s'",
                                    stmt->assign.name, re0_type_kind_name(sym->type->kind),
                                    re0_type_kind_name(vt->kind));
                    s->had_error = true;
                }
            }
            break;
        }
        case STMT_FIELD_ASSIGN: {
            Re0Expr selection = {0};
            selection.kind = EXPR_SELECT;
            selection.span = stmt->span;
            selection.select.object = stmt->field_assign.obj;
            selection.select.field = stmt->field_assign.field;
            Re0Type *target = infer_type(s, &selection);
            Re0Type *value = infer_type(s, stmt->field_assign.value);
            if (!sema_assignable_expr(value, target, stmt->field_assign.value)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL, "field assignment type mismatch; use an explicit conversion");
                s->had_error = true;
            }
            break;
        }
        case STMT_INDEX_ASSIGN: {
            Re0Expr index = {0};
            index.kind = EXPR_INDEX;
            index.span = stmt->span;
            index.index.target = stmt->index_assign.target;
            index.index.index = stmt->index_assign.index;
            Re0Type *target = infer_type(s, &index);
            Re0Type *value = infer_type(s, stmt->index_assign.value);
            if (!sema_assignable_expr(value, target, stmt->index_assign.value)) {
                re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL, "array assignment type mismatch; use an explicit conversion");
                s->had_error = true;
            }
            break;
        }
        case STMT_EXPR:
            if (stmt->expr_stmt.expr) infer_type(s, stmt->expr_stmt.expr);
            break;
        case STMT_IF: {
            Re0Scope *saved = s->current_scope;
            s->current_scope = re0_sema_open_scope(s, s->current_scope);
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
            s->current_scope = re0_sema_open_scope(s, s->current_scope);
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
            s->current_scope = re0_sema_open_scope(s, s->current_scope);
            /* infer loop variable type from the iterator: range->i64,
             * Vec/Array/Slice->element, str->char, others->i64 */
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
                /* B3: return type checking (against the current function return type) */
                if (s->current_fn_return && vt &&
                    !sema_assignable_expr(vt, s->current_fn_return, stmt->return_stmt.value)) {
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
            Re0Scope *saved = s->current_scope;
            Re0Symbol *sym = re0_scope_lookup_local(s->global_scope, stmt->function.name);
            Re0Type *ret = NULL;
            if (sym && sym->type && sym->type->kind == RE0_TYPE_FN) {
                ret = sym->type->func.ret;
            } else {
                Re0Type *params[64];
                int param_count = stmt->function.param_count;
                param_count = sema_cap_count(s, stmt->span, param_count, 64, "parameters");
                for (int i = 0; i < param_count; i++) {
                    params[i] = resolve_type(s, stmt->function.params[i].ptype);
                    if (!params[i]) params[i] = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                }
                ret = resolve_type(s, stmt->function.ret_type);
                if (!ret) ret = re0_type_make(RE0_TYPE_UNIT, NULL);
                Re0Type *ft = re0_type_make_func(params, param_count, ret, false, NULL);
                if (!ft) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                     "cannot allocate signature for function '%s'",
                                     stmt->function.name);
                    s->had_error = true;
                    break;
                }
                re0_scope_define(s->global_scope, stmt->function.name, ft, false);
                sym = re0_scope_lookup_local(s->global_scope, stmt->function.name);
                if (sym) sym->is_function = true;
                re0_model_register_fn(s->model, stmt->function.name,
                                      NULL, param_count,
                                      stmt->function.ret_type,
                                      stmt->function.type_params,
                                      stmt->function.type_param_count);
            }

            s->current_scope = re0_sema_open_scope(s, s->global_scope);
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
            if (!re0_model_find_struct(s->model, stmt->struct_decl.name)) {
                char *field_names[64], *field_types[64];
                int n = stmt->struct_decl.field_count;
                n = sema_cap_count(s, stmt->span, n, 64, "struct fields");
                for (int i = 0; i < n; i++) {
                    field_names[i] = stmt->struct_decl.fields[i].name;
                    field_types[i] = stmt->struct_decl.fields[i].type;
                }
                re0_model_register_struct(s->model, stmt->struct_decl.name,
                                         field_names, field_types, n);
                Re0StructDef *sd = re0_model_find_struct(s->model, stmt->struct_decl.name);
                if (sd) {
                    for (int j = 0; j < sd->field_count; j++)
                        re0_sema_own_type(s, sd->fields[j].type);
                }
            }
            break;
        }
        case STMT_ENUM: {
            if (!re0_model_find_enum(s->model, stmt->enum_decl.name)) {
                char *vnames[64]; int has_payload[64];
                int n = stmt->enum_decl.variant_count;
                n = sema_cap_count(s, stmt->span, n, 64, "enum variants");
                for (int i = 0; i < n; i++) {
                    vnames[i] = stmt->enum_decl.variants[i].vname;
                    has_payload[i] = stmt->enum_decl.variants[i].type_count > 0 ? 1 : 0;
                }
                re0_model_register_enum(s->model, stmt->enum_decl.name,
                                       vnames, has_payload, n);
            }
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
            if (stmt->const_decl.value) {
                Re0Type *value = infer_type(s, stmt->const_decl.value);
                if (type && !sema_assignable_expr(value, type, stmt->const_decl.value)) {
                    re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL, "constant type mismatch; use an explicit conversion");
                    s->had_error = true;
                }
                if (!type) type = value;
            }
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

            /* register methods into the dispatch table + global scope */
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

            /* trait method completeness + signature checking */
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
                        /* parameter count */
                        if (impl_m->function.param_count != tm->param_count) {
                            re0_error_append(s->errors, RE0_ERR_SEMANTIC, stmt->span, NULL,
                                             "method '%s' of trait '%s': expected %d parameter(s), got %d",
                                             tm->name, tn, tm->param_count, impl_m->function.param_count);
                            s->had_error = true;
                        } else {
                            /* per-parameter types (params without explicit
                             * types like self are NULL on both sides,
                             * auto-skipped) */
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
                        /* return type (NULL is treated as unit) */
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

            /* check method bodies (bind self) */
            Re0Scope *saved = s->current_scope;
            for (int i = 0; i < stmt->impl.method_count; i++) {
                Re0Stmt *m = stmt->impl.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                s->current_scope = re0_sema_open_scope(s, s->global_scope);
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
            /* register as struct */
            char *fnames[32]; char *ftypes[32];
            int sc = stmt->component.state_count;
            sc = sema_cap_count(s, stmt->span, sc, 32, "component state fields");
            for (int i = 0; i < sc; i++) {
                fnames[i] = stmt->component.state[i].name;
                ftypes[i] = stmt->component.state[i].type;
            }
            re0_model_register_struct(s->model, stmt->component.name, fnames, ftypes, sc);
            /* Track heap-owned field types, mirroring STMT_STRUCT above. */
            {
                Re0StructDef *sd = re0_model_find_struct(s->model, stmt->component.name);
                if (sd) {
                    for (int j = 0; j < sd->field_count; j++)
                        re0_sema_own_type(s, sd->fields[j].type);
                }
            }
            /* register methods into the dispatch table */
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

/* Track every owned type object reachable from a scope's symbols.
 * Child scopes plus the global scope hold types produced by check_stmt_inner
 * (function signatures, impl method signatures, extern decls) that did not
 * flow through infer_type/resolve_type, so they were never registered. */
static void sema_track_scope_types(Re0Sema *s, Re0Scope *sc) {
    for (Re0Scope *cur = sc; cur; cur = cur->parent) {
        for (size_t i = 0; i < Re0SymbolVec_len(&cur->symbols); i++)
            re0_sema_own_type(s, cur->symbols.data[i].type);
    }
}

static void register_global_declarations(Re0Sema *s, Re0StmtVec *stmts) {
    for (size_t i = 0; i < Re0StmtVec_len(stmts); i++) {
        Re0Stmt *stmt = stmts->data[i];
        if (!stmt) continue;
        if (stmt->kind == STMT_STRUCT) {
            char *field_names[64], *field_types[64];
            int n = stmt->struct_decl.field_count;
            n = sema_cap_count(s, stmt->span, n, 64, "struct fields");
            for (int j = 0; j < n; j++) {
                field_names[j] = stmt->struct_decl.fields[j].name;
                field_types[j] = stmt->struct_decl.fields[j].type;
            }
            re0_model_register_struct(s->model, stmt->struct_decl.name,
                                     field_names, field_types, n);
            Re0StructDef *sd = re0_model_find_struct(s->model, stmt->struct_decl.name);
            if (sd) {
                for (int j = 0; j < sd->field_count; j++)
                    re0_sema_own_type(s, sd->fields[j].type);
            }
        } else if (stmt->kind == STMT_ENUM) {
            char *vnames[64]; int has_payload[64];
            int n = stmt->enum_decl.variant_count;
            n = sema_cap_count(s, stmt->span, n, 64, "enum variants");
            for (int j = 0; j < n; j++) {
                vnames[j] = stmt->enum_decl.variants[j].vname;
                has_payload[j] = stmt->enum_decl.variants[j].type_count > 0 ? 1 : 0;
            }
            re0_model_register_enum(s->model, stmt->enum_decl.name,
                                   vnames, has_payload, n);
        } else if (stmt->kind == STMT_FUNCTION) {
            if (!re0_scope_lookup_local(s->global_scope, stmt->function.name)) {
                Re0Type *params[64];
                int param_count = stmt->function.param_count;
                param_count = sema_cap_count(s, stmt->span, param_count, 64, "parameters");
                for (int j = 0; j < param_count; j++) {
                    params[j] = resolve_type(s, stmt->function.params[j].ptype);
                    if (!params[j]) params[j] = re0_type_make(RE0_TYPE_UNKNOWN, NULL);
                }
                Re0Type *ret = resolve_type(s, stmt->function.ret_type);
                if (!ret) ret = re0_type_make(RE0_TYPE_UNIT, NULL);
                Re0Type *ft = re0_type_make_func(params, param_count, ret, false, NULL);
                if (ft) {
                    re0_scope_define(s->global_scope, stmt->function.name, ft, false);
                    Re0Symbol *sym = re0_scope_lookup_local(s->global_scope, stmt->function.name);
                    if (sym) sym->is_function = true;
                }
                re0_model_register_fn(s->model, stmt->function.name,
                                      NULL, param_count,
                                      stmt->function.ret_type,
                                      stmt->function.type_params,
                                      stmt->function.type_param_count);
            }
        }
    }
}

bool re0_sema_check(Re0Sema *s, Re0StmtVec *stmts) {
    register_global_declarations(s, stmts);
    for (size_t i = 0; i < Re0StmtVec_len(stmts); i++) {
        Re0Stmt *stmt = stmts->data[i];
        check_stmt_inner(s, stmt);
        Re0StmtVec_push(&s->checked, stmt);
    }
    /* Register owned types held directly by scopes (function/impl/extern
     * signatures bypass infer_type/resolve_type). child_scopes own their
     * symbol names, but their type pointers are shared and released here. */
    sema_track_scope_types(s, s->global_scope);
    for (size_t i = 0; i < s->child_scopes.len; i++)
        sema_track_scope_types(s, s->child_scopes.data[i]);
    return !s->had_error;
}

void re0_sema_destroy(Re0Sema *s) {
    if (!s) return;
    Re0StmtVec_free(&s->checked);
    /* free all tracked child scopes before the global scope (they may
     * still point at it as parent, but only names are heap-owned here) */
    for (size_t i = 0; i < s->child_scopes.len; i++)
        re0_scope_free(s->child_scopes.data[i]);
    free(s->child_scopes.data);
    s->child_scopes.data = NULL;
    s->child_scopes.len = 0; s->child_scopes.cap = 0;
    /* Free all heap-owned type objects (deduplicated list, no double-free). */
    for (size_t i = 0; i < s->owned_types.len; i++)
        re0_type_free(s->owned_types.data[i]);
    free(s->owned_types.data);
    s->owned_types.data = NULL;
    s->owned_types.len = 0; s->owned_types.cap = 0;
    re0_scope_free(s->global_scope);
}
