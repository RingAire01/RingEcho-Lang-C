#include "backend/backend_c_internal.h"

/* statement code generation + backend state reset. */

void c_gen_body(Re0Codegen *c, Re0Stmt **body, int count, int depth) {
    int saved=var_type_count;
    for (int i = 0; i < count; i++) c->backend->gen_stmt(c, body[i], depth);
    while(var_type_count>saved) free(var_types[--var_type_count].name);
}

void c_gen_extern_decl(Re0Codegen *c, const Re0ExternFnDecl *decl) {
    Re0Buffer *b = &c->output;
    const char *return_type = c_storage_return(decl->ret_type);
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

void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth) {
    Re0Buffer *b = &c->output;
    if (!s) return;
    switch (s->kind) {
        case STMT_LET: {
            re0_buffer_write_indent(b, depth);
            /* determine C type */
            const char *ctype = NULL;
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
                else if (s->let_stmt.type)
                    ctype = reo_type_to_c(s->let_stmt.type);
                else if (infer_expr_c_type(s->let_stmt.init, inferred_type, sizeof(inferred_type)))
                    ctype = inferred_type;
            } else if (s->let_stmt.type)
                ctype = reo_type_to_c(s->let_stmt.type);
            else if (s->let_stmt.init &&
                     infer_expr_c_type(s->let_stmt.init, inferred_type,
                                       sizeof(inferred_type)))
                ctype = inferred_type;
            if (!ctype) {
                re0_error_append(c->errors, RE0_ERR_SEMANTIC, s->span, NULL,
                                 "C backend cannot represent local type '%s'",
                                 s->let_stmt.type ? s->let_stmt.type : "unknown");
                c->had_error = true;
                break;
            }
            re0_buffer_write_fmt(b, "%s %s", ctype, s->let_stmt.name);
            if (s->let_stmt.init) {
                re0_buffer_write_str(b, " = ");
                c_gen_expr(c, s->let_stmt.init);
            } else re0_buffer_write_str(b," = {0}");
            re0_buffer_write_str(b, ";\n");
            track_var(s->let_stmt.name, ctype);
            break;
        }
        case STMT_CONST:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_fmt(b, "#define %s (", s->const_decl.name);
            if (s->const_decl.type) re0_buffer_write_fmt(b, "(%s)(", reo_type_to_c(s->const_decl.type));
            c_gen_expr(c, s->const_decl.value);
            if (s->const_decl.type) re0_buffer_write_char(b, ')');
            re0_buffer_write_str(b, ")\n");
            break;
        case STMT_TYPE_ALIAS:
            re0_buffer_write_fmt(b, "typedef %s %s;\n",
                                reo_type_to_c(s->type_alias.target),
                                s->type_alias.name);
            break;
        case STMT_ASSIGN:
            re0_buffer_write_indent(b, depth);
            re0_buffer_write_fmt(b, "%s = ", s->assign.name);
            if (s->assign.op == BINOP_ASSIGN_SENTINEL) {
                c_gen_expr(c, s->assign.value);
            } else {
                /* Reuse binary lowering so += etc. preserve wrapping and
                 * checked division semantics instead of discarding the op. */
                Re0Expr left = {.kind = EXPR_IDENT, .span = s->span};
                left.ident.name = s->assign.name;
                Re0Type result = {.kind = c_expr_scalar_kind(&left)};
                Re0Expr binary = {.kind = EXPR_BINARY, .span = s->span,
                                  .resolved_type = &result};
                binary.binary.op = s->assign.op;
                binary.binary.left = &left;
                binary.binary.right = s->assign.value;
                c_gen_expr(c, &binary);
            }
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
            c_gen_sequence_set(c,s);
            break;
        case STMT_STORE:
            re0_buffer_write_indent(b,depth);c_gen_store(c,s->store.target,s->store.value,s->store.op);break;
        case STMT_EXPR:
            re0_buffer_write_indent(b, depth);
            c_gen_expr(c, s->expr_stmt.expr);
            re0_buffer_write_str(b, ";\n");
            break;
        case STMT_RETURN:
            re0_buffer_write_indent(b, depth);
            if(c->c_return_void) {
                if(s->return_stmt.value){re0_buffer_write_str(b,"(void)(");c_gen_expr(c,s->return_stmt.value);re0_buffer_write_str(b,"); ");}
                re0_buffer_write_str(b,"return;\n");break;
            }
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
            int saved_variables=var_type_count;
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
            /* string iteration: for ch in s - byte by byte */
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
            /* Containers retain their concrete element type. */
            else if (iter && expr_is_vec(iter)) {
                int t = c->temp_counter++;
                char type[128]; infer_expr_c_type(iter,type,sizeof(type));
                const CStorageType *v=c_storage_find(type);
                re0_buffer_write_fmt(b, "{ %s __v%d = ",type,t);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b, "; for (int64_t __i%d = 0; __i%d < __v%d->len; __i%d++) {\n",
                                     t, t, t, t);
                re0_buffer_write_fmt(b, "%s %s = __v%d->data[__i%d];\n",v->element,s->for_stmt.var,t,t);
                track_var(s->for_stmt.var,v->element);
            }
            else if (iter && expr_is_array_var(iter)) {
                int t = c->temp_counter++;
                char at[128];
                infer_expr_c_type(iter,at,sizeof(at));
                const CStorageType *seq=c_storage_find(at);
                re0_buffer_write_fmt(b, "{ %s __a%d = ",at,t);
                c_gen_expr(c, iter);
                re0_buffer_write_fmt(b,"; for (size_t __i%d=0; __i%d < ",t,t);
                if(seq->kind==C_STORAGE_ARRAY) re0_buffer_write_fmt(b,"%zu",seq->length);
                else re0_buffer_write_fmt(b,"__a%d.len",t);
                re0_buffer_write_fmt(b,"; __i%d++) {\n%s %s = __a%d.data[__i%d];\n",t,seq->element,s->for_stmt.var,t,t);
                track_var(s->for_stmt.var,seq->element);
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
            while(var_type_count>saved_variables) free(var_types[--var_type_count].name);
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
            const char *ret_c = c_storage_return(s->function.ret_type);
            re0_buffer_write_fmt(b, "%s %s(", ret_c, fn_name);
            if(!s->function.param_count) re0_buffer_write_str(b,"void");
            for (int i = 0; i < s->function.param_count; i++) {
                if (i > 0) re0_buffer_write_str(b, ", ");
                re0_buffer_write_fmt(b, "%s %s",
                                    reo_type_to_c(s->function.params[i].ptype),
                                    s->function.params[i].name);
            }
            re0_buffer_write_str(b, ") {\n");
            /* recursion depth guard: bounded user recursion instead of a
             * native stack overflow (RAII cleanup unwinds on every return). */
            re0_buffer_write_str(b, "    __REO_DEPTH_GUARD;\n");
            /* track param types */
            clear_var_types();
            for (int i = 0; i < s->function.param_count; i++)
                track_var(s->function.params[i].name, reo_type_to_c(s->function.params[i].ptype));
            bool previous_return_void=c->c_return_void;c->c_return_void=strcmp(ret_c,"void")==0;
            c_gen_body(c, s->function.body, s->function.body_count, 1);
            c->c_return_void=previous_return_void;
            re0_buffer_write_str(b, "}\n\n");
            break;
        }
        case STMT_STRUCT:
            /* generic struct: register and skip, wait for instantiation */
            if (s->struct_decl.type_param_count > 0) {
                register_generic_struct(s->struct_decl.name, s);
                break;
            }
            (void)reo_type_to_c(s->struct_decl.name);
            break;
        case STMT_ENUM:
            (void)reo_type_to_c(s->enum_decl.name);
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
                snprintf(self_ptr_type, sizeof(self_ptr_type), "&mut %s", s->impl.name);
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
            (void)reo_type_to_c(s->component.name);
            /* set self param type + generate method with mangled name (same as STMT_IMPL) */
            for (int i = 0; i < s->component.method_count; i++) {
                Re0Stmt *m = s->component.methods[i];
                if (!m || m->kind != STMT_FUNCTION) continue;
                char self_ptr_type[160];
                snprintf(self_ptr_type, sizeof(self_ptr_type), "&mut %s", s->component.name);
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

void reset_c_state(void) {
    clear_var_types();
    g_fn_ret_count = 0;
    g_struct_field_count = 0;
    g_lambda_count = 0;
    g_lambda_counter = 0;
    g_generic_struct_count = 0;
    g_generic_fn_count = 0;
    g_instantiated_count = 0;
    g_pending_count = 0;
}
