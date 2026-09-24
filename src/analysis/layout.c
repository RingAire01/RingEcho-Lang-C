#include "analysis/layout.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const Re0TargetLayout re0_target_x86_64_sysv = {8, 8, 16, PTRDIFF_MAX};
typedef struct Binding {
    char **names;
    Re0Type **args;
    int count;
    const struct Binding *parent;
} Binding;

static void fail(Re0LayoutManager *m, const char *message) {
    if (!m->failed && m->errors)
        re0_error_append(m->errors, RE0_ERR_SEMANTIC, RE0_SPAN_ZERO, NULL,
                         "type layout: %s", message);
    m->failed = true;
}

static bool power_two(size_t n) { return n && !(n & (n - 1)); }
bool re0_layout_init(Re0LayoutManager *m, const Re0TargetLayout *target,
                     Re0SemanticModel *model, Re0StmtVec *declarations, Re0ErrorList *errors) {
    if (!m) return false;
    memset(m, 0, sizeof(*m));
    m->model = model; m->declarations = declarations; m->errors = errors;
    if (!target || (target->pointer_size != 4 && target->pointer_size != 8) ||
        !power_two(target->pointer_align) || target->pointer_align > target->pointer_size ||
        !power_two(target->int128_align) || target->int128_align > 16 ||
        !target->max_object_size || target->max_object_size > PTRDIFF_MAX) {
        fail(m, "invalid target description"); return false;
    }
    m->target = *target;
    return true;
}

void re0_layout_destroy(Re0LayoutManager *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        Re0Layout *t = m->nodes[i];
        free((void *)t->name);
        for (size_t j = 0; j < t->field_count; j++) free((void *)t->fields[j].name);
        for (size_t j = 0; j < t->variant_count; j++) free((void *)t->variants[j].name);
        free(t->fields); free(t->variants); free(t);
    }
    free(m->nodes);
    memset(m, 0, sizeof(*m));
}

static char *copy_name(Re0LayoutManager *m, const char *name) {
    char *copy = strdup(name ? name : "");
    if (!copy) fail(m, "out of memory copying type name");
    return copy;
}

static Re0Layout *node(Re0LayoutManager *m, Re0TypeKind kind) {
    if (m->failed) return NULL;
    if (m->count == RE0_LAYOUT_MAX_NODES) { fail(m, "type graph limit exceeded"); return NULL; }
    if (m->count == m->capacity) {
        size_t capacity = m->capacity ? m->capacity * 2 : 32;
        Re0Layout **p = realloc(m->nodes, capacity * sizeof(*p));
        if (!p) { fail(m, "out of memory growing type graph"); return NULL; }
        m->nodes = p; m->capacity = capacity;
    }
    Re0Layout *t = calloc(1, sizeof(*t));
    if (!t) { fail(m, "out of memory allocating layout"); return NULL; }
    t->kind = kind; t->align = 1; t->inhabited = true;
    m->nodes[m->count++] = t;
    return t;
}

static bool align_up(Re0LayoutManager *m, size_t size, size_t alignment, size_t *out) {
    if (!power_two(alignment) || size > m->target.max_object_size) {
        fail(m, "object size or alignment overflow"); return false;
    }
    size_t padding = (0 - size) & (alignment - 1);
    if (padding > m->target.max_object_size - size) {
        fail(m, "object size or alignment overflow"); return false;
    }
    *out = size + padding;
    return true;
}

static bool fields(Re0LayoutManager *m, Re0Layout *t, size_t count) {
    if (count > RE0_LAYOUT_MAX_FIELDS) { fail(m, "field count limit exceeded"); return false; }
    if (count) {
        t->fields = calloc(count, sizeof(*t->fields));
        if (!t->fields) { fail(m, "out of memory allocating fields"); return false; }
    }
    t->field_count = count;
    return true;
}

static bool append_field(Re0LayoutManager *m, Re0Layout *t, size_t index,
                         const char *name, Re0Layout *child) {
    if (!child || index >= t->field_count) return false;
    size_t offset;
    if (!align_up(m, t->size, child->align, &offset)) return false;
    if (child->size > m->target.max_object_size - offset) {
        fail(m, "aggregate size overflow"); return false;
    }
    t->fields[index] = (Re0LayoutField){copy_name(m, name), offset, child};
    t->size = offset + child->size;
    if (t->align < child->align) t->align = child->align;
    t->inhabited = t->inhabited && child->inhabited;
    return !m->failed;
}

static Re0Stmt *declaration(Re0LayoutManager *m, const char *name, Re0StmtKind kind) {
    if (!m->declarations || !name) return NULL;
    for (size_t i = 0; i < m->declarations->len; i++) {
        Re0Stmt *s = m->declarations->data[i];
        unsigned depth = 0;
        while (s && (s->kind == STMT_ATTRIBUTE || s->kind == STMT_PUB) && depth++ < RE0_LAYOUT_MAX_DEPTH)
            s = s->kind == STMT_PUB ? s->pub.inner : s->attribute.inner;
        if (!s || s->kind != kind) continue;
        const char *candidate = kind == STMT_STRUCT ? s->struct_decl.name :
                                kind == STMT_ENUM ? s->enum_decl.name : s->type_alias.name;
        if (candidate && strcmp(candidate, name) == 0) return s;
    }
    return NULL;
}

static Re0Layout *resolve(Re0LayoutManager *m, const Re0Type *type, const Binding *env, unsigned depth);
static Re0Layout *parse(Re0LayoutManager *m, const char *text, const Binding *env, unsigned depth) {
    if (!text || strlen(text) > RE0_LAYOUT_MAX_TYPE_TEXT) { fail(m, "invalid type spelling"); return NULL; }
    Re0Type *t = re0_type_parse(text);
    if (!t) { fail(m, "cannot parse type spelling"); return NULL; }
    Re0Layout *result = resolve(m, t, env, depth);
    re0_type_free_tree(t);
    return result;
}

static Re0Layout *record(Re0LayoutManager *m, const char *name,
                         Re0Type **args, int arg_count, const Binding *env, unsigned depth) {
    Re0Stmt *ast = declaration(m, name, STMT_STRUCT);
    Re0StructDef *model = m->model ? re0_model_find_struct(m->model, name) : NULL;
    int params = ast ? ast->struct_decl.type_param_count : model ? model->type_param_count : -1;
    if (params < 0) { fail(m, "unknown named type"); return NULL; }
    if (params != arg_count) { fail(m, "generic argument count mismatch"); return NULL; }
    Binding binding = {ast ? ast->struct_decl.type_params : model->type_params, args, arg_count, env};
    const Binding *inner = params ? &binding : env;
    Re0Layout *t = node(m, RE0_TYPE_STRUCT);
    if (!t) return NULL;
    t->name = copy_name(m, name);
    int count = ast ? ast->struct_decl.field_count : model->field_count;
    if (count < 0) { fail(m, "invalid record field count"); return NULL; }
    if (!fields(m, t, (size_t)count)) return NULL;
    for (int i = 0; i < count; i++) {
        const char *field_name = ast ? ast->struct_decl.fields[i].name : model->fields[i].name;
        Re0Layout *child = ast ? parse(m, ast->struct_decl.fields[i].type, inner, depth + 1) :
                                resolve(m, model->fields[i].type, inner, depth + 1);
        if (!append_field(m, t, (size_t)i, field_name, child)) return NULL;
    }
    if (!align_up(m, t->size, t->align, &t->size)) return NULL;
    return t;
}

static Re0Layout *enumeration(Re0LayoutManager *m, const char *name,
                              Re0Type **args, int arg_count, const Binding *env, unsigned depth) {
    bool option = strcmp(name, "Option") == 0, result = strcmp(name, "Result") == 0;
    Re0Stmt *ast = declaration(m, name, STMT_ENUM);
    Re0EnumDef *model = m->model ? re0_model_find_enum(m->model, name) : NULL;
    bool declared = ast || (model && !option && !result);
    if (!declared && !option && !result) { fail(m, "unknown enum type"); return NULL; }
    if ((option && arg_count != 0 && arg_count != 1) ||
        (result && arg_count != 0 && arg_count != 2) || (declared && arg_count != 0)) {
        fail(m, "enum type argument count mismatch"); return NULL;
    }
    size_t count = ast ? (size_t)ast->enum_decl.variant_count : declared ? (size_t)model->variant_count : 2;
    if (count > RE0_LAYOUT_MAX_FIELDS) { fail(m, "variant count limit exceeded"); return NULL; }
    Re0Layout *t = node(m, RE0_TYPE_ENUM);
    if (!t) return NULL;
    t->name = copy_name(m, name);
    if (count) {
        t->variants = calloc(count, sizeof(*t->variants));
        if (!t->variants) { fail(m, "out of memory allocating variants"); return NULL; }
    }
    t->variant_count = count; t->inhabited = false;
    size_t payload_size = 0, payload_align = 1;
    for (size_t i = 0; i < count; i++) {
        Re0Layout *payload = node(m, RE0_TYPE_TUPLE);
        if (!payload) return NULL;
        const char *variant = ast ? ast->enum_decl.variants[i].vname :
                              declared ? model->variant_names[i] :
                              option ? (i == 0 ? "None" : "Some") : (i == 0 ? "Ok" : "Err");
        t->variants[i] = (Re0LayoutVariant){copy_name(m, variant), i, payload};
        size_t n = ast ? (size_t)ast->enum_decl.variants[i].type_count :
                   declared ? (size_t)model->variant_type_counts[i] : option && i == 0 ? 0 : 1;
        if (!fields(m, payload, n)) return NULL;
        for (size_t j = 0; j < n; j++) {
            Re0Layout *child;
            if (ast) child = parse(m, ast->enum_decl.variants[i].types[j], env, depth + 1);
            else if (declared) {
                if (!model->variant_types[i]) { fail(m, "enum payload types were not registered"); return NULL; }
                child = parse(m, model->variant_types[i][j], env, depth + 1);
            }
            else if (arg_count) child = resolve(m, args[option ? 0 : i], env, depth + 1);
            else child = parse(m, "i64", env, depth + 1);
            char index[32]; snprintf(index, sizeof(index), "%zu", j);
            if (!append_field(m, payload, j, index, child)) return NULL;
        }
        if (!align_up(m, payload->size, payload->align, &payload->size)) return NULL;
        if (payload_size < payload->size) payload_size = payload->size;
        if (payload_align < payload->align) payload_align = payload->align;
        t->inhabited = t->inhabited || payload->inhabited;
    }
    if (!count) return t;
    /* Stable i64 tag, followed by a maximally aligned payload union. */
    t->align = payload_align > 8 ? payload_align : 8;
    if (!align_up(m, 8, payload_align, &t->payload_offset) ||
        payload_size > m->target.max_object_size - t->payload_offset) {
        fail(m, "enum size overflow"); return NULL;
    }
    if (!align_up(m, t->payload_offset + payload_size, t->align, &t->size)) return NULL;
    return t;
}

static Re0Layout *resolve(Re0LayoutManager *m, const Re0Type *type, const Binding *env, unsigned depth) {
    if (m->failed) return NULL;
    if (!type || depth >= RE0_LAYOUT_MAX_DEPTH) {
        fail(m, "invalid type or recursive by-value layout exceeds depth limit"); return NULL;
    }
    if (type->kind == RE0_TYPE_STRUCT || type->kind == RE0_TYPE_ENUM || type->kind == RE0_TYPE_TYPEVAR) {
        const char *name = type->named.name;
        if (!name) { fail(m, "missing type name"); return NULL; }
        for (const Binding *b = env; b; b = b->parent)
            for (int i = 0; i < b->count; i++)
                if (strcmp(name, b->names[i]) == 0) return resolve(m, b->args[i], b->parent, depth + 1);
        Re0Stmt *alias = declaration(m, name, STMT_TYPE_ALIAS);
        const char *target = alias ? alias->type_alias.target :
            m->model ? re0_model_resolve_type_alias(m->model, name) : NULL;
        if (target) return parse(m, target, env, depth + 1);
        if (declaration(m, name, STMT_ENUM) || (m->model && re0_model_find_enum(m->model, name)) ||
            strcmp(name, "Option") == 0 || strcmp(name, "Result") == 0)
            return enumeration(m, name, NULL, 0, env, depth + 1);
        return record(m, name, NULL, 0, env, depth + 1);
    }
    if (type->kind == RE0_TYPE_GENERIC) {
        if (!type->generic.name || type->generic.arg_count < 0 ||
            (type->generic.arg_count && !type->generic.args)) {
            fail(m, "invalid generic type"); return NULL;
        }
        if(strcmp(type->generic.name,"Result")==0 && type->generic.arg_count==2 && type->generic.args[1]->kind==RE0_TYPE_I64) {
            Re0Layout *result=node(m,RE0_TYPE_STRUCT);
            if(!result || !fields(m,result,4))return NULL;
            result->name=copy_name(m,"Result");
            Re0Type integer={.kind=RE0_TYPE_I64};
            Re0Layout *tag=resolve(m,&integer,env,depth+1);
            Re0Layout *value=resolve(m,type->generic.args[0],env,depth+1);
            if(!append_field(m,result,0,"tag",tag) || !append_field(m,result,1,"value",value) ||
               !append_field(m,result,2,"error",tag) || !append_field(m,result,3,"index",tag) ||
               !align_up(m,result->size,result->align,&result->size))return NULL;
            result->inhabited=true; /* The failure variant does not require a payload value. */
            return result;
        }
        if (strcmp(type->generic.name, "Option") == 0 || strcmp(type->generic.name, "Result") == 0)
            return enumeration(m, type->generic.name, type->generic.args, type->generic.arg_count, env, depth + 1);
        return record(m, type->generic.name, type->generic.args, type->generic.arg_count, env, depth + 1);
    }
    if (type->kind == RE0_TYPE_UNKNOWN) { fail(m, "unresolved type cannot have a runtime layout"); return NULL; }
    Re0Layout *t = node(m, type->kind);
    if (!t) return NULL;
    switch (type->kind) {
        case RE0_TYPE_I8: case RE0_TYPE_U8: case RE0_TYPE_BOOL: case RE0_TYPE_CHAR: t->size = t->align = 1; break;
        case RE0_TYPE_I16: case RE0_TYPE_U16: t->size = t->align = 2; break;
        case RE0_TYPE_I32: case RE0_TYPE_U32: case RE0_TYPE_F32: t->size = t->align = 4; break;
        case RE0_TYPE_I64: case RE0_TYPE_U64: case RE0_TYPE_F64: t->size = t->align = 8; break;
        case RE0_TYPE_I128: case RE0_TYPE_U128: t->size = 16; t->align = m->target.int128_align; break;
        case RE0_TYPE_ISIZE: case RE0_TYPE_USIZE: case RE0_TYPE_PTR: case RE0_TYPE_REFERENCE:
        case RE0_TYPE_STR: case RE0_TYPE_FN:
            t->size = m->target.pointer_size; t->align = m->target.pointer_align; break;
        case RE0_TYPE_NEVER: t->inhabited = false; break;
        case RE0_TYPE_UNIT: break;
        case RE0_TYPE_ARRAY:
            t->element = resolve(m, type->array.inner, env, depth + 1);
            if (!t->element) return NULL;
            t->length = type->array.size; t->align = t->element->align;
            if (t->length && t->element->size > m->target.max_object_size / t->length) {
                fail(m, "array size overflow"); return NULL;
            }
            t->size = t->length * t->element->size;
            t->inhabited = !t->length || t->element->inhabited; break;
        case RE0_TYPE_SLICE:
            /* The descriptor has a finite layout even for recursive pointees. */
            t->size = m->target.pointer_size * 2;
            t->align = m->target.pointer_align; break;
        case RE0_TYPE_VEC:
            /* Vec values are shared typed handles. The allocated descriptor
             * contains data/len/cap; it is not copied as a by-value triple. */
            t->size=m->target.pointer_size;t->align=m->target.pointer_align;
            t->allocation_size=m->target.pointer_size*3;break;
        case RE0_TYPE_TUPLE:
            if (type->tuple.count < 0 || (type->tuple.count && !type->tuple.elems)) {
                fail(m, "invalid tuple field list"); return NULL;
            }
            if (!fields(m, t, (size_t)type->tuple.count)) return NULL;
            for (int i = 0; i < type->tuple.count; i++) {
                char index[32]; snprintf(index, sizeof(index), "%d", i);
                if (!append_field(m, t, (size_t)i, index, resolve(m, type->tuple.elems[i], env, depth + 1))) return NULL;
            }
            if (!align_up(m, t->size, t->align, &t->size)) return NULL;
            break;
        default: fail(m, "invalid runtime type kind"); return NULL;
    }
    if (t->size > m->target.max_object_size) { fail(m, "object size limit exceeded"); return NULL; }
    return m->failed ? NULL : t;
}

Re0Layout *re0_layout_type(Re0LayoutManager *m, const Re0Type *type) {
    if (!m || !type || m->failed) return NULL;
    return resolve(m, type, NULL, 0);
}
Re0Layout *re0_layout_parse(Re0LayoutManager *m, const char *type) {
    if (!m || !type || m->failed) return NULL;
    return parse(m, type, NULL, 0);
}
const Re0LayoutField *re0_layout_field(const Re0Layout *type, const char *name) {
    if (!type || !name || type->field_count > RE0_LAYOUT_MAX_FIELDS ||
        (type->field_count && !type->fields)) return NULL;
    for (size_t i = 0; i < type->field_count; i++)
        if (strcmp(type->fields[i].name, name) == 0) return &type->fields[i];
    return NULL;
}

static bool classify(const Re0Layout *t, size_t offset, Re0SysvClass *out, unsigned depth) {
    if (!t || depth >= RE0_LAYOUT_MAX_DEPTH || offset > 16 || t->size > 16 - offset ||
        !power_two(t->align) || offset % t->align) return false;
    if (!t->size) return true;
    if (t->kind == RE0_TYPE_ARRAY) {
        if (!t->element || !t->element->size) return t->size == 0;
        for (size_t i = 0; i < t->length; i++)
            if (!classify(t->element, offset + i * t->element->size, out, depth + 1)) return false;
        return true;
    }
    if (t->kind == RE0_TYPE_ENUM) {
        if (t->variant_count > RE0_LAYOUT_MAX_FIELDS || (t->variant_count && !t->variants)) return false;
        Re0Layout tag = {.kind = RE0_TYPE_I64, .size = 8, .align = 8, .inhabited = true};
        if (!classify(&tag, offset, out, depth + 1)) return false;
        if (t->payload_offset > t->size) return false;
        for (size_t i = 0; i < t->variant_count; i++) {
            if (!t->variants[i].payload || t->variants[i].payload->size > t->size - t->payload_offset) return false;
            if (!classify(t->variants[i].payload, offset + t->payload_offset, out, depth + 1)) return false;
        }
        return true;
    }
    if (t->kind == RE0_TYPE_STRUCT || t->kind == RE0_TYPE_TUPLE) {
        if (t->field_count > RE0_LAYOUT_MAX_FIELDS || (t->field_count && !t->fields)) return false;
        for (size_t i = 0; i < t->field_count; i++) {
            if (!t->fields[i].type || t->fields[i].offset > t->size ||
                t->fields[i].type->size > t->size - t->fields[i].offset) return false;
            if (!classify(t->fields[i].type, offset + t->fields[i].offset, out, depth + 1)) return false;
        }
        return true;
    }
    Re0AbiClass c = t->kind == RE0_TYPE_F32 || t->kind == RE0_TYPE_F64 ? RE0_ABI_SSE : RE0_ABI_INTEGER;
    for (size_t i = offset / 8; i <= (offset + t->size - 1) / 8; i++) {
        if (out->words[i] == RE0_ABI_NONE || c == RE0_ABI_INTEGER) out->words[i] = c;
    }
    return true;
}

bool re0_layout_sysv_classify(const Re0Layout *type, Re0SysvClass *result) {
    if (!type || !result || !power_two(type->align) || type->kind < RE0_TYPE_I8 ||
        type->kind > RE0_TYPE_GENERIC || type->kind == RE0_TYPE_UNKNOWN ||
        type->kind == RE0_TYPE_TYPEVAR || type->kind == RE0_TYPE_GENERIC) return false;
    memset(result, 0, sizeof(*result));
    if (!type->inhabited) return false;
    if (type->size > 16 || !classify(type, 0, result, 0)) {
        result->words[0] = RE0_ABI_MEMORY; result->words[1] = RE0_ABI_NONE;
        result->word_count = 0; result->indirect = true;
    } else result->word_count = (unsigned)((type->size + 7) / 8);
    return true;
}
