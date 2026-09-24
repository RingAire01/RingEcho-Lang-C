#include "backend/backend_c_internal.h"

void register_generic_struct(const char *name, Re0Stmt *def) {
    for(int i=0;i<g_generic_struct_count;i++)if(strcmp(g_generic_structs[i].name,name)==0)return;
    if(g_generic_struct_count==MAX_GENERIC_STRUCTS){c_storage_fail("generic struct limit exceeded");return;}
    g_generic_structs[g_generic_struct_count++]=(GenericStructSlot){name,def};
}
Re0Stmt *find_generic_struct(const char *name) {
    for(int i=0;i<g_generic_struct_count;i++)if(strcmp(g_generic_structs[i].name,name)==0)return g_generic_structs[i].def;
    return NULL;
}
const char *instantiate_generic_struct(Re0Codegen *c,const char *base,const char *argument,char *out,size_t size) {
    (void)c;
    int n=snprintf(out,size,"%s",c_storage_generic(base,argument));
    if(n<0 || (size_t)n>=size)c_storage_fail("generic record name exceeds limit");
    return out;
}
const char *try_instantiate_generic_struct_init(Re0Codegen *c,Re0Expr *e,char *out,size_t size) {
    if(!e || e->kind!=EXPR_STRUCT_INIT || !find_generic_struct(e->struct_init.name) || !e->struct_init.field_count)return NULL;
    char argument[128];
    if(!infer_expr_c_type(e->struct_init.fields[0].value,argument,sizeof(argument)))return NULL;
    return instantiate_generic_struct(c,e->struct_init.name,argument,out,size);
}
void register_generic_fn(const char *name,Re0Stmt *def) {
    for(int i=0;i<g_generic_fn_count;i++)if(strcmp(g_generic_fns[i].name,name)==0)return;
    if(g_generic_fn_count==MAX_GENERIC_FNS){c_storage_fail("generic function limit exceeded");return;}
    g_generic_fns[g_generic_fn_count++]=(GenericFnSlot){name,def};
}
Re0Stmt *find_generic_fn(const char *name) {
    for(int i=0;i<g_generic_fn_count;i++)if(strcmp(g_generic_fns[i].name,name)==0)return g_generic_fns[i].def;
    return NULL;
}
bool is_already_instantiated(const char *name) {
    for(int i=0;i<g_instantiated_count;i++)if(strcmp(g_instantiated[i].name,name)==0)return true;
    return false;
}
void mark_instantiated(const char *name) {
    if(g_instantiated_count==MAX_INSTANTIATED){c_storage_fail("generic instance limit exceeded");return;}
    snprintf(g_instantiated[g_instantiated_count++].name,sizeof(g_instantiated[0].name),"%s",name);
}
bool c_generic_mangle(const char *name,char **arguments,int count,char *out,size_t size) {
    static const char hex[]="0123456789abcdef";
    size_t at=0;
    if(!name || count<0 || !out || !size)return false;
    const char *prefix="__reo_inst_";
    for(const char *p=prefix;*p;p++){if(at+1>=size)goto failure;out[at++]=*p;}
    for(int i=-1;i<count;i++) {
        const unsigned char *p=(const unsigned char*)(i<0?name:arguments[i]);
        if(!p)goto failure;
        for(;*p;p++){if(at+2>=size)goto failure;out[at++]=hex[*p>>4];out[at++]=hex[*p&15];}
        if(at+1>=size)goto failure;
        out[at++]='_';
    }
    out[at]=0;return true;
failure:
    out[0]=0;c_storage_fail("generic instance symbol exceeds limit");return false;
}
void instantiate_generic_fn(Re0Codegen *c,Re0Stmt *def,char **arguments,int count) {
    (void)c;
    if(count<=0 || count>8 || count!=def->function.type_param_count){c_storage_fail("invalid generic argument list");return;}
    char name[256];
    if(!c_generic_mangle(def->function.name,arguments,count,name,sizeof(name)))return;
    if(is_already_instantiated(name))return;
    if(g_pending_count==MAX_INSTANTIATED){c_storage_fail("generic instance limit exceeded");return;}
    for(int i=0;i<count;i++)if(strlen(arguments[i])>=sizeof(g_pending_list[0].type_args[0])){c_storage_fail("generic argument spelling exceeds limit");return;}
    mark_instantiated(name);
    PendingInst *p=&g_pending_list[g_pending_count++];memset(p,0,sizeof(*p));
    p->def=def;p->type_arg_count=count;
    snprintf(p->mangled,sizeof(p->mangled),"%s",name);
    for(int i=0;i<count;i++)snprintf(p->type_args[i],sizeof(p->type_args[i]),"%s",arguments[i]);
}

/* Generate definitions before publishing prototypes, so work discovered while
 * lowering a body is included. The source AST is never mutated to specialize. */
static void publish(Re0Codegen *c,Re0Buffer *prototypes,Re0Buffer *bodies) {
    Re0Buffer joined;re0_buffer_init(&joined);
    if(g_fwd_insert_pos>c->output.len || re0_buffer_failed(prototypes) || re0_buffer_failed(bodies)) {
        c_storage_fail("cannot publish generated functions");return;
    }
    re0_buffer_write_n(&joined,c->output.data,g_fwd_insert_pos);
    re0_buffer_write_n(&joined,prototypes->data,prototypes->len);
    re0_buffer_write_n(&joined,c->output.data+g_fwd_insert_pos,c->output.len-g_fwd_insert_pos);
    re0_buffer_write_n(&joined,bodies->data,bodies->len);
    if(re0_buffer_failed(&joined)){re0_buffer_free(&joined);c_storage_fail("cannot allocate generated functions");return;}
    re0_buffer_free(&c->output);c->output=joined;
}

void flush_pending_instantiations(Re0Codegen *c) {
    int first=0;while(first<g_pending_count && g_pending_list[first].emitted)first++;
    if(first==g_pending_count)return;
    Re0Buffer user=c->output;re0_buffer_init(&c->output);
    for(int i=first;i<g_pending_count && !c->had_error;i++) {
        PendingInst *p=&g_pending_list[i];p->emitted=true;
        char *args[8];for(int j=0;j<p->type_arg_count;j++)args[j]=p->type_args[j];
        size_t mark=c_storage_bind(p->def->function.type_params,args,p->type_arg_count);
        Re0Stmt concrete=*p->def;concrete.function.name=p->mangled;concrete.function.type_param_count=0;
        c_gen_stmt(c,&concrete,0);
        c_storage_unbind(mark);
    }
    Re0Buffer bodies=c->output;c->output=user;
    Re0Buffer prototypes;re0_buffer_init(&prototypes);
    for(int i=first;i<g_pending_count && !c->had_error;i++) {
        PendingInst *p=&g_pending_list[i];Re0Stmt *f=p->def;
        char *args[8];for(int j=0;j<p->type_arg_count;j++)args[j]=p->type_args[j];
        size_t mark=c_storage_bind(f->function.type_params,args,p->type_arg_count);
        re0_buffer_write_fmt(&prototypes,"%s %s(",c_storage_return(f->function.ret_type),p->mangled);
        if(!f->function.param_count)re0_buffer_write_str(&prototypes,"void");
        for(int j=0;j<f->function.param_count;j++)re0_buffer_write_fmt(&prototypes,"%s%s",j?", ":"",reo_type_to_c(f->function.params[j].ptype));
        re0_buffer_write_str(&prototypes,");\n");c_storage_unbind(mark);
    }
    if(!c->had_error)publish(c,&prototypes,&bodies);
    re0_buffer_free(&prototypes);re0_buffer_free(&bodies);
}

void flush_lambdas(Re0Codegen *c) {
    int first=0;while(first<g_lambda_count && g_lambdas[first].emitted)first++;
    if(first==g_lambda_count)return;
    Re0Buffer user=c->output;re0_buffer_init(&c->output);
    for(int i=first;i<g_lambda_count && !c->had_error;i++) {
        LambdaSlot *p=&g_lambdas[i];p->emitted=true;
        char *names[8],*args[8];for(int j=0;j<p->binding_count;j++){names[j]=p->bindings[j];args[j]=p->arguments[j];}
        size_t mark=c_storage_bind(names,args,p->binding_count);
        clear_var_types();
        bool unit=strcmp(p->result,"__reo_unit")==0;
        re0_buffer_write_fmt(&c->output,"%s %s(",unit?"void":p->result,p->name);
        if(!p->parameter_count)re0_buffer_write_str(&c->output,"void");
        for(int j=0;j<p->parameter_count;j++) {
            re0_buffer_write_fmt(&c->output,"%s%s %s",j?", ":"",p->parameters[j],p->lambda->lambda.params[j].name);
            track_var(p->lambda->lambda.params[j].name,p->parameters[j]);
        }
        re0_buffer_write_str(&c->output,") { __REO_DEPTH_GUARD; ");
        if(!unit)re0_buffer_write_str(&c->output,"return ");
        c_gen_expr(c,p->lambda->lambda.body);
        re0_buffer_write_str(&c->output,"; }\n");
        c_storage_unbind(mark);
    }
    Re0Buffer bodies=c->output;c->output=user;
    Re0Buffer prototypes;re0_buffer_init(&prototypes);
    for(int i=first;i<g_lambda_count && !c->had_error;i++) {
        LambdaSlot *p=&g_lambdas[i];
        re0_buffer_write_fmt(&prototypes,"%s %s(",strcmp(p->result,"__reo_unit")==0?"void":p->result,p->name);
        if(!p->parameter_count)re0_buffer_write_str(&prototypes,"void");
        for(int j=0;j<p->parameter_count;j++)re0_buffer_write_fmt(&prototypes,"%s%s",j?", ":"",p->parameters[j]);
        re0_buffer_write_str(&prototypes,");\n");
    }
    if(!c->had_error)publish(c,&prototypes,&bodies);
    re0_buffer_free(&prototypes);re0_buffer_free(&bodies);
}

const char *try_instantiate_generic_call(Re0Codegen *c,const char *name,Re0Expr **args,int count,char *out,size_t size) {
    Re0Stmt *def=find_generic_fn(name);
    if(!def || def->function.type_param_count!=1 || count<=0)return NULL;
    char inferred[128];if(!infer_expr_c_type(args[0],inferred,sizeof(inferred)))return NULL;
    char *arguments[]={inferred};instantiate_generic_fn(c,def,arguments,1);
    return c_generic_mangle(name,arguments,1,out,size)?out:NULL;
}
