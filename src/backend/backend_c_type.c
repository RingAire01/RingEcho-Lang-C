#include "backend/backend_c_internal.h"

/* variable/fn-ret/struct-field type tracking + C type inference +
 * type mapping + expression classification + literal writers. */

/* Global backend state (declared in backend_c_internal.h). */
Re0CVarType var_types[MAX_VAR_TYPES];
int var_type_count = 0;
FnRetSlot g_fn_rets[MAX_FN_RETS];
int g_fn_ret_count = 0;
StructFieldSlot g_struct_fields[MAX_STRUCT_FIELDS];
int g_struct_field_count = 0;
LambdaSlot g_lambdas[MAX_LAMBDAS];
int g_lambda_count = 0;
int g_lambda_counter = 0;
size_t g_fwd_insert_pos = 0;
GenericStructSlot g_generic_structs[MAX_GENERIC_STRUCTS];
int g_generic_struct_count = 0;
GenericFnSlot g_generic_fns[MAX_GENERIC_FNS];
int g_generic_fn_count = 0;
InstantiatedSlot g_instantiated[MAX_INSTANTIATED];
int g_instantiated_count = 0;
PendingInst g_pending_list[MAX_INSTANTIATED];
int g_pending_count = 0;

void track_var(const char *name, const char *ctype) {
    if (var_type_count >= MAX_VAR_TYPES) return;
    var_types[var_type_count].name = strdup(name);
    snprintf(var_types[var_type_count].c_type,
             sizeof(var_types[var_type_count].c_type), "%s", ctype);
    var_types[var_type_count].is_float = (strcmp(ctype, "float") == 0 || strcmp(ctype, "double") == 0);
    var_types[var_type_count].is_string = (strcmp(ctype, "const char*") == 0);
    var_type_count++;
}

const char *var_c_type(const char *name) {
    for (int i = var_type_count - 1; i >= 0; i--)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].c_type;
    return NULL;
}

bool var_is_float(const char *name) {
    for (int i = 0; i < var_type_count; i++)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].is_float;
    return false;
}

bool var_is_string(const char *name) {
    for (int i = 0; i < var_type_count; i++)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].is_string;
    return false;
}

/* Check if a builtin function returns string */
bool builtin_returns_string(const char *fn) {
    return strcmp(fn, "str_concat") == 0 || strcmp(fn, "str_slice") == 0 ||
           strcmp(fn, "char_to_str") == 0 || strcmp(fn, "to_string") == 0 ||
           strcmp(fn, "file_read") == 0 || strcmp(fn, "argv_get") == 0 ||
           strcmp(fn, "stdin_read") == 0 ||
           strcmp(fn, "svec_get") == 0 || strcmp(fn, "dir_next") == 0 ||
           strcmp(fn, "path_join") == 0 || strcmp(fn, "path_ext") == 0 ||
           strcmp(fn, "path_base") == 0;
}

/* Check if a builtin function returns float */
bool builtin_returns_float(const char *fn) {
    (void)fn; return false;
}

/* Check if a builtin function returns vec pointer */
bool builtin_returns_vec(const char *fn) {
    return strcmp(fn, "vec_new") == 0;
}

/* Check if a builtin function returns svec pointer */
bool builtin_returns_svec(const char *fn) {
    return strcmp(fn, "svec_new") == 0;
}

void clear_var_types(void) {
    for (int i = 0; i < var_type_count; i++) free(var_types[i].name);
    var_type_count = 0;
}

void track_fn_ret(const char *name, const char *reo_ret) {
    for (int i = 0; i < g_fn_ret_count; i++)
        if (strcmp(g_fn_rets[i].name, name) == 0) return;
    if (g_fn_ret_count >= MAX_FN_RETS) return;
    strncpy(g_fn_rets[g_fn_ret_count].name, name, 127);
    g_fn_rets[g_fn_ret_count].name[127] = '\0';
    strncpy(g_fn_rets[g_fn_ret_count].ret_c_type, reo_type_to_c(reo_ret), 127);
    g_fn_rets[g_fn_ret_count].ret_c_type[127] = '\0';
    g_fn_ret_count++;
}

const char *fn_ret_c_type(const char *name) {
    for (int i = 0; i < g_fn_ret_count; i++)
        if (strcmp(g_fn_rets[i].name, name) == 0)
            return g_fn_rets[i].ret_c_type;
    return NULL;
}

void track_struct_field(const char *struct_name, const char *field, const char *reo_type) {
    for (int i = 0; i < g_struct_field_count; i++)
        if (strcmp(g_struct_fields[i].struct_name, struct_name) == 0 &&
            strcmp(g_struct_fields[i].field, field) == 0) return;
    if (g_struct_field_count >= MAX_STRUCT_FIELDS) return;
    strncpy(g_struct_fields[g_struct_field_count].struct_name, struct_name, 63);
    g_struct_fields[g_struct_field_count].struct_name[63] = '\0';
    strncpy(g_struct_fields[g_struct_field_count].field, field, 63);
    g_struct_fields[g_struct_field_count].field[63] = '\0';
    strncpy(g_struct_fields[g_struct_field_count].c_type, reo_type_to_c(reo_type), 127);
    g_struct_fields[g_struct_field_count].c_type[127] = '\0';
    g_struct_field_count++;
}

const char *struct_field_c_type(const char *struct_name, const char *field) {
    for (int i = 0; i < g_struct_field_count; i++)
        if (strcmp(g_struct_fields[i].struct_name, struct_name) == 0 &&
            strcmp(g_struct_fields[i].field, field) == 0)
            return g_struct_fields[i].c_type;
    return NULL;
}

#define MAX_GENERIC_FNS RE0_MAX_GENERIC_FNS


const char *reo_type_to_c(const char *t) {
    if (!t) return "int64_t";
    /* Compound type annotation (parser may rebuild with spaces):
     * skip leading whitespace then dispatch by first char/prefix,
     * avoiding Vec, [T;N], &T, ptr etc. falling into C as illegal code. */
    {
        const char *p = t;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') return "__reo_arr_t";   /* [T; N] / [T] -> fat-pointer array with bounds checks */
        if (*p == '&') return "int64_t";    /* &T / &mut T: boxed reference */
        if (*p == '*') return "void*";      /* *T ?raw pointer */
        if (strncmp(p, "Vec", 3) == 0) {
            const char *q = p + 3;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '<') return "__reo_vec_t*";   /* Vec<T>: vec runtime pointer */
        }
        if (strchr(p, '<')) return "int64_t";        /* other generic Name<...>: boxed pointer */
    }
    if (strcmp(t, "i8") == 0)    return "int8_t";
    if (strcmp(t, "i16") == 0)   return "int16_t";
    if (strcmp(t, "i32") == 0)   return "int32_t";
    if (strcmp(t, "i64") == 0)   return "int64_t";
    if (strcmp(t, "i128") == 0)  return "__int128";
    if (strcmp(t, "isize") == 0) return "intptr_t";
    if (strcmp(t, "u8") == 0)    return "uint8_t";
    if (strcmp(t, "u16") == 0)   return "uint16_t";
    if (strcmp(t, "u32") == 0)   return "uint32_t";
    if (strcmp(t, "u64") == 0)   return "uint64_t";
    if (strcmp(t, "u128") == 0)  return "unsigned __int128";
    if (strcmp(t, "usize") == 0) return "uintptr_t";
    if (strcmp(t, "f32") == 0)   return "float";
    if (strcmp(t, "f64") == 0)   return "double";
    if (strcmp(t, "bool") == 0)  return "bool";
    if (strcmp(t, "char") == 0)  return "char";
    if (strcmp(t, "str") == 0)   return "const char*";
    if (strcmp(t, "vec") == 0)   return "__reo_vec_t*";
    if (strcmp(t, "ptr") == 0)   return "void*";
    if (strcmp(t, "unit") == 0 || strcmp(t, "void") == 0) return "void";
    /* struct/enum/type alias names: use directly */
    return t;
}

/* Check if an expression is float-typed.
 * Falls back to the full infer_expr_c_type so struct field selects,
 * casts and call results classify correctly too (p.x with x: f32). */
bool expr_is_float(Re0Expr *e) {
    if (!e) return false;
    if (e->kind == EXPR_FLOAT) return true;
    if (e->kind == EXPR_BINARY) return expr_is_float(e->binary.left) || expr_is_float(e->binary.right);
    if (e->kind == EXPR_IDENT) return var_is_float(e->ident.name);
    char t[128];
    if (infer_expr_c_type(e, t, sizeof(t)))
        return strcmp(t, "float") == 0 || strcmp(t, "double") == 0;
    return false;
}

/* Check if an expression is string-typed */
bool expr_is_string(Re0Expr *e) {
    if (!e) return false;
    if (e->kind == EXPR_STRING) return true;
    if (e->kind == EXPR_IDENT) return var_is_string(e->ident.name);
    if (e->kind == EXPR_CALL && e->call.callee &&
        e->call.callee->kind == EXPR_IDENT &&
        builtin_returns_string(e->call.callee->ident.name))
        return true;
    /* string concatenation: str + str yields str */
    if (e->kind == EXPR_BINARY && e->binary.op == BINOP_ADD &&
        (expr_is_string(e->binary.left) || expr_is_string(e->binary.right)))
        return true;
    return false;
}

/* whether an identifier refers to a fat-pointer array variable
 * (for for-in array iteration) */
bool expr_is_array_var(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && (strcmp(t, "__reo_arr_t") == 0 || strcmp(t, "__reo_arrf_t") == 0);
}

/* whether expression is a vec (__reo_vec_t*): used for for-in Vec iteration */
bool expr_is_vec(Re0Expr *e) {
    if (!e) return false;
    if (e->kind == EXPR_IDENT) {
        const char *t = var_c_type(e->ident.name);
        return t && strcmp(t, "__reo_vec_t*") == 0;
    }    if (e->kind == EXPR_CALL && e->call.callee &&
        e->call.callee->kind == EXPR_IDENT &&
        builtin_returns_vec(e->call.callee->ident.name))
        return true;
    return false;
}

/* whether object expression is pointer-typed (e.g. method self): field access must use -> */
bool expr_is_pointer_obj(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && t[0] != '\0' && t[strlen(t) - 1] == '*';
}

bool expr_is_u128(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && strcmp(t, "unsigned __int128") == 0;
}

bool expr_is_i128(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && strcmp(t, "__int128") == 0;
}

/* pick the wider of two integer C type names for mixed-width arithmetic
 * (mirrors sema numeric promotion). returns NULL when neither is a known
 * integer type. */
const char *c_wider_int_type(const char *a, const char *b) {
    static const char *order[] = {
        "int8_t", "uint8_t", "int16_t", "uint16_t",
        "int32_t", "uint32_t", "int64_t", "uint64_t",
        "__int128", "unsigned __int128"
    };
    const int n = (int)(sizeof(order) / sizeof(order[0]));
    int ia = -1, ib = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(a, order[i]) == 0) ia = i;
        if (strcmp(b, order[i]) == 0) ib = i;
    }
    if (ia < 0 || ib < 0) return NULL;
    /* widen to the larger rank; on equal rank prefer unsigned */
    if (ia == ib) return order[ia];
    int rank_a = ia / 2, rank_b = ib / 2;
    if (rank_a > rank_b) return order[ia];
    if (rank_b > rank_a) return order[ib];
    return (ia > ib) ? order[ia] : order[ib];
}

bool infer_expr_c_type(Re0Expr *e, char *type, size_t type_size) {
    if (!e || !type || type_size == 0) return false;
    const char *known = NULL;
    switch (e->kind) {
        case EXPR_INT: known = "int64_t"; break;
        case EXPR_FLOAT: known = "double"; break;
        case EXPR_BOOL: known = "bool"; break;
        case EXPR_CHAR: known = "char"; break;
        case EXPR_STRING: known = "const char*"; break;
        case EXPR_IDENT: known = var_c_type(e->ident.name); break;
        case EXPR_STRUCT_INIT:
            if (find_generic_struct(e->struct_init.name) &&
                e->struct_init.field_count > 0) {
                char inferred[256];
                if (infer_expr_c_type(e->struct_init.fields[0].value, inferred, sizeof(inferred))) {
                    snprintf(type, type_size, "%s_%s", e->struct_init.name, inferred);
                    type[type_size - 1] = '\0';
                    return true;
                }
            }
            known = e->struct_init.name;
            break;
        case EXPR_LAMBDA: known = "__reo_fn_ptr"; break;
        case EXPR_UNARY:
            if (e->unary.op == UNOP_NOT) known = "bool";
            else if (!infer_expr_c_type(e->unary.operand, type, type_size)) return false;
            else if (e->unary.op == UNOP_REF || e->unary.op == UNOP_REFMUT) {
                size_t len = strlen(type);
                if (len + 1 >= type_size) return false;
                type[len] = '*';
                type[len + 1] = '\0';
                return true;
            } else if (e->unary.op == UNOP_DEREF) {
                size_t len = strlen(type);
                while (len > 0 && type[len - 1] == ' ') len--;
                if (len == 0 || type[len - 1] != '*') return false;
                type[--len] = '\0';
                return true;
            } else {
                return true;
            }
            break;
        case EXPR_CALL:
            if (e->call.callee && e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                if (builtin_returns_string(fn)) known = "const char*";
                else if (builtin_returns_vec(fn)) known = "__reo_vec_t*";
                else if (builtin_returns_svec(fn)) known = "__reo_svec_t*";
                else if (builtin_returns_float(fn)) known = "double";
                else {
                    /* look up user function return type */
                    const char *ret = fn_ret_c_type(fn);
                    if (ret) known = ret;
                }
            }
            /* enum constructor */
            if (!known && e->call.callee && e->call.callee->kind == EXPR_IDENT &&
                strchr(e->call.callee->ident.name, ':')) {
                char ename[128];
                const char *colon = strchr(e->call.callee->ident.name, ':');
                size_t elen = (size_t)(colon - e->call.callee->ident.name);
                if (elen < sizeof(ename)) {
                    memcpy(ename, e->call.callee->ident.name, elen);
                    ename[elen] = '\0';
                    known = ename;
                }
            }
            break;
        case EXPR_ARRAY:
        case EXPR_ARRAY_REPEAT: {
            /* fat-pointer arrays: element type selects __reo_arr_t/__reo_arrf_t */
            if (e->kind == EXPR_ARRAY && e->array.count > 0) {
                char et[128];
                if (infer_expr_c_type(e->array.elems[0], et, sizeof(et)) &&
                    (strcmp(et, "float") == 0 || strcmp(et, "double") == 0))
                    { known = "__reo_arrf_t"; break; }
            } else if (e->kind == EXPR_ARRAY_REPEAT && e->array_repeat.value) {
                char et[128];
                if (infer_expr_c_type(e->array_repeat.value, et, sizeof(et)) &&
                    (strcmp(et, "float") == 0 || strcmp(et, "double") == 0))
                    { known = "__reo_arrf_t"; break; }
            }
            known = "__reo_arr_t";
            break;
        }
        case EXPR_INDEX: {
            /* element access: follow the collection's element type */
            char base[128];
            if (infer_expr_c_type(e->index.target, base, sizeof(base))) {
                if (strcmp(base, "__reo_arrf_t") == 0) { known = "double"; break; }
                if (strcmp(base, "__reo_arr_t") == 0) { known = "int64_t"; break; }
            }
            known = "int64_t";
            break;
        }
        case EXPR_CAST:
            known = reo_type_to_c(e->cast.target_type);
            break;
        case EXPR_SELECT: {
            /* a.field: infer a's struct type, then look up field's C type */
            char obj_type[128];
            if (infer_expr_c_type(e->select.object, obj_type, sizeof(obj_type))) {
                const char *ft = struct_field_c_type(obj_type, e->select.field);
                if (ft) known = ft;
            }
            break;
        }
        case EXPR_BINARY: {
            /* string concatenation yields const char*; numeric arithmetic
             * must mirror sema promotion (int + float -> double), otherwise
             * `let c = 7 + 2.0` declares int64_t and truncates the result. */
            if (e->binary.op == BINOP_ADD &&
                (expr_is_string(e->binary.left) || expr_is_string(e->binary.right))) {
                known = "const char*";
                break;
            }
            if (e->binary.op != BINOP_ADD && e->binary.op != BINOP_SUB &&
                e->binary.op != BINOP_MUL && e->binary.op != BINOP_DIV &&
                e->binary.op != BINOP_MOD)
                break; /* comparisons/logic yield bool via caller default */
            char lt[128], rt[128];
            bool has_l = infer_expr_c_type(e->binary.left, lt, sizeof(lt));
            bool has_r = infer_expr_c_type(e->binary.right, rt, sizeof(rt));
            /* untyped integer literals inherit the other side's type
             * (Rust-style literal flexibility): `i8 a + 1` stays i8.
             * infer_expr_c_type types literals as int64_t, so detect
             * them structurally here. */
            bool l_lit = e->binary.left && e->binary.left->kind == EXPR_INT;
            bool r_lit = e->binary.right && e->binary.right->kind == EXPR_INT;
            if (l_lit && r_lit) { known = "int64_t"; break; }
            if (l_lit && has_r) { known = rt; break; }
            if (r_lit && has_l) { known = lt; break; }
            if (has_l && has_r) {
                bool lf = strcmp(lt, "double") == 0 || strcmp(lt, "float") == 0;
                bool rf = strcmp(rt, "double") == 0 || strcmp(rt, "float") == 0;
                if (lf || rf) { known = "double"; break; }
                /* both integer: keep the wider type */
                known = c_wider_int_type(lt, rt);
                if (known) break;
                known = lt; /* fallback: left operand type */
            } else if (has_l) {
                known = lt;
            }
            break;
        }
        default: break;
    }
    if (!known) return false;
    int written = snprintf(type, type_size, "%s", known);
    return written >= 0 && (size_t)written < type_size;
}

const char *binop_c(Re0BinOpKind op) {
    switch (op) {
        case BINOP_ADD: return "+"; case BINOP_SUB: return "-"; case BINOP_MUL: return "*";
        case BINOP_DIV: return "/"; case BINOP_MOD: return "%%";
        case BINOP_EQ: return "=="; case BINOP_NE: return "!=";
        case BINOP_LT: return "<"; case BINOP_LE: return "<=";
        case BINOP_GT: return ">"; case BINOP_GE: return ">=";
        case BINOP_AND: return "&&"; case BINOP_OR: return "||";
        case BINOP_BAND: return "&"; case BINOP_BOR: return "|"; case BINOP_BXOR: return "^";
        case BINOP_SHL: return "<<"; case BINOP_SHR: return ">>";
        default: return "?";
    }
}

void c_write_string_literal(Re0Buffer *b, const char *value) {
    re0_buffer_write_char(b, '"');
    if (value) {
        const unsigned char *p = (const unsigned char*)value;
        while (*p) {
            switch (*p) {
                case '\\': re0_buffer_write_str(b, "\\\\"); break;
                case '"': re0_buffer_write_str(b, "\\\""); break;
                case '\n': re0_buffer_write_str(b, "\\n"); break;
                case '\r': re0_buffer_write_str(b, "\\r"); break;
                case '\t': re0_buffer_write_str(b, "\\t"); break;
                default:
                    if (*p < 0x20 || *p == 0x7f)
                        re0_buffer_write_fmt(b, "\\%03o", (unsigned int)*p);
                    else
                        re0_buffer_write_char(b, (char)*p);
                    break;
            }
            p++;
        }
    }
    re0_buffer_write_char(b, '"');
}

bool split_qualified(const char *name, char *enum_name, int elen,
                            char *variant_name, int vlen) {
    const char *colon = strstr(name, "::");
    if (!colon) return false;
    int n = (int)(colon - name);
    if (n >= elen) n = elen - 1;
    memcpy(enum_name, name, (size_t)n);
    enum_name[n] = '\0';
    const char *v = colon + 2;
    int vn = (int)strlen(v);
    if (vn >= vlen) vn = vlen - 1;
    memcpy(variant_name, v, (size_t)vn);
    variant_name[vn] = '\0';
    return true;
}

/* safely emit C char literal, escaping special characters correctly */
void c_write_char_literal(Re0Buffer *b, char c) {
    re0_buffer_write_str(b, "'");
    switch (c) {
        case '\n': re0_buffer_write_str(b, "\\n"); break;
        case '\t': re0_buffer_write_str(b, "\\t"); break;
        case '\r': re0_buffer_write_str(b, "\\r"); break;
        case '\\': re0_buffer_write_str(b, "\\\\"); break;
        case '\'': re0_buffer_write_str(b, "\\'"); break;
        case '\0': re0_buffer_write_str(b, "\\0"); break;
        default:
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f)
                re0_buffer_write_fmt(b, "%c", c);
            else
                re0_buffer_write_fmt(b, "\\x%02x", (unsigned char)c);
            break;
    }
    re0_buffer_write_str(b, "'");
}

