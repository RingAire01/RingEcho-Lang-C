#include "backend/backend_c_internal.h"

/* generic struct/fn monomorphization: registration, instantiation,
 * pending flush at c_end, and call-site rewriting. */

void register_generic_struct(const char *name, Re0Stmt *def) {
    if (g_generic_struct_count >= MAX_GENERIC_STRUCTS) return;
    for (int i = 0; i < g_generic_struct_count; i++)
        if (strcmp(g_generic_structs[i].name, name) == 0) return;
    g_generic_structs[g_generic_struct_count].name = name;
    g_generic_structs[g_generic_struct_count].def = def;
    g_generic_struct_count++;
}

Re0Stmt *find_generic_struct(const char *name) {
    for (int i = 0; i < g_generic_struct_count; i++)
        if (strcmp(g_generic_structs[i].name, name) == 0) return g_generic_structs[i].def;
    return NULL;
}

/* set of instantiated struct mangled names (shared via backend_c_internal.h) */
RE0_THREAD_LOCAL char g_struct_instances[MAX_INSTANTIATED][256];
RE0_THREAD_LOCAL int g_struct_instance_count = 0;

bool struct_already_instantiated(const char *mangled) {
    for (int i = 0; i < g_struct_instance_count; i++)
        if (strcmp(g_struct_instances[i], mangled) == 0) return true;
    return false;
}

/* instantiate generic struct: record pending (typedef generated in c_end) */
const char *instantiate_generic_struct(Re0Codegen *c, const char *base_name,
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
void flush_generic_structs(Re0Codegen *c) {
    if (g_struct_instance_count == 0) return;
    (void)c;

    /* build all typedefs and insert after the prelude */
    Re0Buffer decls;
    re0_buffer_init(&decls);
    for (int i = 0; i < g_struct_instance_count; i++) {
        const char *mangled = g_struct_instances[i];
        /* extract base_name: Pair_int64_t -> Pair */
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
const char *try_instantiate_generic_struct_init(Re0Codegen *c, Re0Expr *e,
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



void register_generic_fn(const char *name, Re0Stmt *def) {
    if (g_generic_fn_count >= MAX_GENERIC_FNS) return;
    for (int i = 0; i < g_generic_fn_count; i++)
        if (strcmp(g_generic_fns[i].name, name) == 0) return;
    g_generic_fns[g_generic_fn_count].name = name;
    g_generic_fns[g_generic_fn_count].def = def;
    g_generic_fn_count++;
}

Re0Stmt *find_generic_fn(const char *name) {
    for (int i = 0; i < g_generic_fn_count; i++)
        if (strcmp(g_generic_fns[i].name, name) == 0) return g_generic_fns[i].def;
    return NULL;
}

bool is_already_instantiated(const char *mangled) {
    for (int i = 0; i < g_instantiated_count; i++)
        if (strcmp(g_instantiated[i].name, mangled) == 0) return true;
    return false;
}

void mark_instantiated(const char *mangled) {
    if (g_instantiated_count >= MAX_INSTANTIATED) return;
    snprintf(g_instantiated[g_instantiated_count].name, sizeof(g_instantiated[0].name), "%s", mangled);
    g_instantiated_count++;
}

/* type substitution: look up orig in type_params, return args[i] if found */
const char *substitute_one(const char *orig,
                                   char **params, char **args, int n) {
    if (!orig) return NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(orig, params[i]) == 0) return args[i];
    return orig;
}

/* forward declaration */
void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth);

/* instantiate generic function: record pending + emit forward declaration */
void instantiate_generic_fn(Re0Codegen *c, Re0Stmt *def,
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
void flush_pending_instantiations(Re0Codegen *c) {
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
void flush_lambdas(Re0Codegen *c) {
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
const char *try_instantiate_generic_call(Re0Codegen *c, const char *fn_name,
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

/*  */

