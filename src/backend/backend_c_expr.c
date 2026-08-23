#include "backend/backend_c_internal.h"

/* expression code generation (c_gen_expr) and all expression forms. */

int c_gen_expr(Re0Codegen *c, Re0Expr *e) {
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
            /* generic call detection: infer type args, instantiate, call mangled name */
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
            /* expr?: GCC statement expression
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

