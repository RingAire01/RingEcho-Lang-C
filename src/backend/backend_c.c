#include "backend.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ── variable type tracking for println format selection ── */
#define MAX_VAR_TYPES 256
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
           strcmp(fn, "stdin_read") == 0;
}

/* Check if a builtin function returns float */
static bool builtin_returns_float(const char *fn) {
    (void)fn; return false;
}

/* Check if a builtin function returns vec pointer */
static bool builtin_returns_vec(const char *fn) {
    return strcmp(fn, "vec_new") == 0;
}

static void clear_var_types(void) {
    for (int i = 0; i < var_type_count; i++) free(var_types[i].name);
    var_type_count = 0;
}

/* ════════════════════════════════════════════════════
 *  泛型函数单态化基础设施
 * ════════════════════════════════════════════════════ */

/* 前向声明 */
static bool infer_expr_c_type(Re0Expr *e, char *type, size_t type_size);
static const char *reo_type_to_c(const char *t);
static int c_gen_expr(Re0Codegen *c, Re0Expr *e);

/* 函数返回类型追踪（供 infer_expr_c_type 使用） */
#define MAX_FN_RETS 128
typedef struct { char name[128]; char ret_c_type[128]; } FnRetSlot;
static FnRetSlot g_fn_rets[MAX_FN_RETS];
static int g_fn_ret_count = 0;

/* Lambda 存储与计数 */
#define MAX_LAMBDAS 64
typedef struct { char name[64]; Re0Expr *lambda; } LambdaSlot;
static LambdaSlot g_lambdas[MAX_LAMBDAS];
static int g_lambda_count = 0;
static int g_lambda_counter = 0;

/* 泛型 struct 存储 */
#define MAX_GENERIC_STRUCTS 32
#define MAX_INSTANTIATED 256
static size_t g_fwd_insert_pos = 0;  /* c_begin 结束后 prelude 的长度 */
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

/* 已实例化的 struct mangled 名集合 */
static char g_struct_instances[MAX_INSTANTIATED][256];
static int g_struct_instance_count = 0;

static bool struct_already_instantiated(const char *mangled) {
    for (int i = 0; i < g_struct_instance_count; i++)
        if (strcmp(g_struct_instances[i], mangled) == 0) return true;
    return false;
}

/* 实例化泛型 struct：记录 pending（typedef 在 c_end 中生成） */
static const char *instantiate_generic_struct(Re0Codegen *c, const char *base_name,
                                               const char *type_arg) {
    Re0Stmt *def = find_generic_struct(base_name);
    if (!def) return base_name;

    static char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s_%s", base_name, type_arg);
    if (struct_already_instantiated(mangled)) return mangled;
    if (g_struct_instance_count >= MAX_INSTANTIATED) return mangled;
    strncpy(g_struct_instances[g_struct_instance_count++], mangled, 255);
    /* typedef 在 flush_generic_structs 中生成 */
    return mangled;
}

/* 在 c_end 中调用：生成所有泛型 struct typedef */
static void flush_generic_structs(Re0Codegen *c) {
    if (g_struct_instance_count == 0) return;
    Re0Buffer *b = &c->output;

    /* 构建所有 typedef 并插入到 prelude 之后 */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int i = 0; i < g_struct_instance_count; i++) {
        const char *mangled = g_struct_instances[i];
        /* 提取 base_name: Pair_int64_t → Pair */
        char base_name[128];
        strncpy(base_name, mangled, sizeof(base_name) - 1);
        base_name[sizeof(base_name)-1] = '\0';
        char *last_under = NULL;
        char *p = base_name;
        while (*p) { if (*p == '_') last_under = p; p++; }
        /* 找到 base_name 中最后一个匹配的 generic struct */
        char orig_base[128];
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
        char *tail = (char*)malloc(tail_len > 0 ? tail_len : 1);
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

/* 在 StructInit 处检测泛型并实例化，返回 mangled 名 */
static const char *try_instantiate_generic_struct_init(Re0Codegen *c, Re0Expr *e) {
    if (e->kind != EXPR_STRUCT_INIT) return NULL;
    const char *name = e->struct_init.name;
    if (!find_generic_struct(name)) return NULL;

    /* 从第一个 field 值推断类型 */
    if (e->struct_init.field_count > 0) {
        char inferred[128];
        if (infer_expr_c_type(e->struct_init.fields[0].value, inferred, sizeof(inferred)))
            return instantiate_generic_struct(c, name, inferred);
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

#define MAX_GENERIC_FNS 64

typedef struct { const char *name; Re0Stmt *def; } GenericFnSlot;
static GenericFnSlot g_generic_fns[MAX_GENERIC_FNS];
static int g_generic_fn_count = 0;

typedef struct { char name[256]; } InstantiatedSlot;
static InstantiatedSlot g_instantiated[MAX_INSTANTIATED];
static int g_instantiated_count = 0;

/* 待实例化条目：延迟到 c_end 生成完整函数体 */
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
    strncpy(g_instantiated[g_instantiated_count].name, mangled, 255);
    g_instantiated[g_instantiated_count].name[255] = '\0';
    g_instantiated_count++;
}

/* 类型替换：在 type_params 中查找 orig，找到则返回 args[i] */
static const char *substitute_one(const char *orig,
                                   char **params, char **args, int n) {
    if (!orig) return NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(orig, params[i]) == 0) return args[i];
    return orig;
}

/* 前向声明 */
static void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth);

/* 实例化泛型函数：记录 pending + 输出前置声明 */
static void instantiate_generic_fn(Re0Codegen *c, Re0Stmt *def,
                                    char **type_args, int type_arg_count) {
    char **type_params = def->function.type_params;
    int tp_count = def->function.type_param_count;
    if (tp_count == 0 || type_arg_count == 0) return;

    /* 计算 mangled 名 */
    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s", def->function.name);
    for (int i = 0; i < type_arg_count && i < tp_count; i++) {
        strncat(mangled, "_", sizeof(mangled) - strlen(mangled) - 1);
        strncat(mangled, type_args[i], sizeof(mangled) - strlen(mangled) - 1);
    }
    if (is_already_instantiated(mangled)) return;
    mark_instantiated(mangled);

    /* 记录 pending（供 c_end 生成前置声明 + 完整函数体） */
    if (g_pending_count < MAX_INSTANTIATED) {
        PendingInst *pi = &g_pending_list[g_pending_count++];
        pi->def = def;
        pi->type_arg_count = type_arg_count < 8 ? type_arg_count : 8;
        for (int i = 0; i < pi->type_arg_count; i++)
            strncpy(pi->type_args[i], type_args[i], 63), pi->type_args[i][63] = '\0';
        strncpy(pi->mangled, mangled, 255), pi->mangled[255] = '\0';
    }
}

/* 在 c_end 中调用：前置声明 + 生成所有 pending 泛型函数体 */
static void flush_pending_instantiations(Re0Codegen *c) {
    if (g_pending_count == 0) return;

    /* 1. 构建前置声明文本 */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int idx = 0; idx < g_pending_count; idx++) {
        PendingInst *pi = &g_pending_list[idx];
        Re0Stmt *def = pi->def;
        char **type_params = def->function.type_params;
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

    /* 2. 在 prelude 之后、用户代码之前插入前置声明 */
    if (decls.len > 0 && g_fwd_insert_pos <= c->output.len) {
        size_t tail_len = c->output.len - g_fwd_insert_pos;
        char *tail = (char*)malloc(tail_len > 0 ? tail_len : 1);
        if (tail) {
            memcpy(tail, c->output.data + g_fwd_insert_pos, tail_len);
            c->output.len = g_fwd_insert_pos;
            re0_buffer_write_n(&c->output, decls.data, decls.len);
            re0_buffer_write_n(&c->output, tail, tail_len);
            free(tail);
        }
    }
    re0_buffer_free(&decls);

    /* 3. 生成完整函数体 */
    for (int idx = 0; idx < g_pending_count; idx++) {
        PendingInst *pi = &g_pending_list[idx];
        Re0Stmt *def = pi->def;
        char **type_params = def->function.type_params;
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

/* 在 c_end 中调用：前置声明 + 生成所有 lambda 函数体 */
static void flush_lambdas(Re0Codegen *c) {
    if (g_lambda_count == 0) return;
    Re0Buffer *b = &c->output;

    /* 1. 构建前置声明并插入到 prelude 之后 */
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
        char *tail = (char*)malloc(tail_len > 0 ? tail_len : 1);
        if (tail) {
            memcpy(tail, c->output.data + g_fwd_insert_pos, tail_len);
            c->output.len = g_fwd_insert_pos;
            re0_buffer_write_n(&c->output, decls.data, decls.len);
            re0_buffer_write_n(&c->output, tail, tail_len);
            free(tail);
        }
    }
    re0_buffer_free(&decls);

    /* 2. 生成函数体 */
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

/* 在 CALL 处检测泛型调用并触发实例化，返回 mangled 名（NULL=非泛型） */
static const char *try_instantiate_generic_call(Re0Codegen *c, const char *fn_name,
                                                 Re0Expr **args, int arg_count) {
    Re0Stmt *def = find_generic_fn(fn_name);
    if (!def) return NULL;

    /* 从第一个参数推断类型（MVP: 单类型参数，取首参类型） */
    if (def->function.type_param_count == 1 && arg_count > 0) {
        char inferred_type[128];
        if (!infer_expr_c_type(args[0], inferred_type, sizeof(inferred_type)))
            return NULL;
        char *type_args[1] = { inferred_type };
        instantiate_generic_fn(c, def, type_args, 1);

        /* 构造 mangled 名返回 */
        static char mangled_buf[256];
        snprintf(mangled_buf, sizeof(mangled_buf), "%s_%s",
                 fn_name, inferred_type);
        return mangled_buf;
    }
    return NULL;
}

/* ════════════════════════════════════════════════════ */

/* ── RingEcho type name → C type string ── */
static const char *reo_type_to_c(const char *t) {
    if (!t) return "int64_t";
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
    if (strcmp(t, "ptr") == 0)   return "void*";
    if (strcmp(t, "unit") == 0 || strcmp(t, "void") == 0) return "void";
    /* struct/enum/type alias names: use directly */
    return t;
}

/* Check if an expression is float-typed */
static bool expr_is_float(Re0Expr *e) {
    if (!e) return false;
    if (e->kind == EXPR_FLOAT) return true;
    if (e->kind == EXPR_BINARY) return expr_is_float(e->binary.left) || expr_is_float(e->binary.right);
    if (e->kind == EXPR_IDENT) return var_is_float(e->ident.name);
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
    return false;
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
                char inferred[128];
                if (infer_expr_c_type(e->struct_init.fields[0].value, inferred, sizeof(inferred))) {
                    snprintf(type, type_size, "%s_%s", e->struct_init.name, inferred);
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
                else if (builtin_returns_float(fn)) known = "double";
                else {
                    /* 查用户函数返回类型 */
                    const char *ret = fn_ret_c_type(fn);
                    if (ret) known = ret;
                }
            }
            /* 枚举构造器 */
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

/* 安全输出 C char 字面量，正确转义特殊字符 */
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
            /* 方法调用: obj.method(args) → MangledSymbol(obj, args...) */
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
                        re0_buffer_write_char(b, '(');
                        c_gen_expr(c, obj);
                        for (int i = 0; i < e->call.arg_count; i++) {
                            re0_buffer_write_str(b, ", ");
                            c_gen_expr(c, e->call.args[i]);
                        }
                        re0_buffer_write_char(b, ')');
                        break;
                    }
                }
            }
            /* 枚举构造器: Enum::Variant(args) */
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
                /* spawn/await 并发运行时 */
                if (strcmp(fn, "__reo_spawn") == 0 && e->call.arg_count >= 1) {
                    /* spawn f() → __reo_rt_spawn(&f) */
                    re0_buffer_write_str(b, "__reo_rt_spawn((void*)(uintptr_t)&");
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
            /* 泛型函数调用检测：推断类型参数 → 实例化 → 调用 mangled 名 */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                const char *mangled = try_instantiate_generic_call(
                    c, fn, e->call.args, e->call.arg_count);
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
            /* Lambda 间接调用: callee 是 lambda 变量 */
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
                re0_buffer_write_str(b, ", ");
                if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_char(b, ')'); break;
                /* Vec 3-arg (int, int, int) */
                gen3v:
                if (e->call.arg_count > 0) c_gen_expr(c, e->call.args[0]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", ");
                if (e->call.arg_count > 1) c_gen_expr(c, e->call.args[1]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_str(b, ", ");
                if (e->call.arg_count > 2) c_gen_expr(c, e->call.args[2]); else re0_buffer_write_str(b, "0");
                re0_buffer_write_char(b, ')'); break;
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
            re0_buffer_write_fmt(b, ".%s", e->select.field);
            break;
        case EXPR_STRUCT_INIT: {
            /* 泛型 struct: 推断类型 + 实例化 + 使用 mangled 名 */
            const char *sname = e->struct_init.name;
            const char *mangled = try_instantiate_generic_struct_init(c, e);
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
             * Option: tag 0=None → early return; Some → extract payload
             * Result: tag 1=Err → early return; Ok → extract payload
             * MVP: 统一按 Option 处理（tag 0 = early return） */
            int t = c->temp_counter++;
            re0_buffer_write_fmt(b, "({ Option __t%d = (", t);
            c_gen_expr(c, e->try_.inner);
            re0_buffer_write_fmt(b, "); if (__t%d.tag == 0) return __t%d; __t%d.u.v0; })",
                                 t, t, t);
            break;
        }
        case EXPR_LAMBDA: {
            /* 生成唯一 lambda 名，注册延迟生成，返回函数指针 */
            char name[64];
            snprintf(name, sizeof(name), "__reo_lambda_%d", g_lambda_counter++);
            if (g_lambda_count < MAX_LAMBDAS) {
                strncpy(g_lambdas[g_lambda_count].name, name, 63);
                g_lambdas[g_lambda_count].name[63] = '\0';
                g_lambdas[g_lambda_count].lambda = e;
                g_lambda_count++;
            }
            re0_buffer_write_fmt(b, "((int64_t)(uintptr_t)&%s)", name);
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
            if (s->let_stmt.init && s->let_stmt.init->kind == EXPR_STRUCT_INIT) {
                /* 泛型 struct: 通过 infer_expr_c_type 获取 mangled 名 */
                if (!infer_expr_c_type(s->let_stmt.init, inferred_type, sizeof(inferred_type)))
                    ctype = s->let_stmt.init->struct_init.name;
                else
                    ctype = inferred_type;
            } else if (s->let_stmt.init && s->let_stmt.init->kind == EXPR_IDENT) {
                char ename[128], vname[128];
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
                c_gen_expr(c, s->let_stmt.init);
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
            re0_buffer_write_fmt(b, ".%s = ", s->field_assign.field);
            c_gen_expr(c, s->field_assign.value);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_EXPR:
            re0_buffer_write_indent(b, depth);
            c_gen_expr(c, s->expr_stmt.expr);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_RETURN:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "return");
            if (s->return_stmt.value) {
                re0_buffer_write_char(b, ' ');
                c_gen_expr(c, s->return_stmt.value);
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
            if (s->if_stmt.else_body && s->if_stmt.else_count > 0) {
                re0_buffer_write_str(b, " else {\n");
                c_gen_body(c, s->if_stmt.else_body, s->if_stmt.else_count, depth + 1);
                re0_buffer_write_indent(b, depth);
                re0_buffer_write_str(b, "}");
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
            /* range 迭代: for i in start..end */
            if (iter && iter->kind == EXPR_BINARY &&
                iter->binary.op == BINOP_RANGE) {
                re0_buffer_write_fmt(b, "for (int64_t %s = ", s->for_stmt.var);
                c_gen_expr(c, iter->binary.left);
                re0_buffer_write_fmt(b, "; %s < (int64_t)(", s->for_stmt.var);
                c_gen_expr(c, iter->binary.right);
                re0_buffer_write_fmt(b, "); %s++) {\n", s->for_stmt.var);
            }
            /* 字符串迭代: for ch in s → 逐字节 */
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
                /* 在 body 中用 var_val 替代 ch 的值 */
            }
            /* 数值迭代: for i in count */
            else {
                re0_buffer_write_fmt(b, "for (int64_t %s = 0; %s < (int64_t)(",
                                     s->for_stmt.var, s->for_stmt.var);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "); %s++) {\n", s->for_stmt.var);
            }
            c_gen_body(c, s->for_stmt.body, s->for_stmt.body_count, depth + 1);
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_str(b, "}\n");
            /* 字符串迭代需要额外闭合括号 */
            if (iter && iter->kind != EXPR_BINARY &&
                iter && expr_is_string(iter))
                re0_buffer_write_str(b, "}\n");
            break;
        }
        case STMT_FUNCTION: {
            /* 泛型函数：注册后跳过，等待调用点按需实例化 */
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
            /* 泛型 struct：注册后跳过，等待实例化 */
            if (s->struct_decl.type_param_count > 0) {
                register_generic_struct(s->struct_decl.name, s);
                break;
            }
            re0_buffer_write_str(b, "typedef struct { ");
            for (int i = 0; i < s->struct_decl.field_count; i++)
                re0_buffer_write_fmt(b, "%s %s; ",
                                    reo_type_to_c(s->struct_decl.fields[i].type),
                                    s->struct_decl.fields[i].name);
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
            /* set self param type + 用 mangled 名生成方法 */
            for (int i = 0; i < s->impl.method_count; i++) {
                Re0Stmt *m = s->impl.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                for (int j = 0; j < m->function.param_count; j++) {
                    if (strcmp(m->function.params[j].name, "self") == 0 &&
                        !m->function.params[j].ptype)
                        m->function.params[j].ptype = s->impl.name;
                }
                /* 临时替换为 mangled 名 */
                char *orig_name = m->function.name;
                const char *mangled = re0_model_method_symbol(
                    s->impl.trait_name, s->impl.name, orig_name);
                m->function.name = (char*)mangled;
                c_gen_stmt(c, m, depth);
                m->function.name = orig_name;
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
            /* set self param type + 用 mangled 名生成方法（与 STMT_IMPL 一致） */
            for (int i = 0; i < s->component.method_count; i++) {
                Re0Stmt *m = s->component.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                for (int j = 0; j < m->function.param_count; j++) {
                    if (strcmp(m->function.params[j].name, "self") == 0 &&
                        !m->function.params[j].ptype)
                        m->function.params[j].ptype = s->component.name;
                }
                char *orig_name = m->function.name;
                const char *mangled = re0_model_method_symbol(
                    NULL, s->component.name, orig_name);
                m->function.name = (char*)mangled;
                c_gen_stmt(c, m, depth);
                m->function.name = orig_name;
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

static void c_begin(Re0Codegen *c) {
    re0_buffer_write_str(&c->output,
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n"
        /* Option/Result 核心枚举类型 */
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
        "    if (b < 0 || b >= 64) { fprintf(stderr, \"runtime error: shift out of bounds\\n\"); abort(); }\n"
        "    return a << b;\n"
        "}\n"
        "static int64_t __reo_safe_shr(int64_t a, int64_t b) {\n"
        "    if (b < 0 || b >= 64) { fprintf(stderr, \"runtime error: shift out of bounds\\n\"); abort(); }\n"
        "    return a >> b;\n"
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
        "    n->marked = false; n->dtor = NULL;\n"
        "    n->next = __reo_gc.head; __reo_gc.head = n;\n"
        "    __reo_gc.total_count++;\n"
        "    if (__reo_gc.mode == 1 && __reo_gc.total_count > __reo_gc.threshold)\n"
        "        __reo_gc_collect();\n"
        "    return n->ptr;\n"
        "}\n\n"
        /* ── spawn/await 并发运行时 (pthread Phase 0) ── */
        "#include <pthread.h>\n\n"
        "typedef void (*__reo_task_entry)(void*);\n"
        "typedef struct { void* user_data; size_t result_len; char result_buf[128]; } __reo_task_ctx;\n"
        "typedef struct { pthread_t thread; __reo_task_ctx* ctx; int used; } __reo_task_slot;\n"
        "static __reo_task_slot __reo_tasks[256];\n"
        "static int __reo_task_next = 0;\n"
        "static pthread_mutex_t __reo_task_mtx = PTHREAD_MUTEX_INITIALIZER;\n\n"
        "static void* __reo_task_trampoline(void* arg) {\n"
        "    __reo_task_ctx* c = (__reo_task_ctx*)arg;\n"
        "    /* entry 负责调用用户函数并把结果写入 c->result_buf */\n"
        "    /* entry 通过 c->user_data 获取用户函数指针 */\n"
        "    typedef int64_t (*__reo_user_fn)(void);\n"
        "    __reo_user_fn fn = (__reo_user_fn)c->user_data;\n"
        "    int64_t ret = fn();\n"
        "    memcpy(c->result_buf, &ret, sizeof(int64_t));\n"
        "    c->result_len = sizeof(int64_t);\n"
        "    return NULL;\n"
        "}\n\n"
        "uint64_t __reo_rt_spawn(void* fn_ptr) {\n"
        "    __reo_task_ctx* ctx = calloc(1, sizeof(__reo_task_ctx));\n"
        "    ctx->user_data = fn_ptr;\n"
        "    pthread_mutex_lock(&__reo_task_mtx);\n"
        "    int slot = __reo_task_next++;\n"
        "    if (slot >= 256) { pthread_mutex_unlock(&__reo_task_mtx); return 0; }\n"
        "    __reo_tasks[slot].ctx = ctx;\n"
        "    __reo_tasks[slot].used = 1;\n"
        "    pthread_create(&__reo_tasks[slot].thread, NULL, __reo_task_trampoline, ctx);\n"
        "    pthread_mutex_unlock(&__reo_task_mtx);\n"
        "    return (uint64_t)(slot + 1);\n"
        "}\n\n"
        "int32_t __reo_rt_await(uint64_t task_id, void* out, size_t out_cap) {\n"
        "    if (task_id == 0 || task_id > 256) return -1;\n"
        "    int slot = (int)task_id - 1;\n"
        "    if (!__reo_tasks[slot].used) return -1;\n"
        "    pthread_join(__reo_tasks[slot].thread, NULL);\n"
        "    __reo_task_ctx* c = __reo_tasks[slot].ctx;\n"
        "    size_t copy = c->result_len < out_cap ? c->result_len : out_cap;\n"
        "    if (out && copy > 0) memcpy(out, c->result_buf, copy);\n"
        "    int32_t ret = (c->result_len > 0) ? 0 : -2;\n"
        "    free(c);\n"
        "    __reo_tasks[slot].used = 0;\n"
        "    return ret;\n"
        "}\n\n");
    /* 记录 prelude 结束位置（供 c_end 插入前置声明） */
    g_fwd_insert_pos = c->output.len;
}

static void c_end(Re0Codegen *c) {
    /* 生成所有 pending 泛型函数体（在 main 之前） */
    flush_pending_instantiations(c);
    flush_generic_structs(c);
    flush_lambdas(c);

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
