#include "base/safe.h"
#include "backend/backend.h"
#include "base/re0_limits.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ── variable type tracking for println format selection ── */
#define MAX_VAR_TYPES RE0_MAX_VAR_TYPES
typedef struct {
    char *name;
    char c_type[128];
    bool is_float;
    bool is_string;
} Re0CVarType;

static Re0CVarType var_types[MAX_VAR_TYPES];
static int var_type_count = 0;

static void track_var(const char *name, const char *ctype) {
    if (var_type_count >= MAX_VAR_TYPES) return;
    var_types[var_type_count].name = strdup(name);
    snprintf(var_types[var_type_count].c_type,
             sizeof(var_types[var_type_count].c_type), "%s", ctype);
    var_types[var_type_count].is_float = (strcmp(ctype, "float") == 0 || strcmp(ctype, "double") == 0);
    var_types[var_type_count].is_string = (strcmp(ctype, "const char*") == 0);
    var_type_count++;
}

static const char *var_c_type(const char *name) {
    for (int i = var_type_count - 1; i >= 0; i--)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].c_type;
    return NULL;
}

static bool var_is_float(const char *name) {
    for (int i = 0; i < var_type_count; i++)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].is_float;
    return false;
}

static bool var_is_string(const char *name) {
    for (int i = 0; i < var_type_count; i++)
        if (strcmp(var_types[i].name, name) == 0) return var_types[i].is_string;
    return false;
}

/* Check if a builtin function returns string */
static bool builtin_returns_string(const char *fn) {
    return strcmp(fn, "str_concat") == 0 || strcmp(fn, "str_slice") == 0 ||
           strcmp(fn, "char_to_str") == 0 || strcmp(fn, "to_string") == 0 ||
           strcmp(fn, "file_read") == 0 || strcmp(fn, "argv_get") == 0 ||
           strcmp(fn, "stdin_read") == 0 ||
           strcmp(fn, "svec_get") == 0 || strcmp(fn, "dir_next") == 0 ||
           strcmp(fn, "path_join") == 0 || strcmp(fn, "path_ext") == 0 ||
           strcmp(fn, "path_base") == 0;
}

/* Check if a builtin function returns float */
static bool builtin_returns_float(const char *fn) {
    (void)fn; return false;
}

/* Check if a builtin function returns vec pointer */
static bool builtin_returns_vec(const char *fn) {
    return strcmp(fn, "vec_new") == 0;
}

/* Check if a builtin function returns svec pointer */
static bool builtin_returns_svec(const char *fn) {
    return strcmp(fn, "svec_new") == 0;
}

static void clear_var_types(void) {
    for (int i = 0; i < var_type_count; i++) free(var_types[i].name);
    var_type_count = 0;
}

/* ════════════════════════════════════════════════════
 *  generic function monomorphization infrastructure
 * ════════════════════════════════════════════════════ */

/* forward declarations */
static bool infer_expr_c_type(Re0Expr *e, char *type, size_t type_size);
static const char *reo_type_to_c(const char *t);
static int c_gen_expr(Re0Codegen *c, Re0Expr *e);

/* function return type tracking (used by infer_expr_c_type) */
#define MAX_FN_RETS RE0_MAX_FN_RETS
typedef struct { char name[128]; char ret_c_type[128]; } FnRetSlot;
static FnRetSlot g_fn_rets[MAX_FN_RETS];
static int g_fn_ret_count = 0;

/* struct field type tracking (used by infer_expr_c_type to infer a.field type) */
#define MAX_STRUCT_FIELDS 512
typedef struct { char struct_name[64]; char field[64]; char c_type[128]; } StructFieldSlot;
static StructFieldSlot g_struct_fields[MAX_STRUCT_FIELDS];
static int g_struct_field_count = 0;

/* lambda storage and counting */
#define MAX_LAMBDAS RE0_MAX_LAMBDAS
typedef struct { char name[64]; Re0Expr *lambda; } LambdaSlot;
static LambdaSlot g_lambdas[MAX_LAMBDAS];
static int g_lambda_count = 0;
static int g_lambda_counter = 0;

/* generic struct storage */
#define MAX_GENERIC_STRUCTS RE0_MAX_GENERIC_STRUCTS
#define MAX_INSTANTIATED RE0_MAX_INSTANTIATED
static size_t g_fwd_insert_pos = 0;  /* length of prelude after c_begin finishes */
typedef struct { const char *name; Re0Stmt *def; } GenericStructSlot;
static GenericStructSlot g_generic_structs[MAX_GENERIC_STRUCTS];
static int g_generic_struct_count = 0;

static void register_generic_struct(const char *name, Re0Stmt *def) {
    if (g_generic_struct_count >= MAX_GENERIC_STRUCTS) return;
    for (int i = 0; i < g_generic_struct_count; i++)
        if (strcmp(g_generic_structs[i].name, name) == 0) return;
    g_generic_structs[g_generic_struct_count].name = name;
    g_generic_structs[g_generic_struct_count].def = def;
    g_generic_struct_count++;
}

static Re0Stmt *find_generic_struct(const char *name) {
    for (int i = 0; i < g_generic_struct_count; i++)
        if (strcmp(g_generic_structs[i].name, name) == 0) return g_generic_structs[i].def;
    return NULL;
}

/* set of instantiated struct mangled names */
static char g_struct_instances[MAX_INSTANTIATED][256];
static int g_struct_instance_count = 0;

static bool struct_already_instantiated(const char *mangled) {
    for (int i = 0; i < g_struct_instance_count; i++)
        if (strcmp(g_struct_instances[i], mangled) == 0) return true;
    return false;
}

/* instantiate generic struct: record pending (typedef generated in c_end) */
static const char *instantiate_generic_struct(Re0Codegen *c, const char *base_name,
                                               const char *type_arg,
                                               char *out, size_t out_sz) {
    (void)c;
    Re0Stmt *def = find_generic_struct(base_name);
    if (!def) { snprintf(out, out_sz, "%s", base_name); return out; }

    snprintf(out, out_sz, "%s_%s", base_name, type_arg);
    if (struct_already_instantiated(out)) return out;
    if (g_struct_instance_count >= MAX_INSTANTIATED) return out;
    snprintf(g_struct_instances[g_struct_instance_count++], sizeof(g_struct_instances[0]), "%s", out);
    return out;
}

/* called in c_end: generate all generic struct typedefs */
static void flush_generic_structs(Re0Codegen *c) {
    if (g_struct_instance_count == 0) return;
    (void)c;

    /* build all typedefs and insert after the prelude */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int i = 0; i < g_struct_instance_count; i++) {
        const char *mangled = g_struct_instances[i];
        /* extract base_name: Pair_int64_t → Pair */
        char base_name[128];
        strncpy(base_name, mangled, sizeof(base_name) - 1);
        base_name[sizeof(base_name)-1] = '\0';
        char *last_under = NULL; (void)last_under;
        char *p = base_name;
        while (*p) { if (*p == '_') last_under = p; p++; }
        /* find the last matching generic struct within base_name */
        (void)0;
        for (int j = 0; j < g_generic_struct_count; j++) {
            size_t nlen = strlen(g_generic_structs[j].name);
            if (nlen < strlen(mangled) &&
                strncmp(mangled, g_generic_structs[j].name, nlen) == 0 &&
                mangled[nlen] == '_') {
                strncpy(base_name, g_generic_structs[j].name, sizeof(base_name)-1);
                base_name[sizeof(base_name)-1] = '\0';
                break;
            }
        }
        Re0Stmt *def = find_generic_struct(base_name);
        if (!def) continue;
        const char *type_arg = mangled + strlen(base_name) + 1;
        char **type_params = def->struct_decl.type_params;

        re0_buffer_write_str(&decls, "typedef struct { ");
        for (int j = 0; j < def->struct_decl.field_count; j++) {
            const char *ft = def->struct_decl.fields[j].type;
            if (type_params && type_params[0] && strcmp(ft, type_params[0]) == 0)
                ft = type_arg;
            re0_buffer_write_fmt(&decls, "%s %s; ", reo_type_to_c(ft),
                                 def->struct_decl.fields[j].name);
        }
        re0_buffer_write_fmt(&decls, "} %s;\n", mangled);
    }

    if (decls.len > 0 && g_fwd_insert_pos <= c->output.len) {
        size_t tail_len = c->output.len - g_fwd_insert_pos;
        char *tail = (char*)xmalloc(tail_len > 0 ? tail_len : 1);
        if (tail) {
            memcpy(tail, c->output.data + g_fwd_insert_pos, tail_len);
            c->output.len = g_fwd_insert_pos;
            re0_buffer_write_n(&c->output, decls.data, decls.len);
            re0_buffer_write_n(&c->output, tail, tail_len);
            free(tail);
        }
    }
    re0_buffer_free(&decls);
    g_struct_instance_count = 0;
}

/* detect generics at StructInit and instantiate, return mangled name */
static const char *try_instantiate_generic_struct_init(Re0Codegen *c, Re0Expr *e,
                                                        char *out, size_t out_sz) {
    if (e->kind != EXPR_STRUCT_INIT) return NULL;
    const char *name = e->struct_init.name;
    if (!find_generic_struct(name)) return NULL;

    /* infer type from the first field value */
    if (e->struct_init.field_count > 0) {
        char inferred[128];
        if (infer_expr_c_type(e->struct_init.fields[0].value, inferred, sizeof(inferred)))
            return instantiate_generic_struct(c, name, inferred, out, out_sz);
    }
    return NULL;
}

static void track_fn_ret(const char *name, const char *reo_ret) {
    for (int i = 0; i < g_fn_ret_count; i++)
        if (strcmp(g_fn_rets[i].name, name) == 0) return;
    if (g_fn_ret_count >= MAX_FN_RETS) return;
    strncpy(g_fn_rets[g_fn_ret_count].name, name, 127);
    g_fn_rets[g_fn_ret_count].name[127] = '\0';
    strncpy(g_fn_rets[g_fn_ret_count].ret_c_type, reo_type_to_c(reo_ret), 127);
    g_fn_rets[g_fn_ret_count].ret_c_type[127] = '\0';
    g_fn_ret_count++;
}

static const char *fn_ret_c_type(const char *name) {
    for (int i = 0; i < g_fn_ret_count; i++)
        if (strcmp(g_fn_rets[i].name, name) == 0)
            return g_fn_rets[i].ret_c_type;
    return NULL;
}

static void track_struct_field(const char *struct_name, const char *field, const char *reo_type) {
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

static const char *struct_field_c_type(const char *struct_name, const char *field) {
    for (int i = 0; i < g_struct_field_count; i++)
        if (strcmp(g_struct_fields[i].struct_name, struct_name) == 0 &&
            strcmp(g_struct_fields[i].field, field) == 0)
            return g_struct_fields[i].c_type;
    return NULL;
}

#define MAX_GENERIC_FNS RE0_MAX_GENERIC_FNS

typedef struct { const char *name; Re0Stmt *def; } GenericFnSlot;
static GenericFnSlot g_generic_fns[MAX_GENERIC_FNS];
static int g_generic_fn_count = 0;

typedef struct { char name[256]; } InstantiatedSlot;
static InstantiatedSlot g_instantiated[MAX_INSTANTIATED];
static int g_instantiated_count = 0;

/* pending instantiation entry: full function body generated later in c_end */
typedef struct {
    Re0Stmt *def;
    char type_args[8][64];
    int type_arg_count;
    char mangled[256];
} PendingInst;
static PendingInst g_pending_list[MAX_INSTANTIATED];
static int g_pending_count = 0;

static void register_generic_fn(const char *name, Re0Stmt *def) {
    if (g_generic_fn_count >= MAX_GENERIC_FNS) return;
    for (int i = 0; i < g_generic_fn_count; i++)
        if (strcmp(g_generic_fns[i].name, name) == 0) return;
    g_generic_fns[g_generic_fn_count].name = name;
    g_generic_fns[g_generic_fn_count].def = def;
    g_generic_fn_count++;
}

static Re0Stmt *find_generic_fn(const char *name) {
    for (int i = 0; i < g_generic_fn_count; i++)
        if (strcmp(g_generic_fns[i].name, name) == 0) return g_generic_fns[i].def;
    return NULL;
}

static bool is_already_instantiated(const char *mangled) {
    for (int i = 0; i < g_instantiated_count; i++)
        if (strcmp(g_instantiated[i].name, mangled) == 0) return true;
    return false;
}

static void mark_instantiated(const char *mangled) {
    if (g_instantiated_count >= MAX_INSTANTIATED) return;
    snprintf(g_instantiated[g_instantiated_count].name, sizeof(g_instantiated[0].name), "%s", mangled);
    g_instantiated_count++;
}

/* type substitution: look up orig in type_params, return args[i] if found */
static const char *substitute_one(const char *orig,
                                   char **params, char **args, int n) {
    if (!orig) return NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(orig, params[i]) == 0) return args[i];
    return orig;
}

/* forward declaration */
static void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth);

/* instantiate generic function: record pending + emit forward declaration */
static void instantiate_generic_fn(Re0Codegen *c, Re0Stmt *def,
                                    char **type_args, int type_arg_count) {
    (void)c;
    char **type_params = def->function.type_params; (void)type_params;
    int tp_count = def->function.type_param_count;
    if (tp_count == 0 || type_arg_count == 0) return;

    /* compute mangled name */
    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s", def->function.name);
    for (int i = 0; i < type_arg_count && i < tp_count; i++) {
        strncat(mangled, "_", sizeof(mangled) - strlen(mangled) - 1);
        strncat(mangled, type_args[i], sizeof(mangled) - strlen(mangled) - 1);
    }
    if (is_already_instantiated(mangled)) return;
    mark_instantiated(mangled);

    /* record pending (for c_end to emit forward declaration + full body) */
    if (g_pending_count < MAX_INSTANTIATED) {
        PendingInst *pi = &g_pending_list[g_pending_count++];
        pi->def = def;
        pi->type_arg_count = type_arg_count < 8 ? type_arg_count : 8;
        for (int i = 0; i < pi->type_arg_count; i++)
            snprintf(pi->type_args[i], sizeof(pi->type_args[i]), "%s", type_args[i]);
        snprintf(pi->mangled, sizeof(pi->mangled), "%s", mangled);
    }
}

/* called in c_end: forward declarations + generate all pending generic function bodies */
static void flush_pending_instantiations(Re0Codegen *c) {
    if (g_pending_count == 0) return;

    /* 1. build forward declaration text */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int idx = 0; idx < g_pending_count; idx++) {
        PendingInst *pi = &g_pending_list[idx];
        Re0Stmt *def = pi->def;
        char **type_params = def->function.type_params; (void)type_params;
        int tp_count = def->function.type_param_count;
        char *type_args[8];
        for (int i = 0; i < pi->type_arg_count; i++) type_args[i] = pi->type_args[i];
        int sub_count = pi->type_arg_count < tp_count ? pi->type_arg_count : tp_count;

        const char *sub_ret = substitute_one(def->function.ret_type,
                                              type_params, type_args, sub_count);
        re0_buffer_write_fmt(&decls, "%s %s(", reo_type_to_c(sub_ret), pi->mangled);
        for (int i = 0; i < def->function.param_count; i++) {
            if (i > 0) re0_buffer_write_str(&decls, ", ");
            const char *pt = substitute_one(def->function.params[i].ptype,
                                             type_params, type_args, sub_count);
            re0_buffer_write_fmt(&decls, "%s", reo_type_to_c(pt));
        }
        re0_buffer_write_str(&decls, ");\n");
    }

    /* 2. insert forward declarations after the prelude, before user code */
    if (decls.len > 0 && g_fwd_insert_pos <= c->output.len) {
        size_t tail_len = c->output.len - g_fwd_insert_pos;
        char *tail = (char*)xmalloc(tail_len > 0 ? tail_len : 1);
        if (tail) {
            memcpy(tail, c->output.data + g_fwd_insert_pos, tail_len);
            c->output.len = g_fwd_insert_pos;
            re0_buffer_write_n(&c->output, decls.data, decls.len);
            re0_buffer_write_n(&c->output, tail, tail_len);
            free(tail);
        }
    }
    re0_buffer_free(&decls);

    /* 3. generate full function bodies */
    for (int idx = 0; idx < g_pending_count; idx++) {
        PendingInst *pi = &g_pending_list[idx];
        Re0Stmt *def = pi->def;
        char **type_params = def->function.type_params; (void)type_params;
        int tp_count = def->function.type_param_count;
        char *type_args[8];
        for (int i = 0; i < pi->type_arg_count; i++) type_args[i] = pi->type_args[i];
        int sub_count = pi->type_arg_count < tp_count ? pi->type_arg_count : tp_count;

        char *saved_ptypes[64];
        char *saved_ret = def->function.ret_type;
        char *saved_name = def->function.name;
        int saved_tp = def->function.type_param_count;
        int pc = def->function.param_count;
        if (pc > 64) pc = 64;
        for (int i = 0; i < pc; i++) saved_ptypes[i] = def->function.params[i].ptype;

        for (int i = 0; i < pc; i++)
            def->function.params[i].ptype = (char*)substitute_one(
                saved_ptypes[i], type_params, type_args, sub_count);
        def->function.ret_type = (char*)substitute_one(
            saved_ret, type_params, type_args, sub_count);
        def->function.name = pi->mangled;
        def->function.type_param_count = 0;

        c_gen_stmt(c, def, 0);

        for (int i = 0; i < pc; i++) def->function.params[i].ptype = saved_ptypes[i];
        def->function.ret_type = saved_ret;
        def->function.name = saved_name;
        def->function.type_param_count = saved_tp;
    }
    g_pending_count = 0;
}

/* called in c_end: forward declarations + generate all lambda function bodies */
static void flush_lambdas(Re0Codegen *c) {
    if (g_lambda_count == 0) return;
    Re0Buffer *b = &c->output;

    /* 1. build forward declarations and insert after the prelude */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int i = 0; i < g_lambda_count; i++) {
        Re0Expr *lam = g_lambdas[i].lambda;
        re0_buffer_write_fmt(&decls, "int64_t %s(int64_t", g_lambdas[i].name);
        for (int j = 0; j < lam->lambda.param_count; j++)
            re0_buffer_write_str(&decls, ", int64_t");
        re0_buffer_write_str(&decls, ");\n");
    }
    if (decls.len > 0 && g_fwd_insert_pos <= c->output.len) {
        size_t tail_len = c->output.len - g_fwd_insert_pos;
        char *tail = (char*)xmalloc(tail_len > 0 ? tail_len : 1);
        if (tail) {
            memcpy(tail, c->output.data + g_fwd_insert_pos, tail_len);
            c->output.len = g_fwd_insert_pos;
            re0_buffer_write_n(&c->output, decls.data, decls.len);
            re0_buffer_write_n(&c->output, tail, tail_len);
            free(tail);
        }
    }
    re0_buffer_free(&decls);

    /* 2. generate function bodies */
    for (int i = 0; i < g_lambda_count; i++) {
        Re0Expr *lam = g_lambdas[i].lambda;
        re0_buffer_write_fmt(b, "int64_t %s(int64_t __env", g_lambdas[i].name);
        for (int j = 0; j < lam->lambda.param_count; j++)
            re0_buffer_write_fmt(b, ", int64_t %s", lam->lambda.params[j].name);
        re0_buffer_write_str(b, ") {\n    return ");
        c_gen_expr(c, lam->lambda.body);
        re0_buffer_write_str(b, ";\n}\n\n");
    }
    g_lambda_count = 0;
}

/* detect generic call at CALL and trigger instantiation, return mangled name (NULL=not generic) */
static const char *try_instantiate_generic_call(Re0Codegen *c, const char *fn_name,
                                                 Re0Expr **args, int arg_count,
                                                 char *out, size_t out_sz) {
    Re0Stmt *def = find_generic_fn(fn_name);
    if (!def) return NULL;

    /* infer type from the first argument (MVP: single type param, use first arg's type) */
    if (def->function.type_param_count == 1 && arg_count > 0) {
        char inferred_type[128];
        if (!infer_expr_c_type(args[0], inferred_type, sizeof(inferred_type)))
            return NULL;
        char *type_args[1] = { inferred_type };
        instantiate_generic_fn(c, def, type_args, 1);

        snprintf(out, out_sz, "%s_%s", fn_name, inferred_type);
        return out;
    }
    return NULL;
}

/* ════════════════════════════════════════════════════ */

/* ── RingEcho type name → C type string ── */
static const char *reo_type_to_c(const char *t) {
    if (!t) return "int64_t";
    /* Compound type annotation (parser may rebuild with spaces):
     * skip leading whitespace then dispatch by first char/prefix,
     * avoiding Vec, [T;N], &T, ptr etc. falling into C as illegal code. */
    {
        const char *p = t;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') return "__reo_arr_t";   /* [T; N] / [T] -> fat-pointer array with bounds checks */
        if (*p == '&') return "int64_t";    /* &T / &mut T → boxed reference */
        if (*p == '*') return "void*";      /* *T → raw pointer */
        if (strncmp(p, "Vec", 3) == 0) {
            const char *q = p + 3;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '<') return "__reo_vec_t*";   /* Vec<T> → vec runtime pointer */
        }
        if (strchr(p, '<')) return "int64_t";        /* other generic Name<...> → boxed pointer */
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
static bool expr_is_float(Re0Expr *e) {
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
static bool expr_is_string(Re0Expr *e) {
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
static bool expr_is_array_var(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && (strcmp(t, "__reo_arr_t") == 0 || strcmp(t, "__reo_arrf_t") == 0);
}

/* whether expression is a vec (__reo_vec_t*): used for for-in Vec iteration */
static bool expr_is_vec(Re0Expr *e) {
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
static bool expr_is_pointer_obj(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && t[0] != '\0' && t[strlen(t) - 1] == '*';
}

static bool expr_is_u128(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && strcmp(t, "unsigned __int128") == 0;
}

static bool expr_is_i128(Re0Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return false;
    const char *t = var_c_type(e->ident.name);
    return t && strcmp(t, "__int128") == 0;
}

/* pick the wider of two integer C type names for mixed-width arithmetic
 * (mirrors sema numeric promotion). returns NULL when neither is a known
 * integer type. */
static const char *c_wider_int_type(const char *a, const char *b) {
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

static bool infer_expr_c_type(Re0Expr *e, char *type, size_t type_size) {
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

static const char *binop_c(Re0BinOpKind op) {
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

static void c_write_string_literal(Re0Buffer *b, const char *value) {
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

static bool split_qualified(const char *name, char *enum_name, int elen,
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
static void c_write_char_literal(Re0Buffer *b, char c) {
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

static int c_gen_expr(Re0Codegen *c, Re0Expr *e) {
    Re0Buffer *b = &c->output;
    if (!e) { re0_buffer_write_str(b, "(void)0"); return 0; }
    switch (e->kind) {
        case EXPR_INT: re0_buffer_write_fmt(b, "%lldLL", (long long)e->int_lit.val); break;
        case EXPR_FLOAT: re0_buffer_write_fmt(b, "%g", e->float_lit.val); break;
        case EXPR_BOOL: re0_buffer_write_str(b, e->bool_lit.val ? "1" : "0"); break;
        case EXPR_CHAR: c_write_char_literal(b, e->char_lit.val); break;
        case EXPR_STRING: c_write_string_literal(b, e->str_lit.val); break;
        case EXPR_IDENT: {
            char ename[128], vname[128];
            if (split_qualified(e->ident.name, ename, sizeof(ename), vname, sizeof(vname))) {
                Re0EnumDef *def = re0_model_find_enum(c->model, ename);
                if (def) {
                    int tag = re0_model_variant_tag(def, vname);
                    re0_buffer_write_fmt(b, "(%s){ .tag = %d }", ename, tag);
                    break;
                }
            }
            re0_buffer_write_str(b, e->ident.name);
            break;
        }
        case EXPR_UNIT: re0_buffer_write_str(b, "0"); break;
        case EXPR_BINARY: {
            Re0BinOpKind op = e->binary.op;
            /* constant folding: compute at compile time when both sides are integer literals */
            if (e->binary.left->kind == EXPR_INT && e->binary.right->kind == EXPR_INT) {
                int64_t l = e->binary.left->int_lit.val;
                int64_t r = e->binary.right->int_lit.val;
                int64_t folded;
                bool can_fold = true;
                switch (op) {
                    case BINOP_ADD: folded = l + r; break;
                    case BINOP_SUB: folded = l - r; break;
                    case BINOP_MUL: folded = l * r; break;
                    case BINOP_DIV: if (r == 0) { can_fold = false; break; } folded = l / r; break;
                    case BINOP_MOD: if (r == 0) { can_fold = false; break; } folded = l % r; break;
                    case BINOP_EQ: folded = l == r; break;
                    case BINOP_NE: folded = l != r; break;
                    case BINOP_LT: folded = l < r; break;
                    case BINOP_LE: folded = l <= r; break;
                    case BINOP_GT: folded = l > r; break;
                    case BINOP_GE: folded = l >= r; break;
                    case BINOP_AND: folded = l && r; break;
                    case BINOP_OR: folded = l || r; break;
                    case BINOP_BAND: folded = l & r; break;
                    case BINOP_BOR: folded = l | r; break;
                    case BINOP_BXOR: folded = l ^ r; break;
                    case BINOP_SHL: folded = l << r; break;
                    case BINOP_SHR: folded = l >> r; break;
                    default: can_fold = false; break;
                }
                if (can_fold) {
                    re0_buffer_write_fmt(b, "%lldLL", (long long)folded);
                    break;
                }
            }
            /* safety-checked operations */
            if (op == BINOP_DIV || op == BINOP_MOD) {
                const char *fn = op == BINOP_DIV ? "__reo_safe_div" : "__reo_safe_mod";
                re0_buffer_write_fmt(b, "%s(", fn);
                c_gen_expr(c, e->binary.left);
                re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->binary.right);
                re0_buffer_write_char(b, ')');
                break;
            }
            if (op == BINOP_SHL || op == BINOP_SHR) {
                const char *fn = op == BINOP_SHL ? "__reo_safe_shl" : "__reo_safe_shr";
                re0_buffer_write_fmt(b, "%s(", fn);
                c_gen_expr(c, e->binary.left);
                re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->binary.right);
                re0_buffer_write_char(b, ')');
                break;
            }
            /* string concatenation: str + str -> __reo_str_concat (either operand being str
               counts as concatenation; sema guarantees str is never added to a numeric) */
            if (op == BINOP_ADD &&
                (expr_is_string(e->binary.left) || expr_is_string(e->binary.right))) {
                re0_buffer_write_str(b, "__reo_str_concat(");
                c_gen_expr(c, e->binary.left);
                re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->binary.right);
                re0_buffer_write_char(b, ')');
                break;
            }
            re0_buffer_write_char(b, '('); c_gen_expr(c, e->binary.left);
            re0_buffer_write_fmt(b, " %s ", binop_c(op));
            c_gen_expr(c, e->binary.right); re0_buffer_write_char(b, ')');
            break;
        }
        case EXPR_UNARY: {
            const char *operator = NULL;
            switch (e->unary.op) {
                case UNOP_NEG: operator = "-"; break;
                case UNOP_NOT: operator = "!"; break;
                case UNOP_BNOT: operator = "~"; break;
                case UNOP_REF:
                case UNOP_REFMUT: operator = "&"; break;
                case UNOP_DEREF: operator = "*"; break;
            }
            if (!operator) {
                re0_error_append(c->errors, RE0_ERR_INTERNAL, e->span, NULL,
                                 "unsupported unary operator in C backend");
                c->had_error = true;
                re0_buffer_write_str(b, "0");
                break;
            }
            re0_buffer_write_char(b, '(');
            re0_buffer_write_str(b, operator);
            c_gen_expr(c, e->unary.operand);
            re0_buffer_write_char(b, ')');
            break;
        }
        case EXPR_CALL: {
            /* method sugar: x.len() → str_len(x) or vec_len(x) */
            if (e->call.callee->kind == EXPR_SELECT &&
                strcmp(e->call.callee->select.field, "len") == 0) {
                Re0Expr *obj = e->call.callee->select.object;
                if (expr_is_string(obj)) {
                    re0_buffer_write_str(b, "__reo_str_len((char*)");
                    c_gen_expr(c, obj);
                    re0_buffer_write_char(b, ')');
                    break;
                } else {
                    re0_buffer_write_str(b, "__reo_vec_len((__reo_vec_t*)");
                    c_gen_expr(c, obj);
                    re0_buffer_write_char(b, ')');
                    break;
                }
            }
            /* method call: obj.method(args) → MangledSymbol(obj, args...) */
            if (e->call.callee->kind == EXPR_SELECT) {
                Re0Expr *sel = e->call.callee;
                Re0Expr *obj = sel->select.object;
                char c_type[128];
                const char *sn = NULL;
                if (infer_expr_c_type(obj, c_type, sizeof(c_type))) sn = c_type;
                if (sn) {
                    const char *mangled = re0_model_lookup_method(
                        c->model, sn, sel->select.field);
                    if (mangled) {
                        re0_buffer_write_str(b, mangled);
                        re0_buffer_write_str(b, "(&(");   /* pass self by pointer */
                        c_gen_expr(c, obj);
                        re0_buffer_write_char(b, ')');
                        for (int i = 0; i < e->call.arg_count; i++) {
                            re0_buffer_write_str(b, ", ");
                            c_gen_expr(c, e->call.args[i]);
                        }
                        re0_buffer_write_char(b, ')');
                        break;
                    }
                }
            }
            /* enum constructor: Enum::Variant(args) */
            if (e->call.callee->kind == EXPR_IDENT &&
                strchr(e->call.callee->ident.name, ':')) {
                char ename[128], vname[128];
                if (split_qualified(e->call.callee->ident.name, ename, sizeof(ename),
                                    vname, sizeof(vname))) {
                    Re0EnumDef *def = re0_model_find_enum(c->model, ename);
                    if (def) {
                        int tag = re0_model_variant_tag(def, vname);
                        re0_buffer_write_fmt(b, "((%s){ .tag = %d", ename, tag);
                        if (e->call.arg_count > 0) {
                            re0_buffer_write_str(b, ", .u.v0 = ");
                            c_gen_expr(c, e->call.args[0]);
                        }
                        re0_buffer_write_str(b, "})");
                        break;
                    }
                }
            }
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                if (strcmp(fn, "println") == 0 || strcmp(fn, "print") == 0) {
                    Re0Expr *arg = e->call.arg_count > 0 ? e->call.args[0] : NULL;
                    const char *fmt, *cast;
                    if (expr_is_string(arg)) { fmt = "%s"; cast = "(char*)"; }
                    else if (expr_is_float(arg)) { fmt = "%g"; cast = "(double)"; }
                    else if (expr_is_u128(arg)) {
                        re0_buffer_write_str(b, "__reo_print_u128(");
                        c_gen_expr(c, arg);
                        re0_buffer_write_str(b, ")");
                        break;
                    }
                    else if (expr_is_i128(arg)) {
                        re0_buffer_write_str(b, "__reo_print_i128(");
                        c_gen_expr(c, arg);
                        re0_buffer_write_str(b, ")");
                        break;
                    }
                    else { fmt = "%lld"; cast = "(long long)"; }
                    re0_buffer_write_fmt(b, "printf(\"%s%s\", %s",
                        fmt, strcmp(fn, "println") == 0 ? "\\n" : "", cast);
                    if (arg) c_gen_expr(c, arg); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_str(b, "), fflush(stdout)");
                    break;
                }
                if (strcmp(fn, "panic") == 0) {
                    re0_buffer_write_str(b, "(fprintf(stderr, \"panic: %s\\n\", (char*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]);
                    else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_str(b, "), exit(1), 0)");
                    break;
                }
                if (strcmp(fn, "assert") == 0) {
                    re0_buffer_write_str(b, "((");
                    c_gen_expr(c, e->call.args[0]);
                    re0_buffer_write_str(b, ") ? 0 : (fprintf(stderr, \"assertion failed\\n\"), exit(1), 0))");
                    break;
                }
                /* string builtins */
                if (strcmp(fn, "str_len") == 0)    { re0_buffer_write_str(b, "__reo_str_len((char*)"); goto gen1; }
                if (strcmp(fn, "str_to_int") == 0) { re0_buffer_write_str(b, "__reo_str_to_int((char*)"); goto gen1; }
                /* array length: works for both fat-pointer array kinds */
                if (strcmp(fn, "len") == 0 && e->call.arg_count == 1) {
                    char at[128];
                    if (infer_expr_c_type(e->call.args[0], at, sizeof(at)) &&
                        (strcmp(at, "__reo_arr_t") == 0 || strcmp(at, "__reo_arrf_t") == 0)) {
                        re0_buffer_write_str(b, "(&( ");
                        c_gen_expr(c, e->call.args[0]);
                        re0_buffer_write_str(b, "))->len");
                        break;
                    }
                }
                if (strcmp(fn, "str_char_at") == 0){
                    re0_buffer_write_str(b, "__reo_str_char_at((char*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_str(b, ", (int64_t)");
                    if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_char(b, ')');
                    break;
                }
                if (strcmp(fn, "str_concat") == 0) { re0_buffer_write_str(b, "__reo_str_concat((char*)"); goto gen2; }
                if (strcmp(fn, "str_eq") == 0)     { re0_buffer_write_str(b, "__reo_str_eq((char*)"); goto gen2; }
                if (strcmp(fn, "char_to_str") == 0){ re0_buffer_write_str(b, "__reo_char_to_str((char)"); goto gen1; }
                if (strcmp(fn, "to_string") == 0)  { re0_buffer_write_str(b, "__reo_to_string((int64_t)"); goto gen1; }
                if (strcmp(fn, "is_digit") == 0)   { re0_buffer_write_str(b, "__reo_is_digit((char)"); goto gen1; }
                if (strcmp(fn, "is_alpha") == 0)   { re0_buffer_write_str(b, "__reo_is_alpha((char)"); goto gen1; }
                if (strcmp(fn, "is_alnum") == 0)   { re0_buffer_write_str(b, "__reo_is_alnum((char)"); goto gen1; }
                if (strcmp(fn, "free") == 0)       { re0_buffer_write_str(b, "(free((void*)"); goto gen1; }
                if (strcmp(fn, "exit") == 0)       { re0_buffer_write_str(b, "(exit((int)"); goto gen1; }
                if (strcmp(fn, "str_slice") == 0)  {
                    re0_buffer_write_str(b, "__reo_str_slice((char*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_str(b, ", ");
                    if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_str(b, ", ");
                    if (e->call.arg_count > 2) c_gen_expr(c, e->call.args[2]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_char(b, ')');
                    break;
                }
                if (strcmp(fn, "file_read") == 0)  { re0_buffer_write_str(b, "__reo_file_read((char*)"); goto gen1; }
                if (strcmp(fn, "file_write") == 0) {
                    re0_buffer_write_str(b, "__reo_file_write((char*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_str(b, ", (char*)");
                    if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_char(b, ')');
                    break;
                }
                /* Vec builtins */
                if (strcmp(fn, "vec_new") == 0)    { re0_buffer_write_str(b, "__reo_vec_new()"); break; }
                if (strcmp(fn, "vec_len") == 0)    { re0_buffer_write_str(b, "__reo_vec_len((__reo_vec_t*)"); goto gen1v; }
                if (strcmp(fn, "vec_pop") == 0)    { re0_buffer_write_str(b, "__reo_vec_pop((__reo_vec_t*)"); goto gen1v; }
                if (strcmp(fn, "vec_last") == 0)   { re0_buffer_write_str(b, "__reo_vec_last((__reo_vec_t*)"); goto gen1v; }
                if (strcmp(fn, "vec_get") == 0)    { re0_buffer_write_str(b, "__reo_vec_get((__reo_vec_t*)"); goto gen2v; }
                if (strcmp(fn, "vec_set") == 0)    { re0_buffer_write_str(b, "__reo_vec_set((__reo_vec_t*)"); goto gen3v; }
                if (strcmp(fn, "vec_push") == 0)   { re0_buffer_write_str(b, "__reo_vec_push((__reo_vec_t*)"); goto gen2v; }
                /* System builtins */
                if (strcmp(fn, "argv_len") == 0)   { re0_buffer_write_str(b, "__reo_argv_len_fn()"); break; }
                if (strcmp(fn, "argv_get") == 0)   { re0_buffer_write_str(b, "__reo_argv_get_fn((int64_t)"); goto gen1; }
                if (strcmp(fn, "stdin_read") == 0) { re0_buffer_write_str(b, "__reo_stdin_read()"); break; }
                /* GC API builtins */
                if (strcmp(fn, "gc_collect") == 0)    { re0_buffer_write_str(b, "(__reo_gc_collect(), (int64_t)0)"); break; }
                if (strcmp(fn, "gc_stats") == 0)      { re0_buffer_write_str(b, "__reo_gc_stats()"); break; }
                if (strcmp(fn, "gc_add_root") == 0)   { re0_buffer_write_str(b, "(__reo_gc_add_root((void*)(int64_t)"); goto gen1; }
                if (strcmp(fn, "gc_remove_root") == 0){ re0_buffer_write_str(b, "(__reo_gc_remove_root((void*)(int64_t)"); goto gen1; }
                /* spawn/await concurrency runtime */
                if (strcmp(fn, "__reo_spawn") == 0 && e->call.arg_count >= 1) {
                    /* spawn f() → __reo_rt_spawn(&f) */
                    re0_buffer_write_str(b, "__reo_rt_spawn(&");
                    c_gen_expr(c, e->call.args[0]);
                    re0_buffer_write_char(b, ')');
                    break;
                }
                if (strcmp(fn, "__reo_await") == 0 && e->call.arg_count >= 1) {
                    /* await task → GCC stmt expr */
                    int t = c->temp_counter++;
                    re0_buffer_write_fmt(b, "({ int64_t __ab%d; __reo_rt_await((uint64_t)(",
                                         t);
                    c_gen_expr(c, e->call.args[0]);
                    re0_buffer_write_fmt(b, "), &__ab%d, sizeof(int64_t)); __ab%d; })", t, t);
                    break;
                }
                /* svec builtins (string vector) */
                if (strcmp(fn, "svec_new") == 0)  { re0_buffer_write_str(b, "__reo_svec_new()"); break; }
                if (strcmp(fn, "svec_len") == 0)  { re0_buffer_write_str(b, "__reo_svec_len((__reo_svec_t*)"); goto gen1v; }
                if (strcmp(fn, "svec_free") == 0) { re0_buffer_write_str(b, "__reo_svec_free((__reo_svec_t*)"); goto gen1v; }
                if (strcmp(fn, "svec_get") == 0) {
                    re0_buffer_write_str(b, "__reo_svec_get((__reo_svec_t*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_str(b, ", ");
                    if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_char(b, ')'); break;
                }
                if (strcmp(fn, "svec_push") == 0) {
                    re0_buffer_write_str(b, "__reo_svec_push((__reo_svec_t*)");
                    if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                    re0_buffer_write_str(b, ", ");  /* str arg passed as char* as-is, no int64 cast */
                    if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "\"\"");
                    re0_buffer_write_char(b, ')'); break;
                }
                /* dir builtins (directory traversal, i64 handle) */
                if (strcmp(fn, "dir_open") == 0)  { re0_buffer_write_str(b, "__reo_dir_open((char*)"); goto gen1; }
                if (strcmp(fn, "dir_next") == 0)  { re0_buffer_write_str(b, "__reo_dir_next("); goto gen1; }
                if (strcmp(fn, "dir_close") == 0) { re0_buffer_write_str(b, "__reo_dir_close("); goto gen1; }
                /* path builtins */
                if (strcmp(fn, "path_join") == 0) { re0_buffer_write_str(b, "__reo_path_join((char*)"); goto gen2; }
                if (strcmp(fn, "path_ext") == 0)  { re0_buffer_write_str(b, "__reo_path_ext((char*)"); goto gen1; }
                if (strcmp(fn, "path_base") == 0) { re0_buffer_write_str(b, "__reo_path_base((char*)"); goto gen1; }
                if (strcmp(fn, "path_isdir") == 0){ re0_buffer_write_str(b, "__reo_path_isdir((char*)"); goto gen1; }
                /* proc builtin */
                if (strcmp(fn, "proc_run") == 0)  { re0_buffer_write_str(b, "__reo_proc_run((char*)"); goto gen1; }
                goto generic_call;
                /* 1-arg helper wrappers */
                gen1:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_char(b, ')');
                if (strcmp(fn, "free") == 0 || strcmp(fn, "exit") == 0 ||
                    strcmp(fn, "gc_add_root") == 0 || strcmp(fn, "gc_remove_root") == 0)
                    re0_buffer_write_str(b, ", 0)");
                break;
                /* 2-arg helper wrapper */
                gen2:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", (char*)");
                if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_char(b, ')');
                break;
            }
            generic_call:
            /* generic call detection: infer type args → instantiate → call mangled name */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                char mangled_buf[256];
                const char *mangled = try_instantiate_generic_call(
                    c, fn, e->call.args, e->call.arg_count, mangled_buf, sizeof(mangled_buf));
                if (mangled) {
                    re0_buffer_write_str(b, mangled);
                    re0_buffer_write_char(b, '(');
                    for (int i = 0; i < e->call.arg_count; i++) {
                        if (i > 0) re0_buffer_write_str(b, ", ");
                        c_gen_expr(c, e->call.args[i]);
                    }
                    re0_buffer_write_char(b, ')');
                    break;
                }
            }
            /* lambda indirect call: callee is a lambda variable */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                const char *vt = var_c_type(fn);
                if (vt && strcmp(vt, "__reo_fn_ptr") == 0) {
                    re0_buffer_write_str(b, "((int64_t(*)(int64_t");
                    for (int i = 0; i < e->call.arg_count; i++)
                        re0_buffer_write_str(b, ",int64_t");
                    re0_buffer_write_str(b, "))(uintptr_t)");
                    c_gen_expr(c, e->call.callee);
                    re0_buffer_write_str(b, ")(0");
                    for (int i = 0; i < e->call.arg_count; i++) {
                        re0_buffer_write_str(b, ", (int64_t)");
                        c_gen_expr(c, e->call.args[i]);
                    }
                    re0_buffer_write_char(b, ')');
                    break;
                }
            }
            c_gen_expr(c, e->call.callee);
            re0_buffer_write_char(b, '(');
            for (int i = 0; i < e->call.arg_count; i++) {
                if (i > 0) re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->call.args[i]);
            }
                re0_buffer_write_char(b, ')');
                break;
                /* Vec 1-arg */
                gen1v:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_char(b, ')'); break;
                /* Vec 2-arg (int, int) */
                gen2v:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", (int64_t)(uintptr_t)(");
                if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, "))"); break;
                /* Vec 3-arg (int, int, int) */
                gen3v:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", ");
                if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", (int64_t)(uintptr_t)(");
                if (e->call.arg_count > 2) c_gen_expr(c, e->call.args[2]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, "))"); break;
        }
        case EXPR_IF:
            re0_buffer_write_str(b, "((");
            c_gen_expr(c, e->if_expr.cond);
            re0_buffer_write_str(b, ") ? (");
            c_gen_expr(c, e->if_expr.then);
            re0_buffer_write_str(b, ") : (");
            if (e->if_expr.else_) c_gen_expr(c, e->if_expr.else_);
            else re0_buffer_write_str(b, "0");
            re0_buffer_write_str(b, "))");
            break;
        case EXPR_SELECT:
            c_gen_expr(c, e->select.object);
            re0_buffer_write_fmt(b, "%s%s",
                expr_is_pointer_obj(e->select.object) ? "->" : ".",
                e->select.field);
            break;
        case EXPR_STRUCT_INIT: {
            /* generic struct: infer type + instantiate + use mangled name */
            const char *sname = e->struct_init.name;
            char mangled_buf[256]; const char *mangled = try_instantiate_generic_struct_init(c, e, mangled_buf, sizeof(mangled_buf));
            if (mangled) sname = mangled;
            re0_buffer_write_fmt(b, "(%s){ ", sname);
            for (int i = 0; i < e->struct_init.field_count; i++) {
                if (i > 0) re0_buffer_write_str(b, ", ");
                re0_buffer_write_fmt(b, ".%s = ", e->struct_init.fields[i].field);
                c_gen_expr(c, e->struct_init.fields[i].value);
            }
            re0_buffer_write_str(b, " }");
            break;
        }
        case EXPR_MATCH: {
            int t = re0_codegen_new_temp(c);
            bool is_enum_match = false;
            for (int i = 0; i < e->match_.arm_count; i++) {
                Re0Expr *pat = e->match_.arms[i].pat;
                if (pat && pat->kind == EXPR_IDENT && strchr(pat->ident.name, ':')) {
                    is_enum_match = true; break;
                }
            }
            re0_buffer_write_fmt(b, "({ int64_t _s%d = ", t);
            if (is_enum_match) {
                re0_buffer_write_char(b, '(');
                c_gen_expr(c, e->match_.scrutinee);
                re0_buffer_write_str(b, ").tag");
            } else {
                c_gen_expr(c, e->match_.scrutinee);
            }
            re0_buffer_write_fmt(b, "; int64_t _r%d = 0; ", t);
            for (int i = 0; i < e->match_.arm_count; i++) {
                Re0Expr *pat = e->match_.arms[i].pat;
                bool wildcard = pat && pat->kind == EXPR_IDENT &&
                                strcmp(pat->ident.name, "_") == 0;
                if (!wildcard) {
                    if (i > 0) re0_buffer_write_str(b, " else ");
                    re0_buffer_write_str(b, "if (");
                    char ename[128], vname[128];
                    if (pat && pat->kind == EXPR_IDENT &&
                        split_qualified(pat->ident.name, ename, sizeof(ename),
                                       vname, sizeof(vname))) {
                        Re0EnumDef *def = re0_model_find_enum(c->model, ename);
                        int tag = def ? re0_model_variant_tag(def, vname) : -1;
                        re0_buffer_write_fmt(b, "_s%d == %d", t, tag);
                    } else {
                        re0_buffer_write_fmt(b, "_s%d == ", t);
                        c_gen_expr(c, pat);
                    }
                    re0_buffer_write_str(b, ") ");
                } else {
                    if (i > 0) re0_buffer_write_str(b, " else ");
                }
                re0_buffer_write_fmt(b, "{ _r%d = ", t);
                c_gen_expr(c, e->match_.arms[i].body);
                re0_buffer_write_str(b, "; }");
            }
            re0_buffer_write_fmt(b, " _r%d; })", t);
            break;
        }
        case EXPR_TRY: {
            /* expr? → GCC statement expression
             * Option: None=tag0 early return; Some=tag1 take payload
             * Result: Err=tag1 early return;  Ok=tag0 take payload
             * distinguish Option/Result by inner expression type (default Option if inference fails) */
            int t = c->temp_counter++;
            char inner_type[128] = {0};
            int is_result =
                infer_expr_c_type(e->try_.inner, inner_type, sizeof(inner_type)) &&
                strcmp(inner_type, "Result") == 0;
            const char *ty = is_result ? "Result" : "Option";
            int early_tag = is_result ? 1 : 0;
            re0_buffer_write_fmt(b, "({ %s __t%d = (", ty, t);
            c_gen_expr(c, e->try_.inner);
            re0_buffer_write_fmt(b, "); if (__t%d.tag == %d) return __t%d; __t%d.u.v0; })",
                                 t, early_tag, t, t);
            break;
        }
        case EXPR_LAMBDA: {
            /* generate unique lambda name, register for deferred generation, return function pointer */
            char name[64];
            snprintf(name, sizeof(name), "__reo_lambda_%d", g_lambda_counter++);
            if (g_lambda_count < MAX_LAMBDAS) {
                snprintf(g_lambdas[g_lambda_count].name, sizeof(g_lambdas[g_lambda_count].name), "%s", name);
                g_lambdas[g_lambda_count].lambda = e;
                g_lambda_count++;
            }
            re0_buffer_write_fmt(b, "((int64_t)(uintptr_t)&%s)", name);
            break;
        }
        case EXPR_ARRAY: {
            if (e->array.count == 0) {
                re0_buffer_write_str(b, "__reo_arr_rep(0, 0)");
                break;
            }
            /* element-wise type: any float element makes a double array */
            bool is_float = false;
            for (int i = 0; i < e->array.count && !is_float; i++) {
                char et[128];
                if (infer_expr_c_type(e->array.elems[i], et, sizeof(et)))
                    is_float = strcmp(et, "float") == 0 || strcmp(et, "double") == 0;
            }
            const char *dup = is_float ? "__reo_arrf_lit((const double[]){" : "__reo_arr_lit((const int64_t[]){";
            re0_buffer_write_str(b, dup);
            for (int i = 0; i < e->array.count; i++) {
                if (i > 0) re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->array.elems[i]);
            }
            re0_buffer_write_fmt(b, "}, %d)", e->array.count);
            break;
        }
        case EXPR_ARRAY_REPEAT: {
            /* [value; n]: float values need the double-typed helper */
            char et[128];
            bool rep_float = infer_expr_c_type(e->array_repeat.value, et, sizeof(et)) &&
                             (strcmp(et, "float") == 0 || strcmp(et, "double") == 0);
            const char *fn = rep_float ? "__reo_arrf_rep" : "__reo_arr_rep";
            re0_buffer_write_fmt(b, "%s((int64_t)(", fn);
            if (e->array_repeat.count) c_gen_expr(c, e->array_repeat.count);
            else re0_buffer_write_str(b, "0");
            re0_buffer_write_str(b, "), ");
            c_gen_expr(c, e->array_repeat.value);
            re0_buffer_write_char(b, ')');
            break;
        }
        case EXPR_INDEX: {
            /* fat-pointer arrays: checked access via helper. Detect the
             * element type so double arrays read as double. */
            char base[128];
            bool base_known = infer_expr_c_type(e->index.target, base, sizeof(base));
            bool dbl = base_known && strcmp(base, "__reo_arrf_t") == 0;
            if (base_known && (strcmp(base, "__reo_arr_t") == 0 || dbl)) {
                re0_buffer_write_fmt(b, "%s(&(", dbl ? "__reo_arrf_get" : "__reo_arr_get");
                c_gen_expr(c, e->index.target);
                re0_buffer_write_str(b, "), (int64_t)(");
                c_gen_expr(c, e->index.index);
                re0_buffer_write_str(b, "))");
            } else {
                /* non-array base (str, vec, inline struct arrays): raw C index */
                re0_buffer_write_char(b, '(');
                c_gen_expr(c, e->index.target);
                re0_buffer_write_char(b, '[');
                c_gen_expr(c, e->index.index);
                re0_buffer_write_str(b, "])");
            }
            break;
        }
        case EXPR_CAST: {
            const char *target = e->cast.target_type;
            const char *c_type = reo_type_to_c(target);
            /* Determine source type for safe cast selection */
            char src_type[128] = {0};
            bool src_known = infer_expr_c_type(e->cast.inner, src_type, sizeof(src_type));
            bool src_is_str = src_known && strcmp(src_type, "const char*") == 0;
            bool src_is_float = src_known && (strcmp(src_type, "float") == 0 ||
                                               strcmp(src_type, "double") == 0);
            bool src_is_bool = src_known && strcmp(src_type, "bool") == 0;
            bool dst_is_str = target && strcmp(target, "str") == 0;
            bool dst_is_float = target && (strcmp(target, "f32") == 0 ||
                                            strcmp(target, "f64") == 0);
            bool dst_is_int = target && (target[0] == 'i' || target[0] == 'u' ||
                              strcmp(target, "bool") == 0 || strcmp(target, "char") == 0);

            if (src_is_str && dst_is_float) {
                /* str -> float: parse */
                re0_buffer_write_str(b, "__reo_str_to_f64(");
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, ")");
            } else if (src_is_str && dst_is_int) {
                /* str -> int: parse */
                re0_buffer_write_fmt(b, "(%s)__reo_str_to_int(", c_type);
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, ")");
            } else if (src_is_float && dst_is_str) {
                /* float -> str: format */
                re0_buffer_write_str(b, "__reo_f64_to_str(");
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, ")");
            } else if (!src_is_str && !src_is_float && dst_is_str) {
                /* int/bool/char -> str: format via to_string */
                re0_buffer_write_str(b, "__reo_to_string((int64_t)(");
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, "))");
            } else if (src_is_float && dst_is_int) {
                /* float->int: NaN/Inf-safe conversion */
                re0_buffer_write_fmt(b, "__reo_safe_f2i(");
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_fmt(b, ", \"%s\")", c_type);
            } else if (src_known && !src_is_float && !src_is_str && dst_is_float) {
                /* int->float: direct cast is safe */
                re0_buffer_write_fmt(b, "((%s)(", c_type);
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, "))");
            } else if (src_known && !src_is_float && !src_is_str && dst_is_int) {
                /* int->int: narrowing-safe conversion */
                size_t dst_sz = 0;
                if (target) {
                    Re0Type *dt = re0_type_parse(target);
                    if (dt) { dst_sz = re0_type_sizeof(dt->kind); free(dt); }
                }
                size_t src_sz = 8; /* default i64 */
                if (strcmp(src_type, "int8_t") == 0 || strcmp(src_type, "uint8_t") == 0) src_sz = 1;
                else if (strcmp(src_type, "int16_t") == 0 || strcmp(src_type, "uint16_t") == 0) src_sz = 2;
                else if (strcmp(src_type, "int32_t") == 0 || strcmp(src_type, "uint32_t") == 0) src_sz = 4;
                if (dst_sz > 0 && dst_sz < src_sz) {
                    /* narrowing: use safe cast with range check */
                    re0_buffer_write_fmt(b, "((%s)__reo_safe_narrow((int64_t)(", c_type);
                    c_gen_expr(c, e->cast.inner);
                    re0_buffer_write_fmt(b, "), %zuU, \"%s\"))", dst_sz, c_type);
                } else {
                    re0_buffer_write_fmt(b, "((%s)(", c_type);
                    c_gen_expr(c, e->cast.inner);
                    re0_buffer_write_str(b, "))");
                }
            } else if (src_is_bool && dst_is_float) {
                /* bool -> float: 0.0/1.0 */
                re0_buffer_write_fmt(b, "((%s)(", c_type);
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, ") ? 1.0 : 0.0)");
            } else {
                /* fallback: direct C cast */
                re0_buffer_write_fmt(b, "((%s)(", c_type);
                c_gen_expr(c, e->cast.inner);
                re0_buffer_write_str(b, "))");
            }
            break;
        }
        default: re0_buffer_write_str(b, "0"); break;
    }
    return 0;
}

static void c_gen_body(Re0Codegen *c, Re0Stmt **body, int count, int depth) {
    for (int i = 0; i < count; i++) c->backend->gen_stmt(c, body[i], depth);
}

static void c_gen_extern_decl(Re0Codegen *c, const Re0ExternFnDecl *decl) {
    Re0Buffer *b = &c->output;
    const char *return_type = decl->ret_type ? reo_type_to_c(decl->ret_type) : "void";
    re0_buffer_write_fmt(b, "extern %s %s(", return_type, decl->name);
    if (decl->param_count == 0 && !decl->variadic) {
        re0_buffer_write_str(b, "void");
    }
    for (int i = 0; i < decl->param_count; i++) {
        if (i > 0) re0_buffer_write_str(b, ", ");
        re0_buffer_write_fmt(b, "%s %s", reo_type_to_c(decl->params[i].ptype),
                             decl->params[i].pname);
    }
    if (decl->variadic) {
        if (decl->param_count > 0) re0_buffer_write_str(b, ", ");
        re0_buffer_write_str(b, "...");
    }
    re0_buffer_write_str(b, ");\n");
}

static void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth) {
    Re0Buffer *b = &c->output;
    if (!s) return;
    switch (s->kind) {
        case STMT_LET: {
            re0_buffer_write_indent(b, depth);
            /* determine C type */
            const char *ctype = "int64_t";
            char inferred_type[128];
            char ename[128], vname[128];   /* must stay alive until write_fmt/track_var below, avoid stack out-of-scope */
            if (s->let_stmt.init && s->let_stmt.init->kind == EXPR_STRUCT_INIT) {
                /* generic struct: get mangled name via infer_expr_c_type */
                if (!infer_expr_c_type(s->let_stmt.init, inferred_type, sizeof(inferred_type)))
                    ctype = s->let_stmt.init->struct_init.name;
                else
                    ctype = inferred_type;
            } else if (s->let_stmt.init && s->let_stmt.init->kind == EXPR_IDENT) {
                if (split_qualified(s->let_stmt.init->ident.name, ename, sizeof(ename), vname, sizeof(vname)))
                    ctype = ename;
                else
                    ctype = reo_type_to_c(s->let_stmt.type);
            } else if (s->let_stmt.type)
                ctype = reo_type_to_c(s->let_stmt.type);
            else if (s->let_stmt.init &&
                     infer_expr_c_type(s->let_stmt.init, inferred_type,
                                       sizeof(inferred_type)))
                ctype = inferred_type;
            re0_buffer_write_fmt(b, "%s %s", ctype, s->let_stmt.name);
            if (s->let_stmt.init) {
                re0_buffer_write_str(b, " = ");
                if (ctype && strstr(ctype, "*") && s->let_stmt.init->kind != EXPR_STRING) {
                    re0_buffer_write_fmt(b, "(%s)(uintptr_t)(", ctype);
                    c_gen_expr(c, s->let_stmt.init);
                    re0_buffer_write_char(b, ')');
                } else {
                    c_gen_expr(c, s->let_stmt.init);
                }
            }
            re0_buffer_write_str(b, ";\n");
            track_var(s->let_stmt.name, ctype);
            break;
        }
        case STMT_CONST:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_fmt(b, "#define %s ", s->const_decl.name);
            c_gen_expr(c, s->const_decl.value);
            re0_buffer_write_char(b, '\n');
            break;
        case STMT_TYPE_ALIAS:
            re0_buffer_write_fmt(b, "typedef %s %s;\n",
                                reo_type_to_c(s->type_alias.target),
                                s->type_alias.name);
            break;
        case STMT_ASSIGN:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_fmt(b, "%s = ", s->assign.name);
            c_gen_expr(c, s->assign.value);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_FIELD_ASSIGN:
            re0_buffer_write_indent(b, depth);
            c_gen_expr(c, s->field_assign.obj);
            re0_buffer_write_fmt(b, "%s%s = ",
                expr_is_pointer_obj(s->field_assign.obj) ? "->" : ".",
                s->field_assign.field);
            c_gen_expr(c, s->field_assign.value);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_INDEX_ASSIGN:
            re0_buffer_write_indent(b, depth);
            /* array element assignment: bounds-checked via fat pointer */
            {
                char base[128];
                bool base_known = infer_expr_c_type(s->index_assign.target, base, sizeof(base));
                bool dbl = base_known && strcmp(base, "__reo_arrf_t") == 0;
                if (base_known && (strcmp(base, "__reo_arr_t") == 0 || dbl)) {
                    re0_buffer_write_fmt(b, "%s(&(", dbl ? "__reo_arrf_set" : "__reo_arr_set");
                    c_gen_expr(c, s->index_assign.target);
                    re0_buffer_write_str(b, "), (int64_t)(");
                    c_gen_expr(c, s->index_assign.index);
                    re0_buffer_write_str(b, "), ");
                    if (s->index_assign.op == BINOP_ASSIGN_SENTINEL) {
                        c_gen_expr(c, s->index_assign.value);
                    } else if (s->index_assign.op == BINOP_ADD) {                        /* compound += : read, add, write */
                        re0_buffer_write_fmt(b, "%s(&(", dbl ? "__reo_arrf_get" : "__reo_arr_get");
                        c_gen_expr(c, s->index_assign.target);
                        re0_buffer_write_str(b, "), (int64_t)(");
                        c_gen_expr(c, s->index_assign.index);
                        re0_buffer_write_str(b, ")) + ");
                        c_gen_expr(c, s->index_assign.value);
                    }
                    re0_buffer_write_str(b, ");\n");
                    break;
                }
            }
            c_gen_expr(c, s->index_assign.target);
            re0_buffer_write_char(b, '[');
            c_gen_expr(c, s->index_assign.index);
            re0_buffer_write_str(b, "] = ");
            c_gen_expr(c, s->index_assign.value);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_EXPR:
            re0_buffer_write_indent(b, depth);
            c_gen_expr(c, s->expr_stmt.expr);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_RETURN:
            re0_buffer_write_indent(b, depth);
            if (s->return_stmt.value) {
                re0_buffer_write_str(b, "return ");
                c_gen_expr(c, s->return_stmt.value);
            } else {
                re0_buffer_write_str(b, "return 0");
            }
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_IF:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "if (");
            c_gen_expr(c, s->if_stmt.branches[0].cond);
            re0_buffer_write_str(b, ") {\n");
            c_gen_body(c, s->if_stmt.branches[0].body,
                       s->if_stmt.branches[0].body_count, depth + 1);
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "}");
            /* else-if chain: if else body is a single if statement, emit "else if" instead of "else { if }" */
            if (s->if_stmt.else_body && s->if_stmt.else_count > 0) {
                if (s->if_stmt.else_count == 1 && s->if_stmt.else_body[0] &&
                    s->if_stmt.else_body[0]->kind == STMT_IF) {
                    /* else-if chain */
                    re0_buffer_write_str(b, " else ");
                    c_gen_stmt(c, s->if_stmt.else_body[0], depth);
                } else {
                    re0_buffer_write_str(b, " else {\n");
                    c_gen_body(c, s->if_stmt.else_body, s->if_stmt.else_count, depth + 1);
                    re0_buffer_write_indent(b, depth);
                    re0_buffer_write_str(b, "}");
                }
            }
            re0_buffer_write_str(b, "\n");
            break;
        case STMT_WHILE:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "while (");
            c_gen_expr(c, s->while_stmt.cond);
            re0_buffer_write_str(b, ") {\n");
            c_gen_body(c, s->while_stmt.body, s->while_stmt.body_count, depth + 1);
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "}\n");
            break;
        case STMT_FOR: {
            re0_buffer_write_indent(b, depth);
            Re0Expr *iter = s->for_stmt.iter;
            /* range iteration: for i in start..end */
            if (iter && iter->kind == EXPR_BINARY &&
                iter->binary.op == BINOP_RANGE) {
                re0_buffer_write_fmt(b, "for (int64_t %s = ", s->for_stmt.var);
                c_gen_expr(c, iter->binary.left);
                re0_buffer_write_fmt(b, "; %s < (int64_t)(", s->for_stmt.var);
                c_gen_expr(c, iter->binary.right);
                re0_buffer_write_fmt(b, "); %s++) {\n", s->for_stmt.var);
            }
            /* string iteration: for ch in s → byte by byte */
            else if (iter && expr_is_string(iter)) {
                int t = c->temp_counter++;
                re0_buffer_write_fmt(b, "{ const char* __s%d = ", t);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "; int64_t __n%d = (int64_t)strlen(__s%d);\n",
                                     t, t);
                re0_buffer_write_fmt(b, "for (int64_t %s = 0; %s < __n%d; %s++) {\n"
                                     "int64_t %s_val = (int64_t)(unsigned char)__s%d[%s];\n",
                                     s->for_stmt.var, s->for_stmt.var, t, s->for_stmt.var,
                                     s->for_stmt.var, t, s->for_stmt.var);
                /* inside body, var_val replaces ch's value */
            }
            /* Vec iteration: for x in v → walk i64 slots */
            else if (iter && expr_is_vec(iter)) {
                int t = c->temp_counter++;
                re0_buffer_write_fmt(b, "{ __reo_vec_t* __v%d = ", t);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "; for (int64_t __i%d = 0; __i%d < __v%d->len; __i%d++) {\n",
                                     t, t, t, t);
                re0_buffer_write_fmt(b, "int64_t %s = __v%d->data[__i%d];\n",
                                     s->for_stmt.var, t, t);
            }
            /* fat-pointer array iteration: for x in [..] / arr */
            else if (iter && (iter->kind == EXPR_ARRAY || iter->kind == EXPR_ARRAY_REPEAT ||
                              (iter->kind == EXPR_IDENT && expr_is_array_var(iter)))) {
                int t = c->temp_counter++;
                char at[128];
                bool dbl = infer_expr_c_type(iter, at, sizeof(at)) &&
                           strcmp(at, "__reo_arrf_t") == 0;
                re0_buffer_write_fmt(b, "{ %s __a%d = ", dbl ? "__reo_arrf_t" : "__reo_arr_t", t);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "; for (int64_t __i%d = 0; __i%d < __a%d.len; __i%d++) {\n",
                                     t, t, t, t);
                re0_buffer_write_fmt(b, "%s %s = __a%d.data[__i%d];\n",
                                     dbl ? "double" : "int64_t", s->for_stmt.var, t, t);
            }
            /* numeric iteration: for i in count */
            else {
                re0_buffer_write_fmt(b, "for (int64_t %s = 0; %s < (int64_t)(",
                                     s->for_stmt.var, s->for_stmt.var);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "); %s++) {\n", s->for_stmt.var);
            }
            c_gen_body(c, s->for_stmt.body, s->for_stmt.body_count, depth + 1);
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "}\n");
            /* string/Vec/array iteration needs an extra closing brace for the outer block */
            if (iter && iter->kind != EXPR_BINARY &&
                (expr_is_string(iter) || expr_is_vec(iter) || expr_is_array_var(iter) ||
                 iter->kind == EXPR_ARRAY || iter->kind == EXPR_ARRAY_REPEAT))
                re0_buffer_write_str(b, "}\n");
            break;
        }
        case STMT_FUNCTION: {
            /* generic function: register and skip, wait for call sites to instantiate on demand */
            if (s->function.type_param_count > 0) {
                register_generic_fn(s->function.name, s);
                break;
            }
            track_fn_ret(s->function.name,
                         s->function.ret_type ? s->function.ret_type : "unit");
            const char *fn_name = s->function.name;
            if (strcmp(fn_name, "main") == 0) fn_name = "main_";
            const char *ret_c = reo_type_to_c(s->function.ret_type);
            re0_buffer_write_fmt(b, "%s %s(", ret_c, fn_name);
            for (int i = 0; i < s->function.param_count; i++) {
                if (i > 0) re0_buffer_write_str(b, ", ");
                re0_buffer_write_fmt(b, "%s %s",
                                    reo_type_to_c(s->function.params[i].ptype),
                                    s->function.params[i].name);
            }
            re0_buffer_write_str(b, ") {\n");
            /* track param types */
            clear_var_types();
            for (int i = 0; i < s->function.param_count; i++)
                track_var(s->function.params[i].name, reo_type_to_c(s->function.params[i].ptype));
            c_gen_body(c, s->function.body, s->function.body_count, 1);
            re0_buffer_write_str(b, "}\n\n");
            break;
        }
        case STMT_STRUCT:
            /* generic struct: register and skip, wait for instantiation */
            if (s->struct_decl.type_param_count > 0) {
                register_generic_struct(s->struct_decl.name, s);
                break;
            }
            re0_buffer_write_str(b, "typedef struct { ");
            for (int i = 0; i < s->struct_decl.field_count; i++) {
                re0_buffer_write_fmt(b, "%s %s; ",
                                    reo_type_to_c(s->struct_decl.fields[i].type),
                                    s->struct_decl.fields[i].name);
                track_struct_field(s->struct_decl.name,
                                   s->struct_decl.fields[i].name,
                                   s->struct_decl.fields[i].type);
            }
            re0_buffer_write_fmt(b, "} %s;\n", s->struct_decl.name);
            break;
        case STMT_ENUM:
            re0_buffer_write_fmt(b, "typedef struct { int64_t tag; union { ");
            for (int i = 0; i < s->enum_decl.variant_count; i++) {
                if (s->enum_decl.variants[i].type_count > 0)
                    re0_buffer_write_fmt(b, "int64_t v%d; ", i);
            }
            re0_buffer_write_fmt(b, "} u; } %s;\n", s->enum_decl.name);
            break;
        case STMT_EXTERN:
            for (int i = 0; i < s->extern_.func_count; i++)
                c_gen_extern_decl(c, &s->extern_.funcs[i]);
            re0_buffer_write_char(b, '\n');
            break;
        case STMT_TRAIT:
            /* traits are compile-time only, no C output */
            break;
        case STMT_IMPL: {
            /* set self param type + generate method with mangled name */
            for (int i = 0; i < s->impl.method_count; i++) {
                Re0Stmt *m = s->impl.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                char self_ptr_type[160];
                snprintf(self_ptr_type, sizeof(self_ptr_type), "%s*", s->impl.name);
                /* temp-patch AST, generate, restore: leaving the stack buffer
                 * pointer in the AST would dangle on any later traversal
                 * (LSP re-checks, IR backend pass, ...) */
                int self_idx = -1;
                for (int j = 0; j < m->function.param_count; j++) {
                    if (strcmp(m->function.params[j].name, "self") == 0 &&
                        !m->function.params[j].ptype) {
                        m->function.params[j].ptype = self_ptr_type;
                        self_idx = j;
                    }
                }
                /* temporarily swap in mangled name */
                char *orig_name = m->function.name;
                char mangled_buf[256];
                const char *mangled = re0_model_method_symbol(
                    s->impl.trait_name, s->impl.name, orig_name, mangled_buf, sizeof(mangled_buf));
                m->function.name = (char*)mangled;
                c_gen_stmt(c, m, depth);
                m->function.name = orig_name;
                if (self_idx >= 0) m->function.params[self_idx].ptype = NULL;
            }
            break;
        }
        case STMT_PUB:
            if (s->pub.inner) c_gen_stmt(c, s->pub.inner, depth);
            break;
        case STMT_ATTRIBUTE:
            /* @gc(...) only sets gc_mode (already scanned); @repr(C) etc.
               are compile-time only. Always emit inner statement. */
            if (s->attribute.inner) c_gen_stmt(c, s->attribute.inner, depth);
            break;
        case STMT_MODULE:
            if (s->module.body) c_gen_body(c, s->module.body, s->module.body_count, depth);
            break;
        case STMT_COMPONENT: {
            /* generate struct typedef from state fields */
            if (s->component.state_count > 0) {
                re0_buffer_write_str(b, "typedef struct { ");
                for (int i = 0; i < s->component.state_count; i++)
                    re0_buffer_write_fmt(b, "%s %s; ",
                                        reo_type_to_c(s->component.state[i].type),
                                        s->component.state[i].name);
                re0_buffer_write_fmt(b, "} %s;\n", s->component.name);
            }
            /* set self param type + generate method with mangled name (same as STMT_IMPL) */
            for (int i = 0; i < s->component.method_count; i++) {
                Re0Stmt *m = s->component.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                char self_ptr_type[160];
                snprintf(self_ptr_type, sizeof(self_ptr_type), "%s*", s->component.name);
                /* temp-patch AST, generate, restore: see STMT_IMPL */
                int self_idx = -1;
                for (int j = 0; j < m->function.param_count; j++) {
                    if (strcmp(m->function.params[j].name, "self") == 0 &&
                        !m->function.params[j].ptype) {
                        m->function.params[j].ptype = self_ptr_type;
                        self_idx = j;
                    }
                }
                char *orig_name = m->function.name;
                char mangled_buf[256];
                const char *mangled = re0_model_method_symbol(
                    NULL, s->component.name, orig_name, mangled_buf, sizeof(mangled_buf));
                m->function.name = (char*)mangled;
                c_gen_stmt(c, m, depth);
                m->function.name = orig_name;
                if (self_idx >= 0) m->function.params[self_idx].ptype = NULL;
            }
            break;
        }
        case STMT_IMPORT:
            /* imports are no-ops in C backend (single-file) */
            break;
        case STMT_BREAK:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "break;\n");
            break;
        case STMT_CONTINUE:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "continue;\n");
            break;
        default: break;
    }
}

static void reset_c_state(void) {
    clear_var_types();
    g_fn_ret_count = 0;
    g_struct_field_count = 0;
    g_lambda_count = 0;
    g_lambda_counter = 0;
    g_generic_struct_count = 0;
    g_struct_instance_count = 0;
    g_generic_fn_count = 0;
    g_instantiated_count = 0;
    g_pending_count = 0;
}

static void c_begin(Re0Codegen *c) {
    reset_c_state();
    if (c->backend == &re0_backend_c_freestanding) {
        re0_buffer_write_str(&c->output,
            "#include <stdint.h>\n#include <stdbool.h>\n\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Option;\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Result;\n"
            "typedef int64_t __reo_fn_ptr;\n\n");
        g_fwd_insert_pos = c->output.len;
        return;
    }
    re0_buffer_write_str(&c->output,
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <dirent.h>\n#include <sys/stat.h>\n\n"
        /* Option/Result core enum types */
        "typedef struct { int64_t tag; union { int64_t v0; } u; } Option;\n"
        "typedef struct { int64_t tag; union { int64_t v0; } u; } Result;\n"
        "typedef int64_t __reo_fn_ptr;\n\n"
        /* runtime safety helpers */
        "static int64_t __reo_safe_div(int64_t a, int64_t b) {\n"
        "    if (b == 0) { fprintf(stderr, \"runtime error: division by zero\\n\"); abort(); }\n"
        "    if (a == INT64_MIN && b == -1) { fprintf(stderr, \"runtime error: division overflow\\n\"); abort(); }\n"
        "    return a / b;\n"
        "}\n"
        "static int64_t __reo_safe_mod(int64_t a, int64_t b) {\n"
        "    if (b == 0) { fprintf(stderr, \"runtime error: division by zero\\n\"); abort(); }\n"
        "    if (a == INT64_MIN && b == -1) { fprintf(stderr, \"runtime error: division overflow\\n\"); abort(); }\n"
        "    return a % b;\n"
        "}\n"
        "static int64_t __reo_safe_shl(int64_t a, int64_t b) {\n"
        "    return a << (b & 63);\n"
        "}\n"
        "static int64_t __reo_safe_shr(int64_t a, int64_t b) {\n"
        "    return a >> (b & 63);\n"
        "}\n"
        /* __reo_safe_narrow: well-defined wrapping (Rust `as` semantics), no abort.
           The outer C cast interprets the returned value in the destination type. */
        "static int64_t __reo_safe_narrow(int64_t v, size_t bytes, const char* ty) {\n"
        "    if (bytes == 0 || bytes >= 8) return v;\n"
        "    unsigned bits = (unsigned)(bytes * 8);\n"
        "    uint64_t mask = (((uint64_t)1) << bits) - 1;\n"
        "    uint64_t u = (uint64_t)v & mask;\n"
        "    if (ty && ty[0] == 'u') return (int64_t)u;\n"
        "    uint64_t sign = ((uint64_t)1) << (bits - 1);\n"
        "    if (u & sign) u |= ~mask;\n"
        "    return (int64_t)u;\n"
        "}\n"
        "static int64_t __reo_safe_f2i(double v, const char* ty) {\n"
        "    if (v != v) return 0;\n"
        "    if (ty && (strncmp(ty, \"uint\", 4) == 0 || strncmp(ty, \"int\", 3) == 0)) {\n"
        "        /* narrow integer target (intN_t / uintN_t, N < 64):\n"
        "         * saturate to the target range like Rust `as` (no wrap).\n"
        "         * digits start at index 4 for uintN_t, 3 for intN_t. */\n"
        "        const char* d = ty + (ty[0] == 'u' ? 4 : 3);\n"
        "        int bits;\n"
        "        if (d[0] == '8') bits = 8;\n"
        "        else if (d[0] == '1' && d[1] == '6') bits = 16;\n"
        "        else if (d[0] == '3' && d[1] == '2') bits = 32;\n"
        "        else bits = 64;\n"
        "        if (bits < 64) {\n"
        "            if (ty[0] == 'u') {\n"
        "                double max = (double)(((((uint64_t)1) << (bits - 1)) - 1) * 2 + 1);\n"
        "                if (v == (double)(1.0/0.0)) return (int64_t)(uint64_t)max;\n"
        "                if (v == (double)(-1.0/0.0) || v < 0.0) return 0;\n"
        "                if (v > max) return (int64_t)(uint64_t)max;\n"
        "                return (int64_t)(uint64_t)v;\n"
        "            } else {\n"
        "                double max = (double)(((int64_t)1) << (bits - 1)) - 1.0;\n"
        "                double min = -max - 1.0;\n"
        "                if (v == (double)(1.0/0.0)) return (int64_t)max;\n"
        "                if (v == (double)(-1.0/0.0)) return (int64_t)min;\n"
        "                if (v > max) return (int64_t)max;\n"
        "                if (v < min) return (int64_t)min;\n"
        "                return (int64_t)v;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    if (ty && strcmp(ty, \"bool\") == 0) return v != 0.0;\n"
        "    if (v == (double)(1.0/0.0)) return 9223372036854775807LL;\n"
        "    if (v == (double)(-1.0/0.0)) return (-9223372036854775807LL - 1);\n"
        "    if (v > (double)9223372036854775807LL) return 9223372036854775807LL;\n"
        "    if (v < (double)(-9223372036854775807LL - 1)) return (-9223372036854775807LL - 1);\n"
        "    return (int64_t)v;\n"
        "}\n"
        "static void __reo_print_u128(unsigned __int128 v) {\n"
        "    char buf[41]; char *p = buf + sizeof(buf) - 1; *p = '\\0';\n"
        "    if (v == 0) { *(--p) = '0'; }\n"
        "    else { while (v > 0) { *(--p) = (char)('0' + (int)(v % 10)); v /= 10; } }\n"
        "    printf(\"%s\\n\", p);\n"
        "}\n"
        "static void __reo_print_i128(__int128 v) {\n"
        "    if (v < 0) { putchar('-'); __reo_print_u128((unsigned __int128)(-v)); }\n"
        "    else __reo_print_u128((unsigned __int128)v);\n"
        "}\n"
        /* string helpers */
        "static int64_t __reo_str_len(const char* s) { return s ? (int64_t)strlen(s) : 0; }\n"
        "static const char* __reo_str_concat(const char* a, const char* b) {\n"
        "    if (!a) a = \"\"; if (!b) b = \"\";\n"
        "    size_t la = strlen(a), lb = strlen(b);\n"
        "    char* r = (char*)malloc(la + lb + 1);\n"
        "    memcpy(r, a, la); memcpy(r + la, b, lb); r[la + lb] = 0;\n"
        "    return r;\n"
        "}\n"
        "static bool __reo_str_eq(const char* a, const char* b) {\n"
        "    if (!a) a = \"\"; if (!b) b = \"\";\n"
        "    return strcmp(a, b) == 0;\n"
        "}\n"
        "static int64_t __reo_str_to_int(const char* s) {\n"
        "    return s ? strtoll(s, NULL, 10) : 0;\n"
        "}\n"
        "static double __reo_str_to_f64(const char* s) {\n"
        "    return s ? strtod(s, NULL) : 0.0;\n"
        "}\n"
        "static const char* __reo_f64_to_str(double v) {\n"
        "    char* r = (char*)malloc(64);\n"
        "    snprintf(r, 64, \"%g\", v);\n"
        "    return r;\n"
        "}\n"
        "static const char* __reo_str_slice(const char* s, int64_t start, int64_t end) {\n"
        "    if (!s) return \"\";\n"
        "    int64_t len = (int64_t)strlen(s);\n"
        "    if (start < 0) start = 0;\n"
        "    if (end > len) end = len;\n"
        "    if (start >= end) return \"\";\n"
        "    char* r = (char*)malloc((size_t)(end - start) + 1);\n"
        "    memcpy(r, s + start, (size_t)(end - start)); r[end - start] = 0;\n"
        "    return r;\n"
        "}\n"
        "static char __reo_str_char_at(const char* s, int64_t i) {\n"
        "    if (!s || i < 0 || i >= (int64_t)strlen(s)) return 0;\n"
        "    return s[i];\n"
        "}\n"
        "static const char* __reo_char_to_str(char c) {\n"
        "    char* r = (char*)malloc(2); r[0] = c; r[1] = 0; return r;\n"
        "}\n"
        "static const char* __reo_to_string(int64_t v) {\n"
        "    char* r = (char*)malloc(32); snprintf(r, 32, \"%lld\", (long long)v); return r;\n"
        "}\n"
        "static bool __reo_is_digit(char c) { return c >= '0' && c <= '9'; }\n"
        "static bool __reo_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }\n"
        "static bool __reo_is_alnum(char c) { return __reo_is_digit(c) || __reo_is_alpha(c); }\n");
    re0_buffer_write_str(&c->output,
        /* file I/O with path traversal protection */
        "static const char* __reo_file_read(const char* path) {\n"
        "    if (!path || strstr(path, \"..\")) { fprintf(stderr, \"runtime error: invalid path\\n\"); abort(); }\n"
        "    FILE* f = fopen(path, \"rb\");\n"
        "    if (!f) return \"\";\n"
        "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n"
        "    if (sz < 0 || sz > 16777216) { fclose(f); return \"\"; }\n"
        "    char* r = (char*)malloc((size_t)sz + 1);\n"
        "    size_t rd = fread(r, 1, (size_t)sz, f); r[rd] = 0; fclose(f);\n"
        "    return r;\n"
        "}\n"
        "static void __reo_file_write(const char* path, const char* data) {\n"
        "    if (!path || strstr(path, \"..\")) { fprintf(stderr, \"runtime error: invalid path\\n\"); abort(); }\n"
        "    FILE* f = fopen(path, \"wb\");\n"
        "    if (!f) return;\n"
        "    if (data) fputs(data, f);\n"
        "    fclose(f);\n"
        "}\n"
        /* Vec helpers: Vec<T> is stored as { int64_t* ptr; int64_t len; int64_t cap } */
        "#define VEC_INIT_CAP 4\n"
        "typedef struct { int64_t* data; int64_t len; int64_t cap; } __reo_vec_t;\n"
        "static __reo_vec_t* __reo_vec_new(void) {\n"
        "    __reo_vec_t* v = (__reo_vec_t*)malloc(sizeof(__reo_vec_t));\n"
        "    if (!v) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    v->data = (int64_t*)malloc(sizeof(int64_t) * VEC_INIT_CAP);\n"
        "    if (!v->data) { free(v); fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    v->len = 0; v->cap = VEC_INIT_CAP; return v;\n"
        "}\n"
        "static void __reo_vec_push(__reo_vec_t* v, int64_t x) {\n"
        "    if (v->len >= v->cap) {\n"
        "        v->cap *= 2;\n"
        "        v->data = (int64_t*)realloc(v->data, sizeof(int64_t) * (size_t)v->cap);\n"
        "        if (!v->data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    }\n"
        "    v->data[v->len++] = x;\n"
        "}\n"
        "static int64_t __reo_vec_get(__reo_vec_t* v, int64_t i) {\n"
        "    if (i < 0 || i >= v->len) { fprintf(stderr, \"runtime error: index out of bounds\\n\"); abort(); }\n"
        "    return v->data[i];\n"
        "}\n"
        "static void __reo_vec_set(__reo_vec_t* v, int64_t i, int64_t x) {\n"
        "    if (i < 0 || i >= v->len) { fprintf(stderr, \"runtime error: index out of bounds\\n\"); abort(); }\n"
        "    v->data[i] = x;\n"
        "}\n"
        "static int64_t __reo_vec_pop(__reo_vec_t* v) {\n"
        "    if (v->len <= 0) { fprintf(stderr, \"runtime error: pop from empty vec\\n\"); abort(); }\n"
        "    return v->data[--v->len];\n"
        "}\n"
        "static int64_t __reo_vec_last(__reo_vec_t* v) {\n"
        "    if (v->len <= 0) { fprintf(stderr, \"runtime error: last on empty vec\\n\"); abort(); }\n"
        "    return v->data[v->len - 1];\n"
        "}\n"
        "static int64_t __reo_vec_len(__reo_vec_t* v) { return v->len; }\n"
        /* fixed-length array: heap-allocated so it can survive function return / be stored
            in a struct (stack compound literals would dangle). Uses malloc like vec
            (reclaimed at the language level by GC/program lifetime). */
        "static int64_t* __reo_array_repeat(int64_t n, int64_t val) {\n"
        "    if (n < 0) n = 0;\n"
        "    int64_t* a = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    for (int64_t i = 0; i < n; i++) a[i] = val;\n"
        "    return a;\n"
        "}\n"
        "static int64_t* __reo_array_dup(const int64_t* src, int64_t n) {\n"
        "    if (n < 0) n = 0;\n"
        "    int64_t* a = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    if (n > 0) memcpy(a, src, sizeof(int64_t) * (size_t)n);\n"
        "    return a;\n"
        "}\n"
        /* float arrays: same shape as int64 arrays but double-typed storage */
        "static double* __reo_array_dup_f64(const double* src, int64_t n) {\n"
        "    if (n < 0) n = 0;\n"
        "    double* a = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    if (n > 0) memcpy(a, src, sizeof(double) * (size_t)n);\n"
        "    return a;\n"
        "}\n"
        "static double* __reo_array_repeat_f64(int64_t n, double val) {\n"
        "    if (n < 0) n = 0;\n"
        "    double* a = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    for (int64_t i = 0; i < n; i++) a[i] = val;\n"
        "    return a;\n"
        "}\n"
        /* fat-pointer arrays (literals / repeat): {data, len} with bounds
         * checking on every element access — replaces bare pointers that
         * silently read/write out of bounds */
        "typedef struct { int64_t* data; int64_t len; } __reo_arr_t;\n"
        "typedef struct { double* data; int64_t len; } __reo_arrf_t;\n"
        "static int64_t __reo_chk_idx(int64_t i, int64_t len, const char* what) {\n"
        "    if (i < 0 || i >= len) {\n"
        "        fprintf(stderr, \"runtime error: array index %lld out of bounds (len %lld) in %s\\n\",\n"
        "                (long long)i, (long long)len, what);\n"
        "        abort();\n"
        "    }\n"
        "    return i;\n"
        "}\n"
        "static __reo_arr_t __reo_arr_lit(const int64_t* src, int64_t n) {\n"
        "    __reo_arr_t a;\n"
        "    a.len = n < 0 ? 0 : n;\n"
        "    a.data = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a.data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    if (n > 0) memcpy(a.data, src, sizeof(int64_t) * (size_t)n);\n"
        "    return a;\n"
        "}\n"
        "static __reo_arr_t __reo_arr_rep(int64_t n, int64_t val) {\n"
        "    __reo_arr_t a;\n"
        "    a.len = n < 0 ? 0 : n;\n"
        "    a.data = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a.data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    for (int64_t i = 0; i < a.len; i++) a.data[i] = val;\n"
        "    return a;\n"
        "}\n"
        "static __reo_arrf_t __reo_arrf_lit(const double* src, int64_t n) {\n"
        "    __reo_arrf_t a;\n"
        "    a.len = n < 0 ? 0 : n;\n"
        "    a.data = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a.data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    if (n > 0) memcpy(a.data, src, sizeof(double) * (size_t)n);\n"
        "    return a;\n"
        "}\n"
        "static __reo_arrf_t __reo_arrf_rep(int64_t n, double val) {\n"
        "    __reo_arrf_t a;\n"
        "    a.len = n < 0 ? 0 : n;\n"
        "    a.data = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));\n"
        "    if (!a.data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    for (int64_t i = 0; i < a.len; i++) a.data[i] = val;\n"
        "    return a;\n"
        "}\n"
        "static int64_t __reo_arr_get(__reo_arr_t* a, int64_t i) {\n"
        "    return a->data[__reo_chk_idx(i, a->len, \"array read\")];\n"
        "}\n"
        "static void __reo_arr_set(__reo_arr_t* a, int64_t i, int64_t v) {\n"
        "    a->data[__reo_chk_idx(i, a->len, \"array write\")] = v;\n"
        "}\n"
        "static double __reo_arrf_get(__reo_arrf_t* a, int64_t i) {\n"
        "    return a->data[__reo_chk_idx(i, a->len, \"array read\")];\n"
        "}\n"
        "static void __reo_arrf_set(__reo_arrf_t* a, int64_t i, double v) {\n"
        "    a->data[__reo_chk_idx(i, a->len, \"array write\")] = v;\n"
        "}\n"
        "static int64_t __reo_arr_len(const void* a, int64_t len) { (void)a; return len; }\n"
        /* svec helpers: string vector { char** data; len; cap }, elements are strdup'd strings */
        "typedef struct { char** data; int64_t len; int64_t cap; } __reo_svec_t;\n"
        "static __reo_svec_t* __reo_svec_new(void) {\n"
        "    __reo_svec_t* v = (__reo_svec_t*)malloc(sizeof(__reo_svec_t));\n"
        "    if (!v) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    v->cap = 8; v->len = 0;\n"
        "    v->data = (char**)calloc((size_t)v->cap, sizeof(char*));\n"
        "    if (!v->data) { free(v); fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    return v;\n"
        "}\n"
        "static void __reo_svec_push(__reo_svec_t* v, const char* s) {\n"
        "    if (v->len >= v->cap) {\n"
        "        v->cap *= 2;\n"
        "        v->data = (char**)realloc(v->data, sizeof(char*) * (size_t)v->cap);\n"
        "        if (!v->data) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    }\n"
        "    const char* src = s ? s : \"\";\n"
        "    char* dup = (char*)malloc(strlen(src) + 1);\n"
        "    if (!dup) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    strcpy(dup, src);\n"
        "    v->data[v->len++] = dup;\n"
        "}\n"
        "static const char* __reo_svec_get(__reo_svec_t* v, int64_t i) {\n"
        "    if (i < 0 || i >= v->len) return \"\";\n"
        "    return v->data[i];\n"
        "}\n"
        "static int64_t __reo_svec_len(__reo_svec_t* v) { return v->len; }\n"
        "static void __reo_svec_free(__reo_svec_t* v) {\n"
        "    if (!v) return;\n"
        "    for (int64_t i = 0; i < v->len; i++) free(v->data[i]);\n"
        "    free(v->data); free(v);\n"
        "}\n"
        /* dir helpers: directory traversal (POSIX dirent; mingw/ucrt64 ships a compat layer). Handle is i64 */
        "typedef struct { DIR* d; } __reo_dir_t;\n"
        "static int64_t __reo_dir_open(const char* path) {\n"
        "    if (!path) return -1;\n"
        "    DIR* d = opendir(path);\n"
        "    if (!d) return -1;\n"
        "    __reo_dir_t* h = (__reo_dir_t*)malloc(sizeof(__reo_dir_t));\n"
        "    if (!h) { closedir(d); return -1; }\n"
        "    h->d = d;\n"
        "    return (int64_t)(intptr_t)h;\n"
        "}\n"
        "static const char* __reo_dir_next(int64_t hh) {\n"
        "    __reo_dir_t* h = (__reo_dir_t*)(intptr_t)hh;\n"
        "    if (!h || !h->d) return \"\";\n"
        "    struct dirent* ent = readdir(h->d);\n"
        "    return ent ? ent->d_name : \"\";\n"
        "}\n"
        "static void __reo_dir_close(int64_t hh) {\n"
        "    __reo_dir_t* h = (__reo_dir_t*)(intptr_t)hh;\n"
        "    if (!h) return;\n"
        "    if (h->d) closedir(h->d);\n"
        "    free(h);\n"
        "}\n"
        /* path helpers */
        "static const char* __reo_path_join(const char* a, const char* b) {\n"
        "    const char* aa = a ? a : \"\"; const char* bb = b ? b : \"\";\n"
        "    size_t la = strlen(aa), lb = strlen(bb);\n"
        "    int need_sep = (la > 0 && aa[la-1] != '/') ? 1 : 0;\n"
        "    char* r = (char*)malloc(la + lb + 2);\n"
        "    if (!r) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    memcpy(r, aa, la);\n"
        "    if (need_sep) r[la++] = '/';\n"
        "    memcpy(r + la, bb, lb);\n"
        "    r[la + lb] = 0;\n"
        "    return r;\n"
        "}\n"
        "static const char* __reo_path_ext(const char* p) {\n"
        "    if (!p) return \"\";\n"
        "    const char* dot = strrchr(p, '.');\n"
        "    const char* sep = strrchr(p, '/');\n"
        "    if (!dot || (sep && dot < sep)) return \"\";\n"
        "    char* r = (char*)malloc(strlen(dot) + 1);\n"
        "    if (!r) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    strcpy(r, dot); return r;\n"
        "}\n"
        "static const char* __reo_path_base(const char* p) {\n"
        "    if (!p) return \"\";\n"
        "    const char* sep = strrchr(p, '/');\n"
        "    const char* base = sep ? sep + 1 : p;\n"
        "    char* r = (char*)malloc(strlen(base) + 1);\n"
        "    if (!r) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    strcpy(r, base); return r;\n"
        "}\n"
        "static bool __reo_path_isdir(const char* p) {\n"
        "    if (!p) return false;\n"
        "    struct stat st;\n"
        "    if (stat(p, &st) != 0) return false;\n"
        "    return S_ISDIR(st.st_mode);\n"
        "}\n"
        /* proc helper: subprocess (wraps system) */
        "static int64_t __reo_proc_run(const char* cmd) {\n"
        "    if (!cmd) return -1;\n"
        "    return (int64_t)system(cmd);\n"
        "}\n"
        /* system helpers */
        "static int64_t __reo_argv_len = 0;\n"
        "static const char** __reo_argv_list = NULL;\n"
        "static void __reo_init_argv(int argc, char** argv) {\n"
        "    __reo_argv_len = argc; __reo_argv_list = (const char**)argv;\n"
        "}\n"
        "static int64_t __reo_argv_len_fn(void) { return __reo_argv_len; }\n"
        "static const char* __reo_argv_get_fn(int64_t i) {\n"
        "    if (i < 0 || i >= __reo_argv_len) return \"\";\n"
        "    return __reo_argv_list[i];\n"
        "}\n"
        "static const char* __reo_stdin_read(void) {\n"
        "    size_t cap = 256, len = 0;\n"
        "    char* buf = (char*)malloc(cap);\n"
        "    if (!buf) return \"\";\n"
        "    int c;\n"
        "    while ((c = getchar()) != EOF && c != '\\n') {\n"
        "        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }\n"
        "        buf[len++] = (char)c;\n"
        "    }\n"
        "    buf[len] = 0; return buf;\n"
        "}\n\n");
    re0_buffer_write_str(&c->output,
        /* ── GC runtime (mark-sweep with mode selection) ── */
        /* mode: 0=none(no-op), 1=auto(threshold collect), 2=manual(API only) */
        "typedef struct __reo_gc_node {\n"
        "    void *ptr; size_t size; int ref_count; int kind;\n"
        "    struct __reo_gc_node *next; bool marked;\n"
        "    void (*dtor)(void *);\n"
        "} __reo_gc_node_t;\n"
        "static struct {\n"
        "    __reo_gc_node_t *head; __reo_gc_node_t **roots;\n"
        "    int root_count, root_cap, total_count, threshold, mode;\n"
        "} __reo_gc = { NULL, NULL, 0, 0, 0, 1024, 0 };\n"
        "static void __reo_gc_mark(__reo_gc_node_t *n) {\n"
        "    if (!n || n->marked) return; n->marked = true;\n"
        "}\n"
        "static void __reo_gc_sweep(void) {\n"
        "    __reo_gc_node_t **pp = &__reo_gc.head;\n"
        "    while (*pp) {\n"
        "        __reo_gc_node_t *n = *pp;\n"
        "        if (!n->marked && n->ref_count <= 0 && n->kind != 3) {\n"
        "            *pp = n->next;\n"
        "            if (n->dtor) n->dtor(n->ptr);\n"
        "            free(n->ptr); free(n);\n"
        "            __reo_gc.total_count--;\n"
        "        } else { n->marked = false; pp = &n->next; }\n"
        "    }\n"
        "}\n"
        "static void __reo_gc_collect(void) {\n"
        "    if (__reo_gc.mode == 0) return;\n"
        "    for (int i = 0; i < __reo_gc.root_count; i++)\n"
        "        __reo_gc_mark(__reo_gc.roots[i]);\n"
        "    __reo_gc_sweep();\n"
        "}\n"
        "static int64_t __reo_gc_stats(void) {\n"
        "    return (int64_t)__reo_gc.total_count;\n"
        "}\n"
        "static void __reo_gc_add_root(void *p) {\n"
        "    if (!p) return;\n"
        "    if (__reo_gc.root_count >= __reo_gc.root_cap) {\n"
        "        int nc = __reo_gc.root_cap ? __reo_gc.root_cap * 2 : 16;\n"
        "        __reo_gc_node_t **nr = (__reo_gc_node_t**)realloc(\n"
        "            __reo_gc.roots, sizeof(__reo_gc_node_t*) * (size_t)nc);\n"
        "        if (!nr) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "        __reo_gc.roots = nr; __reo_gc.root_cap = nc;\n"
        "    }\n"
        "    __reo_gc.roots[__reo_gc.root_count++] = (__reo_gc_node_t*)p;\n"
        "}\n"
        "static void __reo_gc_remove_root(void *p) {\n"
        "    if (!p) return;\n"
        "    for (int i = 0; i < __reo_gc.root_count; i++)\n"
        "        if (__reo_gc.roots[i] == (__reo_gc_node_t*)p) {\n"
        "            __reo_gc.roots[i] = __reo_gc.roots[--__reo_gc.root_count];\n"
        "            return;\n"
        "        }\n"
        "}\n"
        "static void *__reo_gc_alloc(size_t sz) {\n"
        "    if (sz == 0) return NULL;\n"
        "    __reo_gc_node_t *n = (__reo_gc_node_t*)malloc(sizeof(__reo_gc_node_t));\n"
        "    if (!n) { fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    n->ptr = malloc(sz);\n"
        "    if (!n->ptr) { free(n); fprintf(stderr, \"runtime error: out of memory\\n\"); abort(); }\n"
        "    n->size = sz; n->ref_count = 1; n->kind = 2;\n"
        "    n->next = __reo_gc.head; __reo_gc.head = n;\n"
        "    __reo_gc.total_count++;\n"
        "    if (__reo_gc.mode == 1 && __reo_gc.total_count > __reo_gc.threshold)\n"
        "        __reo_gc_collect();\n"
        "    return n->ptr;\n"
        "}\n\n");
    re0_buffer_write_str(&c->output,
        /* ── spawn/await concurrency runtime (pthread Phase 0) ── */
        "#include <pthread.h>\n\n"
        "typedef int64_t (*__reo_task_fn)(void);\n"
        "typedef struct { __reo_task_fn fn; size_t result_len; char result_buf[128]; } __reo_task_ctx;\n"
        "typedef struct { pthread_t thread; __reo_task_ctx* ctx; int state; } __reo_task_slot;\n"
        "static __reo_task_slot __reo_tasks[256];\n"
        "static pthread_mutex_t __reo_task_mtx = PTHREAD_MUTEX_INITIALIZER;\n\n"
        "static void* __reo_task_trampoline(void* arg) {\n"
        "    __reo_task_ctx* c = (__reo_task_ctx*)arg;\n"
        "    int64_t ret = c->fn();\n"
        "    memcpy(c->result_buf, &ret, sizeof(int64_t));\n"
        "    c->result_len = sizeof(int64_t);\n"
        "    return NULL;\n"
        "}\n\n"
        "uint64_t __reo_rt_spawn(__reo_task_fn fn) {\n"
        "    if (!fn) return 0;\n"
        "    __reo_task_ctx* ctx = calloc(1, sizeof(__reo_task_ctx));\n"
        "    if (!ctx) return 0;\n"
        "    ctx->fn = fn;\n"
        "    pthread_mutex_lock(&__reo_task_mtx);\n"
        "    int slot = -1;\n"
        "    for (int i = 0; i < 256; i++) if (__reo_tasks[i].state == 0) { slot = i; break; }\n"
        "    if (slot < 0) { pthread_mutex_unlock(&__reo_task_mtx); free(ctx); return 0; }\n"
        "    __reo_tasks[slot].ctx = ctx;\n"
        "    __reo_tasks[slot].state = 1;\n"
        "    int create_rc = pthread_create(&__reo_tasks[slot].thread, NULL, __reo_task_trampoline, ctx);\n"
        "    if (create_rc != 0) { __reo_tasks[slot].ctx = NULL; __reo_tasks[slot].state = 0; pthread_mutex_unlock(&__reo_task_mtx); free(ctx); return 0; }\n"
        "    pthread_mutex_unlock(&__reo_task_mtx);\n"
        "    return (uint64_t)(slot + 1);\n"
        "}\n\n"
        "int32_t __reo_rt_await(uint64_t task_id, void* out, size_t out_cap) {\n"
        "    if (task_id == 0 || task_id > 256) return -1;\n"
        "    int slot = (int)task_id - 1;\n"
        "    pthread_mutex_lock(&__reo_task_mtx);\n"
        "    if (__reo_tasks[slot].state != 1) { pthread_mutex_unlock(&__reo_task_mtx); return -1; }\n"
        "    __reo_tasks[slot].state = 2;\n"
        "    pthread_t thread = __reo_tasks[slot].thread;\n"
        "    __reo_task_ctx* c = __reo_tasks[slot].ctx;\n"
        "    pthread_mutex_unlock(&__reo_task_mtx);\n"
        "    if (pthread_join(thread, NULL) != 0) return -2;\n"
        "    size_t copy = c->result_len < out_cap ? c->result_len : out_cap;\n"
        "    if (out && copy > 0) memcpy(out, c->result_buf, copy);\n"
        "    int32_t ret = (c->result_len > 0) ? 0 : -2;\n"
        "    free(c);\n"
        "    pthread_mutex_lock(&__reo_task_mtx);\n"
        "    __reo_tasks[slot].ctx = NULL;\n"
        "    __reo_tasks[slot].state = 0;\n"
        "    pthread_mutex_unlock(&__reo_task_mtx);\n"
        "    return ret;\n"
        "}\n\n");
    /* record prelude end position (for c_end to insert forward declarations) */
    g_fwd_insert_pos = c->output.len;
}

static void c_end(Re0Codegen *c) {
    /* generate all pending generic function bodies (before main) */
    flush_pending_instantiations(c);
    flush_generic_structs(c);
    flush_lambdas(c);

    if (c->backend == &re0_backend_c_freestanding) return;

    int mode = re0_gc_mode_to_int(c->gc_mode);
    re0_buffer_write_fmt(&c->output,
        "\nint main(int argc, char **argv) {\n"
        "    __reo_gc.mode = %d;\n"
        "    __reo_init_argv(argc, argv);\n"
        "    main_();\n"
        "    __reo_gc_collect();\n"
        "    return 0;\n}\n", mode);
}

Re0Backend re0_backend_c = { "c", c_begin, c_end, c_gen_expr, c_gen_stmt };
Re0Backend re0_backend_c_freestanding = {
    "c-freestanding", c_begin, c_end, c_gen_expr, c_gen_stmt
};
