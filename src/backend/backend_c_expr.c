#include "backend/backend_c_internal.h"

/* expression code generation (c_gen_expr) and all expression forms. */

int c_gen_expr(Re0Codegen *c, Re0Expr *e) {
    Re0Buffer *b = &c->output;
    if (!e) { re0_buffer_write_str(b, "(void)0"); return 0; }
    switch (e->kind) {
        case EXPR_INT: {
            Re0Integer value = e->int_lit.integer;
            Re0TypeKind kind = e->resolved_type ? e->resolved_type->kind : RE0_TYPE_I64;
            const char *type = reo_type_to_c(re0_type_kind_name(kind));
            if (value.high || !re0_integer_fits(value, 64, true)) {
                re0_buffer_write_fmt(b, "((%s)(", type);
                if (value.negative) re0_buffer_write_str(b, "-1 - (__reo_i128)(");
                if (value.negative) {
                    if (value.low == 0) value.high--;
                    value.low--;
                }
                re0_buffer_write_fmt(b, "(((__reo_u128)%lluULL << 64) | (__reo_u128)%lluULL)",
                    (unsigned long long)value.high, (unsigned long long)value.low);
                if (e->int_lit.integer.negative) re0_buffer_write_char(b, ')');
                re0_buffer_write_str(b, "))");
            } else if (e->int_lit.val == INT64_MIN) {
                re0_buffer_write_fmt(b, "((%s)(-9223372036854775807LL - 1))", type);
            } else re0_buffer_write_fmt(b, "((%s)%lldLL)", type, (long long)e->int_lit.val);
            break;
        }
        case EXPR_FLOAT:
            if (e->resolved_type && e->resolved_type->kind == RE0_TYPE_F32)
                re0_buffer_write_fmt(b, "__reo_conv_f32(%a)", e->float_lit.val);
            else re0_buffer_write_fmt(b, "((double)%a)", e->float_lit.val);
            break;
        case EXPR_BOOL: re0_buffer_write_str(b, e->bool_lit.val ? "1" : "0"); break;
        case EXPR_CHAR: re0_buffer_write_fmt(b, "((uint8_t)%u)", (unsigned char)e->char_lit.val); break;
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
            Re0TypeKind result_kind = c_expr_scalar_kind(e);
            int operation = op == BINOP_ADD ? 0 : op == BINOP_SUB ? 1 : op == BINOP_MUL ? 2 :
                op == BINOP_DIV ? 3 : op == BINOP_MOD ? 4 : op == BINOP_SHL ? 5 : op == BINOP_SHR ? 6 : -1;
            if (operation >= 0 && re0_type_is_integer(result_kind)) {
                const char *type = reo_type_to_c(re0_type_kind_name(result_kind));
                bool sign = re0_type_is_signed(result_kind);
                re0_buffer_write_fmt(b, "((%s)(%s__reo_conv_binary((__reo_u128)(", type,
                                        sign ? "__reo_conv_signed(" : "");
                c_gen_expr(c, e->binary.left);
                re0_buffer_write_str(b, "), (__reo_u128)(");
                c_gen_expr(c, e->binary.right);
                re0_buffer_write_fmt(b, "), sizeof(%s) * CHAR_BIT, %s, %d)", type,
                                        sign ? "true" : "false", operation);
                if (sign) re0_buffer_write_fmt(b, ", sizeof(%s) * CHAR_BIT)", type);
                re0_buffer_write_str(b, "))");
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
            Re0TypeKind kind = c_expr_scalar_kind(e);
            if (e->unary.op == UNOP_NEG && re0_type_is_integer(kind)) {
                const char *type = reo_type_to_c(re0_type_kind_name(kind));
                bool sign = re0_type_is_signed(kind);
                re0_buffer_write_fmt(b, "((%s)(%s((__reo_u128)0 - (__reo_u128)(", type,
                                        sign ? "__reo_conv_signed(" : "");
                c_gen_expr(c, e->unary.operand);
                re0_buffer_write_str(b, "))");
                if (sign) re0_buffer_write_fmt(b, ", sizeof(%s) * CHAR_BIT)", type);
                re0_buffer_write_str(b, "))");
                break;
            }
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
            /* method sugar: x.len() -> str_len(x) or vec_len(x) */
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
            /* method call: obj.method(args) -> MangledSymbol(obj, args...) */
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
                    else if (expr_is_u128(arg) || expr_is_i128(arg)) {
                        re0_buffer_write_fmt(b, "printf(\"%%s%s\", %s(",
                            strcmp(fn, "println") == 0 ? "\\n" : "",
                            expr_is_u128(arg) ? "__reo_conv_unsigned_string" : "__reo_conv_signed_string");
                        c_gen_expr(c, arg);
                        re0_buffer_write_str(b, ")), fflush(stdout)");
                        break;
                    }
                    else if (re0_type_is_integer(c_expr_scalar_kind(arg)) &&
                             !re0_type_is_signed(c_expr_scalar_kind(arg))) {
                        fmt = "%llu"; cast = "(unsigned long long)";
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
                if ((strcmp(fn, "str_to_int") == 0 || strcmp(fn, "to_string") == 0 ||
                     strcmp(fn, "char_to_str") == 0) && e->call.arg_count == 1) {
                    Re0Type target = {0};
                    target.kind = strcmp(fn, "str_to_int") == 0 ? RE0_TYPE_I64 : RE0_TYPE_STR;
                    Re0Expr conversion = {0};
                    conversion.kind = EXPR_CAST;
                    conversion.span = e->span;
                    conversion.resolved_type = &target;
                    conversion.cast.inner = e->call.args[0];
                    Re0Expr byte_conversion = {0};
                    Re0Type byte_type = {0};
                    if (strcmp(fn, "char_to_str") == 0) {
                        byte_type.kind = RE0_TYPE_CHAR;
                        byte_conversion.kind = EXPR_CAST;
                        byte_conversion.span = e->span;
                        byte_conversion.resolved_type = &byte_type;
                        byte_conversion.cast.inner = e->call.args[0];
                        byte_conversion.cast.target_type = "char";
                        conversion.cast.inner = &byte_conversion;
                    }
                    conversion.cast.target_type = (char*)re0_type_kind_name(target.kind);
                    return c_gen_cast(c, &conversion);
                }
                /* array length: works for both fat-pointer array kinds */
                if (strcmp(fn, "len") == 0 && e->call.arg_count == 1) {
                    char at[128];
                    if (infer_expr_c_type(e->call.args[0], at, sizeof(at)) &&
                        (strcmp(at, "__reo_arr_t") == 0 || strcmp(at, "__reo_arrf_t") == 0 || strcmp(at, "__reo_arr128_t") == 0)) {
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
                if (strcmp(fn, "is_digit") == 0)   { re0_buffer_write_str(b, "__reo_is_digit((char)"); goto gen1; }
                if (strcmp(fn, "is_alpha") == 0)   { re0_buffer_write_str(b, "__reo_is_alpha((char)"); goto gen1; }
                if (strcmp(fn, "is_alnum") == 0)   { re0_buffer_write_str(b, "__reo_is_alnum((char)"); goto gen1; }
                if (strcmp(fn, "free") == 0)       { re0_buffer_write_str(b, "(__reo_gc_free((void*)"); goto gen1; }
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
                    /* spawn f() -> __reo_rt_spawn(&f) */
                    re0_buffer_write_str(b, "__reo_rt_spawn(&");
                    c_gen_expr(c, e->call.args[0]);
                    re0_buffer_write_char(b, ')');
                    break;
                }
                if (strcmp(fn, "__reo_await") == 0 && e->call.arg_count >= 1) {
                    /* await task: GCC stmt expr */
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
            /* generic call detection: infer type args, instantiate, call mangled name.
             * Callee may carry turbofish form "name<type>" (parser rewrite):
             * split it off and pass the explicit type to instantiation. */
            if (e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name;
                char base[128]; const char *explicit_ty = NULL;
                const char *lt = strchr(fn, '<');
                if (lt && strchr(lt, '>')) {
                    size_t blen = (size_t)(lt - fn);
                    if (blen < sizeof(base)) {
                        memcpy(base, fn, blen); base[blen] = '\0';
                        fn = base;
                        explicit_ty = lt + 1;
                        char *gt = strchr(base, 0); /* unused; strip below */
                        (void)gt;
                        /* extract type text between < and > into a local */
                        static char tybuf[8][128]; static int tyseq = 0;
                        int slot = tyseq++ % 8;
                        const char *ty2 = lt + 1;
                        const char *end2 = strchr(ty2, '>');
                        size_t tl = end2 ? (size_t)(end2 - ty2) : 0;
                        if (tl >= sizeof(tybuf[0])) tl = sizeof(tybuf[0]) - 1;
                        memcpy(tybuf[slot], ty2, tl); tybuf[slot][tl] = '\0';
                        explicit_ty = tybuf[slot];
                    }
                }
                char mangled_buf[256];
                const char *mangled = explicit_ty
                    ? NULL /* handled below */
                    : try_instantiate_generic_call(
                        c, fn, e->call.args, e->call.arg_count, mangled_buf, sizeof(mangled_buf));
                if (!mangled && explicit_ty) {
                    /* explicit type: build mangled name directly. Only record
                     * the pending instantiation — c_end emits the body via
                     * flush_pending_instantiations (calling it here would
                     * splice the function body into the current statement). */
                    Re0Stmt *def = find_generic_fn(fn);
                    if (def) {
                        snprintf(mangled_buf, sizeof(mangled_buf), "%s_%s", fn, explicit_ty);
                        if (!is_already_instantiated(mangled_buf)) {
                            char *targs[1]; targs[0] = (char*)explicit_ty;
                            instantiate_generic_fn(c, def, targs, 1);
                        }
                        mangled = mangled_buf;
                    }
                }
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
            /* expr?: GCC statement expression
             * Option: None=tag0 early return; Some=tag1 take payload
             * Result: Err=tag1 early return;  Ok=tag0 take payload
             * distinguish Option/Result by inner expression type (default Option if inference fails) */
            int t = c->temp_counter++;
            char inner_type[128] = {0};
            if (infer_expr_c_type(e->try_.inner, inner_type, sizeof(inner_type)) &&
                strncmp(inner_type, "__reo_result_", 13) == 0) {
                re0_buffer_write_fmt(b, "({ %s __t%d = (", inner_type, t);
                c_gen_expr(c, e->try_.inner);
                Re0Expr result_expression = {0};
                result_expression.resolved_type = e->try_.return_type;
                char result_type[128];
                if (!infer_expr_c_type(&result_expression, result_type, sizeof(result_type))) {
                    c->had_error = true;
                    re0_error_append(c->errors, RE0_ERR_SEMANTIC, e->span, NULL, "unsupported Result propagation layout");
                    break;
                }
                re0_buffer_write_fmt(b, "); if (__t%d.tag) return (%s){ .tag=1, .error=__t%d.error, .index=__t%d.index }; __t%d.value; })",
                                     t, result_type, t, t, t);
                break;
            }
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
            char array_type[128];
            bool wide = infer_expr_c_type(e, array_type, sizeof(array_type)) && strcmp(array_type, "__reo_arr128_t") == 0;
            const char *dup = wide ? "__reo_arr128_lit((const __reo_u128[]){" : is_float ? "__reo_arrf_lit((const double[]){" : "__reo_arr_lit((const int64_t[]){";
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
            char array_type[128];
            bool wide = infer_expr_c_type(e, array_type, sizeof(array_type)) && strcmp(array_type, "__reo_arr128_t") == 0;
            const char *fn = wide ? "__reo_arr128_rep" : rep_float ? "__reo_arrf_rep" : "__reo_arr_rep";
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
            bool wide = base_known && strcmp(base, "__reo_arr128_t") == 0;
            if (base_known && (strcmp(base, "__reo_arr_t") == 0 || dbl || wide)) {
                Re0TypeKind element = e->resolved_type ? e->resolved_type->kind : RE0_TYPE_I64;
                bool signed_wide = wide && element == RE0_TYPE_I128;
                int temporary = c->temp_counter++;
                re0_buffer_write_fmt(b, "({ %s __array%d = (", base, temporary);
                c_gen_expr(c, e->index.target);
                re0_buffer_write_fmt(b, "); ((%s)(%s", reo_type_to_c(re0_type_kind_name(element)), signed_wide ? "__reo_conv_signed(" : "");
                re0_buffer_write_fmt(b, "%s(&__array%d, (int64_t)(", wide ? "__reo_arr128_get" : dbl ? "__reo_arrf_get" : "__reo_arr_get", temporary);
                c_gen_expr(c, e->index.index);
                re0_buffer_write_str(b, "))");
                if (signed_wide) re0_buffer_write_str(b, ", 128)");
                re0_buffer_write_str(b, ")); })");
            } else if (base_known && strcmp(base, "const char*") == 0) {
                re0_buffer_write_str(b, "__reo_str_char_at(");
                c_gen_expr(c, e->index.target);
                re0_buffer_write_str(b, ", ");
                c_gen_expr(c, e->index.index);
                re0_buffer_write_char(b, ')');
            } else {
                /* Inline C arrays retain their native representation. */
                re0_buffer_write_char(b, '(');
                c_gen_expr(c, e->index.target);
                re0_buffer_write_char(b, '[');
                c_gen_expr(c, e->index.index);
                re0_buffer_write_str(b, "])");
            }
            break;
        }
        case EXPR_CAST: return c_gen_cast(c, e);
        default: re0_buffer_write_str(b, "0"); break;
    }
    return 0;
}

