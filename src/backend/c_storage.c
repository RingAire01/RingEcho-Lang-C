#include "backend/backend_c_internal.h"
#include <stdlib.h>
#include <inttypes.h>

enum { C_MAX_TYPES = 4096, C_MAX_TYPE_DEPTH = 128, C_MAX_SEQUENCE = 1048576 };
typedef struct { const char *parameter, *value; } Binding;
typedef struct {
    Re0Codegen *c;
    Re0StmtVec *declarations;
    CStorageType *types;
    size_t count;
    unsigned depth;
    Binding bindings[128];
    size_t binding_count;
    Re0Buffer forwards, definitions, helpers;
} Storage;
static RE0_THREAD_LOCAL Storage storage;

static void collect_templates(Re0Stmt **list,size_t count,unsigned depth) {
    if(depth>=C_MAX_TYPE_DEPTH){c_storage_fail("declaration nesting limit exceeded");return;}
    for(size_t i=0;i<count;i++) {
        Re0Stmt *s=list[i];unsigned wrappers=depth;
        while(s && (s->kind==STMT_PUB || s->kind==STMT_ATTRIBUTE) && wrappers++<C_MAX_TYPE_DEPTH)
            s=s->kind==STMT_PUB?s->pub.inner:s->attribute.inner;
        if(!s)continue;
        if(s->kind==STMT_MODULE)collect_templates(s->module.body,(size_t)s->module.body_count,depth+1);
        if(s->kind==STMT_STRUCT && s->struct_decl.type_param_count)register_generic_struct(s->struct_decl.name,s);
        if(s->kind==STMT_FUNCTION && s->function.type_param_count)register_generic_fn(s->function.name,s);
    }
}

void c_storage_fail(const char *message) {
    if (storage.c && !storage.c->had_error)
        re0_error_append(storage.c->errors, RE0_ERR_SEMANTIC, RE0_SPAN_ZERO, NULL,
                         "C type lowering: %s", message);
    if (storage.c) storage.c->had_error = true;
}

void c_storage_destroy(void) {
    for (size_t i = 0; i < storage.count; i++) free((void *)storage.types[i].key);
    free(storage.types);
    re0_buffer_free(&storage.forwards); re0_buffer_free(&storage.definitions);
    re0_buffer_free(&storage.helpers);
    memset(&storage, 0, sizeof(storage));
}

void c_storage_begin(Re0Codegen *c, Re0StmtVec *declarations) {
    c_storage_destroy();
    storage.c = c; storage.declarations = declarations;
    storage.types = calloc(C_MAX_TYPES, sizeof(*storage.types));
    re0_buffer_init(&storage.forwards); re0_buffer_init(&storage.definitions);
    re0_buffer_init(&storage.helpers);
    if (!storage.types) c_storage_fail("cannot allocate type registry");
    if(declarations && !c->had_error)collect_templates(declarations->data,declarations->len,0);
}

const CStorageType *c_storage_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < storage.count; i++)
        if (strcmp(storage.types[i].name, name) == 0) return &storage.types[i];
    return NULL;
}

static CStorageType *entry(const char *key, CStorageKind kind, const char *name) {
    if (!storage.c || storage.c->had_error || !key) return NULL;
    for (size_t i = 0; i < storage.count; i++)
        if (strcmp(storage.types[i].key, key) == 0) return &storage.types[i];
    if (storage.count == C_MAX_TYPES) { c_storage_fail("type registry limit exceeded"); return NULL; }
    CStorageType *t = &storage.types[storage.count];
    t->key = strdup(key);
    if (!t->key) { c_storage_fail("cannot allocate type identity"); return NULL; }
    t->kind = kind;
    if (name) {
        if (strlen(name) >= sizeof(t->name)) { free((void *)t->key); t->key = NULL; c_storage_fail("type name is too long"); return NULL; }
        snprintf(t->name, sizeof(t->name), "%s", name);
    } else snprintf(t->name, sizeof(t->name), "__reo_type_%zu", storage.count);
    snprintf(t->tag, sizeof(t->tag), "%s", t->name);
    storage.count++;
    return t;
}

static Re0Stmt *find_decl_in(Re0Stmt **list,size_t count,const char *name,unsigned depth) {
    if(depth>=C_MAX_TYPE_DEPTH){c_storage_fail("declaration nesting limit exceeded");return NULL;}
    for (size_t i = 0; i < count; i++) {
        Re0Stmt *s = list[i];
        unsigned wrappers=depth;
        while (s && (s->kind == STMT_PUB || s->kind == STMT_ATTRIBUTE) && wrappers++ < C_MAX_TYPE_DEPTH)
            s = s->kind == STMT_PUB ? s->pub.inner : s->attribute.inner;
        if (!s) continue;
        if(s->kind==STMT_MODULE) {
            Re0Stmt *found=find_decl_in(s->module.body,(size_t)s->module.body_count,name,depth+1);
            if(found)return found;
        }
        const char *candidate = s->kind == STMT_STRUCT ? s->struct_decl.name :
                                s->kind == STMT_ENUM ? s->enum_decl.name : s->kind==STMT_COMPONENT?s->component.name:NULL;
        if (candidate && strcmp(candidate, name) == 0) return s;
    }
    return NULL;
}
static Re0Stmt *find_decl(const char *name) {
    return storage.declarations?find_decl_in(storage.declarations->data,storage.declarations->len,name,0):NULL;
}

static const char *render(const Re0Type *type, bool complete);
static const char *text(const char *name, bool complete);

const char *c_storage_pointer(const char *element, bool mutable_) {
    char key[192],name[128];
    if(!element){c_storage_fail("missing pointer target");return "__reo_invalid_type";}
    snprintf(key,sizeof(key),"pointer:%d:%s",mutable_,element);
    int n=snprintf(name,sizeof(name),"%s%s*",element,mutable_?"":" const");
    if(n<0 || (size_t)n>=sizeof(name)){c_storage_fail("pointer nesting too deep");return "__reo_invalid_type";}
    CStorageType *t=entry(key,C_STORAGE_POINTER,name);
    if(t)snprintf(t->element,sizeof(t->element),"%s",element);
    return t?t->name:"__reo_invalid_type";
}

static const char *scalar(Re0TypeKind kind) {
    switch (kind) {
        case RE0_TYPE_I8: return "int8_t"; case RE0_TYPE_I16: return "int16_t";
        case RE0_TYPE_I32: return "int32_t"; case RE0_TYPE_I64: return "int64_t";
        case RE0_TYPE_I128: return "__int128"; case RE0_TYPE_ISIZE: return "intptr_t";
        case RE0_TYPE_U8: return "uint8_t"; case RE0_TYPE_U16: return "uint16_t";
        case RE0_TYPE_U32: return "uint32_t"; case RE0_TYPE_U64: return "uint64_t";
        case RE0_TYPE_U128: return "unsigned __int128"; case RE0_TYPE_USIZE: return "uintptr_t";
        case RE0_TYPE_F32: return "float"; case RE0_TYPE_F64: return "double";
        case RE0_TYPE_BOOL: return "bool"; case RE0_TYPE_CHAR: return "uint8_t";
        case RE0_TYPE_STR: return "const char*";
        case RE0_TYPE_UNIT: return "__reo_unit";
        case RE0_TYPE_NEVER: return "void";
        default: return NULL;
    }
}

const char *c_storage_sequence(const char *element, size_t count, bool slice) {
    if (!element || strcmp(element, "void") == 0 || strlen(element) >= 128 || count > C_MAX_SEQUENCE) {
        c_storage_fail("invalid array element or length"); return "__reo_invalid_type";
    }
    char key[192]; snprintf(key, sizeof(key), "%s:%s:%zu", slice ? "slice" : "array", element, slice ? 0 : count);
    CStorageType *t = entry(key, slice ? C_STORAGE_SLICE : C_STORAGE_ARRAY, NULL);
    if (!t) return "__reo_invalid_type";
    if (t->state) return t->name;
    t->state = 2; t->length = count; snprintf(t->element, sizeof(t->element), "%s", element);
    Re0Buffer *b = &storage.definitions;
    if (slice) re0_buffer_write_fmt(b, "typedef struct { %s *data; size_t len; } %s;\n", element, t->name);
    else re0_buffer_write_fmt(b, "__extension__ typedef struct { %s data[%zu]; } %s;\n", element, count, t->name);
    b=&storage.helpers;
    re0_buffer_write_fmt(b, "static %s %s_repeat(%s value, int64_t count) {\n", t->name, t->name, element);
    re0_buffer_write_fmt(b, "    %s result = {0};\n", t->name);
    if (slice) {
        re0_buffer_write_str(b, "    size_t stride = sizeof(value) ? sizeof(value) : 1;\n    if (count < 0 || (uint64_t)count > __REO_MAX_SEQUENCE || (uint64_t)count > SIZE_MAX / stride) __reo_sequence_error();\n");
        re0_buffer_write_str(b, "    result.len = (size_t)count;\n    result.data = count ? __reo_sequence_alloc((size_t)count * stride) : NULL;\n");
    } else re0_buffer_write_fmt(b, "    if (count != %zu) __reo_sequence_error();\n", count);
    re0_buffer_write_str(b, "    for (int64_t i = 0; i < count; i++) result.data[i] = value;\n    return result;\n}\n");
    return t->name;
}

static const char *record(const Re0Type *type, bool complete) {
    const char *name = type->kind == RE0_TYPE_GENERIC ? type->generic.name : type->named.name;
    if (!name) { c_storage_fail("missing named type"); return "__reo_invalid_type"; }
    const CStorageType *registered = c_storage_find(name);
    if (registered && ((registered->kind!=C_STORAGE_RECORD && registered->kind!=C_STORAGE_ENUM) || registered->state==3)) return registered->name;
    for (size_t i = storage.binding_count; i > 0; i--)
        if (strcmp(storage.bindings[i - 1].parameter, name) == 0) return storage.bindings[i - 1].value;
    const char *alias = re0_model_resolve_type_alias(storage.c->model, name);
    if (alias) return text(alias, complete);
    if (type->kind!=RE0_TYPE_GENERIC && strcmp(name, "Option") == 0) return "Option";
    if (type->kind!=RE0_TYPE_GENERIC && strcmp(name, "Result") == 0) return "Result";
    Re0Stmt *def = find_decl(name);
    if (!def) {
        /* Component and instantiated declarations are tracked by their emitters. */
        Re0StructDef *model=re0_model_find_struct(storage.c->model,name);
        if (model) return model->name;
        c_storage_fail("unresolved named type"); return "__reo_invalid_type";
    }
    int pc = def->kind == STMT_STRUCT ? def->struct_decl.type_param_count : 0;
    int ac = type->kind == RE0_TYPE_GENERIC ? type->generic.arg_count : 0;
    if (pc != ac || pc > 8) { c_storage_fail("uninstantiated or invalid generic record"); return "__reo_invalid_type"; }
    const char *args[8];
    Re0Buffer key; re0_buffer_init(&key); re0_buffer_write_str(&key, name);
    for (int i = 0; i < ac; i++) {
        args[i] = render(type->generic.args[i], true);
        re0_buffer_write_fmt(&key, ":%s", args[i]);
    }
    re0_buffer_write_char(&key, 0);
    if(re0_buffer_failed(&key)) {re0_buffer_free(&key);c_storage_fail("cannot allocate generic type key");return "__reo_invalid_type";}
    CStorageType *t = entry(key.data, def->kind == STMT_ENUM ? C_STORAGE_ENUM : C_STORAGE_RECORD, ac ? NULL : name);
    re0_buffer_free(&key);
    if (!t) return "__reo_invalid_type";
    if (!t->state) {
        re0_buffer_write_fmt(&storage.forwards, "typedef struct %s %s;\n", t->name, t->name);
        t->state = 1;
    }
    if (!complete || t->state == 3) return t->name;
    if (t->state == 2) { c_storage_fail("recursive by-value record"); return t->name; }
    t->state = 2;
    size_t saved = storage.binding_count;
    if (saved + (size_t)ac > 128) { c_storage_fail("generic binding depth exceeded"); return t->name; }
    for (int i = 0; i < ac; i++) storage.bindings[storage.binding_count++] = (Binding){def->struct_decl.type_params[i], args[i]};
    Re0Buffer body; re0_buffer_init(&body);
    re0_buffer_write_fmt(&body, "struct %s { ", t->name);
    if (def->kind != STMT_ENUM) {
        int count=def->kind==STMT_STRUCT?def->struct_decl.field_count:def->component.state_count;
        Re0StructFieldDecl *fields=def->kind==STMT_STRUCT?def->struct_decl.fields:def->component.state;
        for (int i = 0; i < count; i++) {
            const char *ft = text(fields[i].type, true);
            re0_buffer_write_fmt(&body, "%s %s; ", ft, fields[i].name);
            track_struct_field(t->name, fields[i].name, ft);
        }
    } else {
        char union_key[192];snprintf(union_key,sizeof(union_key),"payload:%s",t->name);
        CStorageType *payload=entry(union_key,C_STORAGE_RECORD,NULL);
        if(!payload) {re0_buffer_free(&body);storage.binding_count=saved;return t->name;}
        payload->state=3;
        Re0Buffer union_body;re0_buffer_init(&union_body);
        re0_buffer_write_str(&union_body,"__extension__ typedef union { ");
        for (int i = 0; i < def->enum_decl.variant_count; i++) {
            Re0EnumVariantDecl *v = &def->enum_decl.variants[i];
            const char *vt=NULL;
            if(v->type_count==1) vt=text(v->types[0],true);
            else if(v->type_count>1) {
                if(v->type_count>64) {c_storage_fail("enum payload limit exceeded");break;}
                Re0Type *parts[64]={0};
                for(int j=0;j<v->type_count;j++) parts[j]=re0_type_parse(v->types[j]);
                Re0Type tuple={.kind=RE0_TYPE_TUPLE,.tuple={.elems=parts,.count=v->type_count}};
                vt=render(&tuple,true);
                for(int j=0;j<v->type_count;j++) re0_type_free_tree(parts[j]);
            }
            if(vt) {
                char field[32];snprintf(field,sizeof(field),"v%d",i);
                re0_buffer_write_fmt(&union_body,"%s %s; ",vt,field);
                track_struct_field(payload->name,field,vt);
            }
        }
        re0_buffer_write_fmt(&union_body,"} %s;\n",payload->name);
        re0_buffer_write_n(&storage.definitions,union_body.data,union_body.len);
        if(re0_buffer_failed(&union_body)) c_storage_fail("cannot allocate enum definition");
        re0_buffer_free(&union_body);
        re0_buffer_write_fmt(&body,"int64_t tag; %s u; ",payload->name);
        track_struct_field(t->name,"u",payload->name);
    }
    re0_buffer_write_str(&body, "};\n");
    re0_buffer_write_n(&storage.definitions, body.data, body.len);
    if (re0_buffer_failed(&body)) c_storage_fail("cannot allocate record definition");
    re0_buffer_free(&body);
    storage.binding_count = saved; t->state = 3;
    return t->name;
}

static const char *render_impl(const Re0Type *type, bool complete) {
    if (!type) { c_storage_fail("missing type information"); return "__reo_invalid_type"; }
    const char *base = scalar(type->kind);
    if (base) return base;
    if (type->kind == RE0_TYPE_ARRAY || type->kind == RE0_TYPE_SLICE) {
        bool slice = type->kind == RE0_TYPE_SLICE;
        base = render(slice ? type->slice.inner : type->array.inner, !slice);
        return c_storage_sequence(base, slice ? 0 : type->array.size, slice);
    }
    if (type->kind == RE0_TYPE_REFERENCE || type->kind == RE0_TYPE_PTR) {
        if (type->kind == RE0_TYPE_PTR && !type->ptr_.inner) return "void*";
        bool mut = type->kind == RE0_TYPE_REFERENCE ? type->ref_.mutable_ : type->ptr_.mutable_;
        base = render(type->kind == RE0_TYPE_REFERENCE ? type->ref_.inner : type->ptr_.inner, false);
        return c_storage_pointer(base,mut);
    }
    if (type->kind == RE0_TYPE_FN || type->kind == RE0_TYPE_TUPLE) {
        bool fn = type->kind == RE0_TYPE_FN;
        int count = fn ? type->func.param_count : type->tuple.count;
        if (count < 0 || count > 64) { c_storage_fail("too many type components"); return "__reo_invalid_type"; }
        const char *children[64];
        const char *ret = fn ? (type->func.ret->kind==RE0_TYPE_UNIT || type->func.ret->kind==RE0_TYPE_NEVER ? "void" : render(type->func.ret,false)) : NULL;
        Re0Buffer key; re0_buffer_init(&key);
        re0_buffer_write_fmt(&key, "%s:%s:", fn ? "fn" : "tuple", ret ? ret : "");
        for (int i = 0; i < count; i++) {
            children[i] = render(fn ? type->func.params[i] : type->tuple.elems[i], !fn);
            re0_buffer_write_fmt(&key, "%s;", children[i]);
        }
        if (fn && type->func.variadic) re0_buffer_write_str(&key, "...");
        re0_buffer_write_char(&key, 0);
        if(re0_buffer_failed(&key)){re0_buffer_free(&key);c_storage_fail("cannot allocate aggregate type key");return "__reo_invalid_type";}
        CStorageType *t = entry(key.data, fn ? C_STORAGE_FUNCTION : C_STORAGE_TUPLE, NULL);
        re0_buffer_free(&key);
        if (!t) return "__reo_invalid_type";
        if (!t->state) {
            t->state = 3;
            Re0Buffer *b = &storage.definitions;
            if (fn) {
                re0_buffer_write_fmt(b, "typedef %s (*%s)(", ret, t->name);
                if (!count) re0_buffer_write_str(b, "void");
                for (int i = 0; i < count; i++) re0_buffer_write_fmt(b, "%s%s", i ? ", " : "", children[i]);
                if (type->func.variadic) re0_buffer_write_str(b, ", ...");
                re0_buffer_write_str(b, ");\n");
            } else {
                re0_buffer_write_str(b, "__extension__ typedef struct { ");
                for (int i = 0; i < count; i++) {
                    re0_buffer_write_fmt(b, "%s f%d; ", children[i], i);
                    char field[32];snprintf(field,sizeof(field),"f%d",i);
                    track_struct_field(t->name,field,children[i]);
                }
                re0_buffer_write_fmt(b, "} %s;\n", t->name);
            }
        }
        return t->name;
    }
    if (type->kind == RE0_TYPE_GENERIC && strcmp(type->generic.name, "Result") == 0 && type->generic.arg_count == 2 &&
        type->generic.args[1]->kind == RE0_TYPE_I64) {
        const Re0Type *payload = type->generic.args[0];
        if (payload->kind <= RE0_TYPE_STR) {
            char name[128]; snprintf(name, sizeof(name), "__reo_result_%s", re0_type_kind_name(payload->kind));
            CStorageType *t = entry(name, C_STORAGE_RESULT, name); return t ? t->name : "__reo_invalid_type";
        }
        base = render(payload, true);
        char key[192]; snprintf(key, sizeof(key), "result:%s", base);
        CStorageType *t = entry(key, C_STORAGE_RESULT, NULL);
        if (!t) return "__reo_invalid_type";
        if (!t->state) {
            t->state = 3; snprintf(t->element, sizeof(t->element), "%s", base);
            re0_buffer_write_fmt(&storage.definitions,
                "typedef struct { int64_t tag; %s value; int64_t error; int64_t index; } %s;\n", base, t->name);
            track_struct_field(t->name, "value", base);
        }
        return t->name;
    }
    if (type->kind == RE0_TYPE_VEC) {
        base = render(type->vec.inner, false);
        /* Implemented below as a precisely typed shared container handle. */
        char key[192]; snprintf(key, sizeof(key), "vec:%s", base);
        CStorageType *t = entry(key, C_STORAGE_VECTOR, NULL);
        if (!t) return "__reo_invalid_type";
        if (!t->state) {
            t->state = 3; snprintf(t->element, sizeof(t->element), "%s", base);
            size_t n = strlen(t->name); t->name[n] = '*'; t->name[n + 1] = 0;
            Re0Buffer *b = &storage.definitions;
            re0_buffer_write_fmt(b, "typedef struct { %s *data; size_t len, cap; } %s;\n", base, t->tag);
            b=&storage.helpers;
            bool strings=strcmp(base,"const char*")==0;
            if(strings) re0_buffer_write_fmt(b,"static char *%s_copy(const char *s) { if (!s) s=\"\"; size_t n=strlen(s); if(n==SIZE_MAX) __reo_sequence_error(); char *p=malloc(n+1); if(!p) __reo_sequence_error(); memcpy(p,s,n+1); return p; }\n",t->tag);
            re0_buffer_write_fmt(b, "static %s %s_new(void) { %s v = calloc(1, sizeof(*v)); if (!v) __reo_sequence_error(); return v; }\n", t->name, t->tag, t->name);
            re0_buffer_write_fmt(b, "static void %s_push(%s v, %s x) {\n", t->tag, t->name, base);
            re0_buffer_write_str(b, "if (!v || v->len >= __REO_MAX_SEQUENCE) __reo_sequence_error(); if (v->len == v->cap) { size_t cap = v->cap ? v->cap * 2 : 8; size_t stride=sizeof(*v->data)?sizeof(*v->data):1; if (cap > __REO_MAX_SEQUENCE || cap > SIZE_MAX / stride) __reo_sequence_error();\n");
            re0_buffer_write_fmt(b, "%s *p = realloc(v->data, cap * stride); if (!p) __reo_sequence_error(); v->data=p; v->cap=cap; } ", base);
            if(strings) re0_buffer_write_fmt(b,"x=%s_copy(x); ",t->tag);
            re0_buffer_write_str(b,"v->data[v->len++] = x; }\n");
            re0_buffer_write_fmt(b, "static %s %s_get(%s v, int64_t i) { if (!v) __reo_sequence_error(); return v->data[__reo_check_index(i,v->len)]; }\n", base, t->tag, t->name);
            re0_buffer_write_fmt(b, "static void %s_set(%s v, int64_t i, %s x) { if (!v) __reo_sequence_error(); size_t slot=__reo_check_index(i,v->len); ", t->tag, t->name, base);
            if(strings) re0_buffer_write_fmt(b,"x=%s_copy(x); free((void*)v->data[slot]); ",t->tag);
            re0_buffer_write_str(b,"v->data[slot] = x; }\n");
            re0_buffer_write_fmt(b, "static %s %s_pop(%s v) { if (!v || !v->len) __reo_sequence_error(); return v->data[--v->len]; }\n", base, t->tag, t->name);
            re0_buffer_write_fmt(b, "static %s %s_last(%s v) { if (!v || !v->len) __reo_sequence_error(); return v->data[v->len-1]; }\n", base, t->tag, t->name);
            re0_buffer_write_fmt(b, "static int64_t %s_len(%s v) { if (!v) __reo_sequence_error(); return (int64_t)v->len; }\n", t->tag, t->name);
            if(strings) re0_buffer_write_fmt(b,"static const char *%s_get_or_empty(%s v,int64_t i) { return !v || i<0 || (uint64_t)i>=v->len ? \"\" : v->data[i]; }\n",t->tag,t->name);
            re0_buffer_write_fmt(b, "static void %s_free(%s v) { if (v) { ", t->tag, t->name);
            if(strings) re0_buffer_write_str(b,"for(size_t i=0;i<v->len;i++) free((void*)v->data[i]); ");
            re0_buffer_write_str(b,"free(v->data); free(v); } }\n");
        }
        return t->name;
    }
    if (type->kind == RE0_TYPE_STRUCT || type->kind == RE0_TYPE_ENUM || type->kind == RE0_TYPE_TYPEVAR || type->kind == RE0_TYPE_GENERIC)
        return record(type, complete);
    c_storage_fail("unresolved type cannot be replaced by an integer");
    return "__reo_invalid_type";
}

static const char *render(const Re0Type *type, bool complete) {
    if (++storage.depth > C_MAX_TYPE_DEPTH) { storage.depth--; c_storage_fail("type depth exceeded"); return "__reo_invalid_type"; }
    const char *result = render_impl(type, complete);
    storage.depth--; return result;
}

static const char *text(const char *name, bool complete) {
    if (!name) return "void";
    if (strcmp(name,"void*")==0) return "void*";
    const CStorageType *known = c_storage_find(name);
    if (known) {
        if(complete && (known->kind==C_STORAGE_RECORD || known->kind==C_STORAGE_ENUM) && known->state!=3) {
            Re0Type named={.kind=RE0_TYPE_STRUCT,.named={.name=(char*)name}};
            return record(&named,true);
        }
        return known->name;
    }
    for (int k = RE0_TYPE_I8; k <= RE0_TYPE_NEVER; k++) {
        const char *s = scalar((Re0TypeKind)k);
        if (s && strcmp(name, s) == 0) return s;
    }
    for (size_t i = storage.binding_count; i > 0; i--)
        if (strcmp(storage.bindings[i - 1].parameter, name) == 0) return storage.bindings[i - 1].value;
    Re0Type *type = re0_type_parse(name);
    if (!type) { c_storage_fail("invalid type annotation"); return "__reo_invalid_type"; }
    const char *result = render(type, complete);
    re0_type_free_tree(type); return result;
}

const char *c_storage_text(const char *name) { return text(name, true); }
const char *c_storage_return(const char *name) {
    if(!name)return "void";
    const char *type=text(name,true);
    return strcmp(type,"__reo_unit")==0?"void":type;
}
const char *c_storage_type(const Re0Type *type) { return render(type, true); }
size_t c_storage_bind(char **parameters, char **arguments, int count) {
    size_t mark=storage.binding_count;
    if(count<0 || mark+(size_t)count>128) { c_storage_fail("generic binding limit exceeded"); return mark; }
    for(int i=0;i<count;i++) {
        const char *value=text(arguments[i],true);
        storage.bindings[storage.binding_count++]=(Binding){parameters[i],value};
    }
    return mark;
}
void c_storage_unbind(size_t mark) { if(mark<=storage.binding_count) storage.binding_count=mark; }
int c_storage_capture(char names[8][64],char arguments[8][128]) {
    if(storage.binding_count>8){c_storage_fail("lambda generic environment exceeds limit");return 0;}
    for(size_t i=0;i<storage.binding_count;i++) {
        if(strlen(storage.bindings[i].parameter)>=64 || strlen(storage.bindings[i].value)>=128) {c_storage_fail("lambda type binding is too long");return 0;}
        snprintf(names[i],64,"%s",storage.bindings[i].parameter);
        snprintf(arguments[i],128,"%s",storage.bindings[i].value);
    }
    return (int)storage.binding_count;
}
const char *c_storage_generic(const char *name, const char *argument) {
    /* Internal monomorphization may pass a canonical C spelling. */
    Re0Type *arg = NULL;
    for (int k = RE0_TYPE_I8; k <= RE0_TYPE_NEVER; k++) {
        const char *s = scalar((Re0TypeKind)k);
        if (s && strcmp(s, argument) == 0) { arg = re0_type_make((Re0TypeKind)k, NULL); break; }
    }
    if (!arg) arg = re0_type_parse(argument);
    if(strcmp(argument,"void*")==0) { re0_type_free_tree(arg);arg=re0_type_make(RE0_TYPE_PTR,NULL); }
    Re0Type generic = {.kind = RE0_TYPE_GENERIC, .generic = {.name = (char *)name, .args = &arg, .arg_count = 1}};
    const char *result = render(&generic, true);
    re0_type_free_tree(arg); return result;
}

void c_storage_finish(Re0Codegen *c) {
    if (!storage.c) return;
    if (re0_buffer_failed(&storage.forwards) || re0_buffer_failed(&storage.definitions) || re0_buffer_failed(&storage.helpers)) c_storage_fail("cannot allocate type declarations");
    if (!c->had_error && g_fwd_insert_pos <= c->output.len) {
        Re0Buffer joined; re0_buffer_init(&joined);
        re0_buffer_write_n(&joined, c->output.data, g_fwd_insert_pos);
        re0_buffer_write_n(&joined, storage.forwards.data, storage.forwards.len);
        re0_buffer_write_n(&joined, storage.definitions.data, storage.definitions.len);
        re0_buffer_write_n(&joined, storage.helpers.data, storage.helpers.len);
        re0_buffer_write_n(&joined, c->output.data + g_fwd_insert_pos, c->output.len - g_fwd_insert_pos);
        if (re0_buffer_failed(&joined)) { c_storage_fail("cannot publish type declarations"); re0_buffer_free(&joined); }
        else { re0_buffer_free(&c->output); c->output = joined; }
    }
    c_storage_destroy();
}
