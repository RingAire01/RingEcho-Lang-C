#include "backend/backend_c_internal.h"

void c_gen_lvalue(Re0Codegen *c, Re0Expr *e) {
    Re0Buffer *b=&c->output;
    if (!e) { c_storage_fail("missing lvalue"); return; }
    if (e->kind==EXPR_IDENT) { re0_buffer_write_str(b,e->ident.name); return; }
    if (e->kind==EXPR_UNARY && e->unary.op==UNOP_DEREF) {
        re0_buffer_write_str(b,"(*("); c_gen_expr(c,e->unary.operand); re0_buffer_write_str(b,"))"); return;
    }
    if (e->kind==EXPR_SELECT) {
        if (expr_is_pointer_obj(e->select.object)) {
            re0_buffer_write_char(b,'('); c_gen_expr(c,e->select.object);
            re0_buffer_write_fmt(b,")->%s",e->select.field);
        } else { c_gen_lvalue(c,e->select.object); re0_buffer_write_fmt(b,".%s",e->select.field); }
        return;
    }
    if (e->kind==EXPR_INDEX) {
        char name[128];
        if (!c_storage_expr_sequence(e->index.target,name,sizeof(name))) { c_storage_fail("indexed lvalue has no sequence type"); return; }
        const CStorageType *t=c_storage_find(name); int id=c->temp_counter++;
        re0_buffer_write_fmt(b,"(*({ __auto_type __seqp%d = &(",id);
        c_gen_lvalue(c,e->index.target);
        re0_buffer_write_fmt(b,"); int64_t __idx%d = (",id); c_gen_expr(c,e->index.index);
        re0_buffer_write_fmt(b,"); &__seqp%d->data[__reo_check_index(__idx%d,",id,id);
        if(t->kind==C_STORAGE_ARRAY) re0_buffer_write_fmt(b,"%zu",t->length);
        else re0_buffer_write_fmt(b,"__seqp%d->len",id);
        re0_buffer_write_str(b,")]; }))"); return;
    }
    c_storage_fail("address or assignment requires an lvalue");
}

void c_gen_sequence_expr(Re0Codegen *c, Re0Expr *e) {
    Re0Buffer *b=&c->output; char name[128];
    if (!c_storage_expr_sequence(e,name,sizeof(name))) { c_storage_fail("array type is unresolved"); return; }
    const CStorageType *t=c_storage_find(name);
    if(e->kind==EXPR_ARRAY) {
        re0_buffer_write_fmt(b,"((%s){ .data = {",name);
        if(!e->array.count) re0_buffer_write_str(b," ");
        for(int i=0;i<e->array.count;i++){if(i)re0_buffer_write_str(b,", ");c_gen_expr(c,e->array.elems[i]);}
        re0_buffer_write_str(b,"} })");
    } else {
        int id=c->temp_counter++;
        re0_buffer_write_fmt(b,"({ %s __repeat%d = (",t->element,id);
        c_gen_expr(c,e->array_repeat.value);
        re0_buffer_write_fmt(b,"); int64_t __count%d = (",id); c_gen_expr(c,e->array_repeat.count);
        re0_buffer_write_fmt(b,"); %s_repeat(__repeat%d,__count%d); })",name,id,id);
    }
}

void c_gen_sequence_set(Re0Codegen *c, Re0Stmt *s) {
    Re0Expr indexed={.kind=EXPR_INDEX,.span=s->span};
    indexed.index.target=s->index_assign.target; indexed.index.index=s->index_assign.index;
    char name[128];
    if(!c_storage_expr_sequence(indexed.index.target,name,sizeof(name))) { c_storage_fail("assignment requires a typed sequence");return; }
    const CStorageType *t=c_storage_find(name); int id=c->temp_counter++;
    re0_buffer_write_fmt(&c->output,"{ %s *__slot%d = &(",t->element,id);
    c_gen_lvalue(c,&indexed);
    re0_buffer_write_str(&c->output,"); ");
    re0_buffer_write_fmt(&c->output,"*__slot%d = ",id);
    if(s->index_assign.op==BINOP_ASSIGN_SENTINEL) c_gen_expr(c,s->index_assign.value);
    else {
        char temporary[64]; snprintf(temporary,sizeof(temporary),"(*__slot%d)",id);
        Re0Expr left={.kind=EXPR_IDENT,.span=s->span}; left.ident.name=temporary;
        int saved=var_type_count;
        track_var(temporary,t->element);
        Re0Type result={.kind=c_expr_scalar_kind(&left)};
        Re0Expr binary={.kind=EXPR_BINARY,.span=s->span,.resolved_type=&result};
        binary.binary.op=s->index_assign.op; binary.binary.left=&left; binary.binary.right=s->index_assign.value;
        c_gen_expr(c,&binary);
        while(var_type_count>saved) free(var_types[--var_type_count].name);
    }
    re0_buffer_write_str(&c->output,"; }\n");
}

void c_gen_store(Re0Codegen *c, Re0Expr *target, Re0Expr *value, Re0BinOpKind op) {
    int id=c->temp_counter++;
    re0_buffer_write_fmt(&c->output,"{ __auto_type __place%d = &(",id);
    c_gen_lvalue(c,target);
    re0_buffer_write_fmt(&c->output,"); *__place%d = ",id);
    if(op==BINOP_ASSIGN_SENTINEL) c_gen_expr(c,value);
    else {
        char type[128],name[64];
        if(!infer_expr_c_type(target,type,sizeof(type))) {c_storage_fail("compound store target is unresolved");return;}
        snprintf(name,sizeof(name),"(*__place%d)",id);
        int saved=var_type_count;track_var(name,type);
        Re0Expr lhs={.kind=EXPR_IDENT,.span=target->span};lhs.ident.name=name;
        Re0Type result={.kind=c_expr_scalar_kind(&lhs)};
        Re0Expr binary={.kind=EXPR_BINARY,.span=target->span,.resolved_type=&result};
        binary.binary.op=op;binary.binary.left=&lhs;binary.binary.right=value;
        c_gen_expr(c,&binary);
        while(var_type_count>saved)free(var_types[--var_type_count].name);
    }
    re0_buffer_write_str(&c->output,"; }\n");
}
