#include "backend/backend_c_internal.h"

RE0_THREAD_LOCAL Re0CVarType var_types[MAX_VAR_TYPES];
RE0_THREAD_LOCAL int var_type_count;
RE0_THREAD_LOCAL FnRetSlot g_fn_rets[MAX_FN_RETS];
RE0_THREAD_LOCAL int g_fn_ret_count;
RE0_THREAD_LOCAL StructFieldSlot g_struct_fields[MAX_STRUCT_FIELDS];
RE0_THREAD_LOCAL int g_struct_field_count;
RE0_THREAD_LOCAL LambdaSlot g_lambdas[MAX_LAMBDAS];
RE0_THREAD_LOCAL int g_lambda_count, g_lambda_counter;
RE0_THREAD_LOCAL size_t g_fwd_insert_pos;
RE0_THREAD_LOCAL GenericStructSlot g_generic_structs[MAX_GENERIC_STRUCTS];
RE0_THREAD_LOCAL int g_generic_struct_count;
RE0_THREAD_LOCAL GenericFnSlot g_generic_fns[MAX_GENERIC_FNS];
RE0_THREAD_LOCAL int g_generic_fn_count;
RE0_THREAD_LOCAL InstantiatedSlot g_instantiated[MAX_INSTANTIATED];
RE0_THREAD_LOCAL int g_instantiated_count;
RE0_THREAD_LOCAL PendingInst g_pending_list[MAX_INSTANTIATED];
RE0_THREAD_LOCAL int g_pending_count;

static bool copy_type(char *out, size_t size, const char *value) {
    if (!out || !size || !value) return false;
    int n = snprintf(out, size, "%s", value);
    if (n < 0 || (size_t)n >= size) { c_storage_fail("C type spelling exceeds capacity"); return false; }
    return true;
}
void track_var(const char *name, const char *type) {
    if (var_type_count == MAX_VAR_TYPES) { c_storage_fail("variable tracking limit exceeded"); return; }
    Re0CVarType *v = &var_types[var_type_count++];
    v->name = strdup(name);
    if (!v->name) { c_storage_fail("cannot allocate variable name"); return; }
    copy_type(v->c_type, sizeof(v->c_type), type);
    v->is_float = strcmp(type, "float") == 0 || strcmp(type, "double") == 0;
    v->is_string = strcmp(type, "const char*") == 0;
}
const char *var_c_type(const char *name) {
    for (int i = var_type_count; i > 0; i--)
        if (var_types[i - 1].name && strcmp(var_types[i - 1].name, name) == 0) return var_types[i - 1].c_type;
    return NULL;
}
bool var_is_float(const char *name) { const char *t = var_c_type(name); return t && (strcmp(t,"float") == 0 || strcmp(t,"double") == 0); }
bool var_is_string(const char *name) { const char *t = var_c_type(name); return t && strcmp(t,"const char*") == 0; }
void clear_var_types(void) { for (int i = 0; i < var_type_count; i++) free(var_types[i].name); var_type_count = 0; }
const char *reo_type_to_c(const char *name) { return c_storage_text(name); }
void track_fn_ret(const char *name, const char *ret) {
    for (int i = 0; i < g_fn_ret_count; i++) if (strcmp(g_fn_rets[i].name, name) == 0) return;
    if (g_fn_ret_count == MAX_FN_RETS) { c_storage_fail("function tracking limit exceeded"); return; }
    FnRetSlot *f = &g_fn_rets[g_fn_ret_count++];
    copy_type(f->name, sizeof(f->name), name); copy_type(f->ret_c_type, sizeof(f->ret_c_type), reo_type_to_c(ret));
}
const char *fn_ret_c_type(const char *name) {
    for (int i = 0; i < g_fn_ret_count; i++) if (strcmp(g_fn_rets[i].name, name) == 0) return g_fn_rets[i].ret_c_type;
    return NULL;
}
void track_struct_field(const char *owner, const char *field, const char *type) {
    for (int i = 0; i < g_struct_field_count; i++)
        if (strcmp(g_struct_fields[i].struct_name,owner) == 0 && strcmp(g_struct_fields[i].field,field) == 0) return;
    if (g_struct_field_count == MAX_STRUCT_FIELDS) { c_storage_fail("field tracking limit exceeded"); return; }
    StructFieldSlot *f = &g_struct_fields[g_struct_field_count++];
    copy_type(f->struct_name,sizeof(f->struct_name),owner); copy_type(f->field,sizeof(f->field),field);
    copy_type(f->c_type,sizeof(f->c_type),reo_type_to_c(type));
}
const char *struct_field_c_type(const char *owner, const char *field) {
    for (int i = 0; i < g_struct_field_count; i++)
        if (strcmp(g_struct_fields[i].struct_name,owner) == 0 && strcmp(g_struct_fields[i].field,field) == 0) return g_struct_fields[i].c_type;
    return NULL;
}
bool builtin_returns_string(const char *f) {
    const char *names[] = {"str_concat","str_slice","char_to_str","to_string","file_read","argv_get","stdin_read","svec_get","dir_next","path_join","path_ext","path_base"};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) if (strcmp(f,names[i])==0) return true;
    return false;
}
bool builtin_returns_float(const char *f) { (void)f; return false; }
bool builtin_returns_vec(const char *f) { return strcmp(f,"vec_new")==0; }
bool builtin_returns_svec(const char *f) { return strcmp(f,"svec_new")==0; }

bool infer_expr_c_type(Re0Expr *e, char *out, size_t size) {
    if (!e || !out || !size) return false;
    char left[128], right[128];
    if (e->kind == EXPR_IDENT && e->ident.name) {
        const char *known = var_c_type(e->ident.name);
        if (known) return copy_type(out,size,known);
    }
    if (e->kind == EXPR_STRUCT_INIT) {
        if (find_generic_struct(e->struct_init.name) && e->struct_init.field_count &&
            infer_expr_c_type(e->struct_init.fields[0].value,left,sizeof(left)))
            return copy_type(out,size,c_storage_generic(e->struct_init.name,left));
        return copy_type(out,size,reo_type_to_c(e->struct_init.name));
    }
    if (e->kind == EXPR_SELECT && infer_expr_c_type(e->select.object,left,sizeof(left))) {
        const CStorageType *p=c_storage_find(left);
        const char *field = struct_field_c_type(p && p->kind==C_STORAGE_POINTER?p->element:left,e->select.field);
        if (field) return copy_type(out,size,field);
    }
    if(e->kind==EXPR_UNARY && infer_expr_c_type(e->unary.operand,left,sizeof(left))) {
        if(e->unary.op==UNOP_REF && e->resolved_type && e->resolved_type->kind==RE0_TYPE_FN)
            return copy_type(out,size,c_storage_type(e->resolved_type));
        if(e->unary.op==UNOP_REF || e->unary.op==UNOP_REFMUT)
            return copy_type(out,size,c_storage_pointer(left,e->unary.op==UNOP_REFMUT));
        if(e->unary.op==UNOP_DEREF) {
            const CStorageType *pointer=c_storage_find(left);
            if(pointer && pointer->kind==C_STORAGE_POINTER)return copy_type(out,size,pointer->element);
        }
    }
    if (e->resolved_type && e->resolved_type->kind != RE0_TYPE_UNKNOWN && e->resolved_type->kind != RE0_TYPE_TYPEVAR) {
        /* Concrete local tracking above takes precedence during specialization. */
        return copy_type(out,size,c_storage_type(e->resolved_type));
    }
    switch (e->kind) {
        case EXPR_INT: return copy_type(out,size,reo_type_to_c(e->int_lit.suffix ? e->int_lit.suffix : "i64"));
        case EXPR_FLOAT: return copy_type(out,size,reo_type_to_c(e->float_lit.suffix ? e->float_lit.suffix : "f64"));
        case EXPR_BOOL: return copy_type(out,size,"bool");
        case EXPR_CHAR: return copy_type(out,size,"uint8_t");
        case EXPR_STRING: return copy_type(out,size,"const char*");
        case EXPR_CALL:
            if (e->call.callee && e->call.callee->kind == EXPR_IDENT) {
                const char *fn = e->call.callee->ident.name, *ret = fn_ret_c_type(fn);
                if (ret) return copy_type(out,size,ret);
                Re0Stmt *generic = find_generic_fn(fn);
                if (generic && generic->function.type_param_count==1 && e->call.arg_count>0 &&
                    generic->function.ret_type && strcmp(generic->function.ret_type,generic->function.type_params[0])==0)
                    return infer_expr_c_type(e->call.args[0],out,size);
                if (builtin_returns_string(fn)) return copy_type(out,size,"const char*");
                if (builtin_returns_vec(fn) || builtin_returns_svec(fn)) return copy_type(out,size,reo_type_to_c(builtin_returns_vec(fn)?"vec":"svec"));
            }
            return false;
        case EXPR_ARRAY: case EXPR_ARRAY_REPEAT:
            if (e->kind == EXPR_ARRAY && e->array.count > 0 && infer_expr_c_type(e->array.elems[0],left,sizeof(left)))
                return copy_type(out,size,c_storage_sequence(left,(size_t)e->array.count,false));
            if (e->kind == EXPR_ARRAY_REPEAT && infer_expr_c_type(e->array_repeat.value,left,sizeof(left))) {
                bool fixed = e->array_repeat.count && e->array_repeat.count->kind == EXPR_INT;
                return copy_type(out,size,c_storage_sequence(left,fixed?(size_t)e->array_repeat.count->int_lit.val:0,!fixed));
            }
            return false;
        case EXPR_INDEX:
            if (infer_expr_c_type(e->index.target,left,sizeof(left))) {
                const CStorageType *t = c_storage_find(left);
                if (t && (t->kind == C_STORAGE_ARRAY || t->kind == C_STORAGE_SLICE || t->kind == C_STORAGE_VECTOR)) return copy_type(out,size,t->element);
            }
            return false;
        case EXPR_BINARY:
            if (e->binary.op >= BINOP_EQ && e->binary.op <= BINOP_OR) return copy_type(out,size,"bool");
            if (infer_expr_c_type(e->binary.left,left,sizeof(left)) && infer_expr_c_type(e->binary.right,right,sizeof(right))) {
                if (strcmp(left,"const char*")==0 || strcmp(right,"const char*")==0) return copy_type(out,size,"const char*");
                if (strcmp(left,"double")==0 || strcmp(right,"double")==0) return copy_type(out,size,"double");
                if (strcmp(left,"float")==0 || strcmp(right,"float")==0) return copy_type(out,size,"float");
                const char *wide = c_wider_int_type(left,right);
                return wide && copy_type(out,size,wide);
            }
            return false;
        default: return false;
    }
}

bool c_storage_expr_sequence(Re0Expr *e, char *name, size_t size) {
    if (!infer_expr_c_type(e,name,size)) return false;
    const CStorageType *t = c_storage_find(name);
    return t && (t->kind == C_STORAGE_ARRAY || t->kind == C_STORAGE_SLICE);
}
bool expr_is_array_var(Re0Expr *e) { char name[128]; return c_storage_expr_sequence(e,name,sizeof(name)); }
bool expr_is_vec(Re0Expr *e) { char name[128]; if (!infer_expr_c_type(e,name,sizeof(name))) return false; const CStorageType *t=c_storage_find(name); return t && t->kind==C_STORAGE_VECTOR; }
bool expr_is_pointer_obj(Re0Expr *e) { char name[128]; return infer_expr_c_type(e,name,sizeof(name)) && strlen(name)>0 && name[strlen(name)-1]=='*'; }
bool expr_is_float(Re0Expr *e) { char name[128]; return infer_expr_c_type(e,name,sizeof(name)) && (strcmp(name,"float")==0 || strcmp(name,"double")==0); }
bool expr_is_string(Re0Expr *e) { char name[128]; return infer_expr_c_type(e,name,sizeof(name)) && strcmp(name,"const char*")==0; }
bool expr_is_i128(Re0Expr *e) { char name[128]; return infer_expr_c_type(e,name,sizeof(name)) && strcmp(name,"__int128")==0; }
bool expr_is_u128(Re0Expr *e) { char name[128]; return infer_expr_c_type(e,name,sizeof(name)) && strcmp(name,"unsigned __int128")==0; }
const char *c_wider_int_type(const char *a, const char *b) {
    static const char *types[]={"int8_t","uint8_t","int16_t","uint16_t","int32_t","uint32_t","int64_t","uint64_t","__int128","unsigned __int128"};
    int ia=-1,ib=-1;
    for(int i=0;i<10;i++){if(strcmp(a,types[i])==0)ia=i;if(strcmp(b,types[i])==0)ib=i;}
    return ia<0 || ib<0 ? NULL : types[ia>ib?ia:ib];
}
const char *binop_c(Re0BinOpKind op) {
    static const char *ops[]={"+","-","*","/","%","==","!=","<","<=",">",">=","&&","||","&","|","^","?","<<",">>"};
    return op>=BINOP_ADD && op<=BINOP_SHR ? ops[op] : "?";
}
bool split_qualified(const char *name, char *owner, int osize, char *variant, int vsize) {
    if (!name || !owner || !variant || osize<=0 || vsize<=0) return false;
    const char *colon=strstr(name,"::"); if (!colon) return false;
    size_t n=(size_t)(colon-name),v=strlen(colon+2);
    if(n>=(size_t)osize || v>=(size_t)vsize) { c_storage_fail("qualified name is too long"); return false; }
    memcpy(owner,name,n);owner[n]=0;memcpy(variant,colon+2,v+1);return true;
}
void c_write_string_literal(Re0Buffer *b, const char *value) {
    re0_buffer_write_char(b,'"');
    for (const unsigned char *p=(const unsigned char *)(value?value:""); *p; p++) {
        if (*p=='\\' || *p=='"') {re0_buffer_write_char(b,'\\');re0_buffer_write_char(b,(char)*p);}
        else if(*p<32 || *p==127) re0_buffer_write_fmt(b,"\\%03o",(unsigned)*p);
        else re0_buffer_write_char(b,(char)*p);
    }
    re0_buffer_write_char(b,'"');
}
void c_write_char_literal(Re0Buffer *b, char value) { re0_buffer_write_fmt(b,"((uint8_t)%u)",(unsigned)(unsigned char)value); }
