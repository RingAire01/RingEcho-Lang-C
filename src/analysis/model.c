#include "model.h"
#include "arena.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void re0_model_init(Re0SemanticModel *m) {
    Re0StructDefVec_init(&m->struct_defs);
    Re0EnumDefVec_init(&m->enum_defs);
    Re0TraitDefVec_init(&m->trait_defs);
    Re0ImplEntryVec_init(&m->impl_entries);
    Re0MethodEntryVec_init(&m->method_table);
    Re0TypeAliasVec_init(&m->type_aliases);
    Re0FnSignatureVec_init(&m->fn_signatures);
}

/* ════════════════════════════════════════
 *  Struct / Enum 注册（现有功能，增加 type_params）
 * ════════════════════════════════════════ */

void re0_model_register_struct(Re0SemanticModel *m, const char *name,
                               char **field_names, char **field_types, int n) {
    Re0StructDef def;
    memset(&def, 0, sizeof(def));
    def.name = strdup(name);
    def.fields = (Re0StructField*)calloc((size_t)n, sizeof(Re0StructField));
    def.field_count = n;
    def.type_params = NULL;
    def.type_param_count = 0;
    for (int i = 0; i < n; i++) {
        def.fields[i].name = strdup(field_names[i]);
        def.fields[i].type = re0_model_std_type(field_types ? field_types[i] : "i64");
    }
    Re0StructDefVec_push(&m->struct_defs, def);
}

void re0_model_register_enum(Re0SemanticModel *m, const char *name,
                            char **variant_names, int *has_payload, int n) {
    Re0EnumDef def;
    def.name = strdup(name);
    def.variant_names = (char**)calloc((size_t)n, sizeof(char*));
    def.variant_has_payload = (int*)calloc((size_t)n, sizeof(int));
    def.variant_count = n;
    for (int i = 0; i < n; i++) {
        def.variant_names[i] = strdup(variant_names[i]);
        def.variant_has_payload[i] = has_payload ? has_payload[i] : 0;
    }
    Re0EnumDefVec_push(&m->enum_defs, def);
}

Re0StructDef *re0_model_find_struct(Re0SemanticModel *m, const char *name) {
    for (size_t i = 0; i < Re0StructDefVec_len(&m->struct_defs); i++)
        if (strcmp(m->struct_defs.data[i].name, name) == 0)
            return &m->struct_defs.data[i];
    return NULL;
}

Re0EnumDef *re0_model_find_enum(Re0SemanticModel *m, const char *name) {
    for (size_t i = 0; i < Re0EnumDefVec_len(&m->enum_defs); i++)
        if (strcmp(m->enum_defs.data[i].name, name) == 0)
            return &m->enum_defs.data[i];
    return NULL;
}

int re0_model_variant_tag(Re0EnumDef *def, const char *variant_name) {
    if (!def) return -1;
    for (int i = 0; i < def->variant_count; i++)
        if (strcmp(def->variant_names[i], variant_name) == 0) return i;
    return -1;
}

/* ════════════════════════════════════════
 *  Trait 注册与查询
 * ════════════════════════════════════════ */

void re0_model_register_trait(Re0SemanticModel *m, const char *name,
                              Re0TraitMethod *methods, int method_count) {
    Re0TraitDef def;
    def.name = strdup(name);
    def.method_count = method_count;
    def.methods = (Re0TraitMethod*)calloc((size_t)(method_count > 0 ? method_count : 1),
                                          sizeof(Re0TraitMethod));
    for (int i = 0; i < method_count; i++) {
        def.methods[i].name = strdup(methods[i].name);
        def.methods[i].ret_type = methods[i].ret_type ? strdup(methods[i].ret_type) : NULL;
        def.methods[i].param_count = methods[i].param_count;
        if (methods[i].param_count > 0) {
            def.methods[i].param_types = (char**)calloc((size_t)methods[i].param_count,
                                                         sizeof(char*));
            for (int j = 0; j < methods[i].param_count; j++)
                def.methods[i].param_types[j] = methods[i].param_types[j]
                    ? strdup(methods[i].param_types[j]) : NULL;
        }
    }
    Re0TraitDefVec_push(&m->trait_defs, def);
}

Re0TraitDef *re0_model_find_trait(Re0SemanticModel *m, const char *name) {
    if (!m || !name) return NULL;
    for (size_t i = 0; i < Re0TraitDefVec_len(&m->trait_defs); i++)
        if (strcmp(m->trait_defs.data[i].name, name) == 0)
            return &m->trait_defs.data[i];
    return NULL;
}

/* ════════════════════════════════════════
 *  Impl 注册与查询
 * ════════════════════════════════════════ */

void re0_model_register_impl(Re0SemanticModel *m, const char *struct_name,
                             const char *trait_name) {
    Re0ImplEntry entry;
    entry.struct_name = strdup(struct_name);
    entry.trait_name = trait_name ? strdup(trait_name) : NULL;
    Re0ImplEntryVec_push(&m->impl_entries, entry);
}

bool re0_model_has_impl(Re0SemanticModel *m, const char *struct_name,
                        const char *trait_name) {
    for (size_t i = 0; i < Re0ImplEntryVec_len(&m->impl_entries); i++) {
        Re0ImplEntry *e = &m->impl_entries.data[i];
        if (strcmp(e->struct_name, struct_name) != 0) continue;
        if (!trait_name || !e->trait_name) {
            if (!trait_name && !e->trait_name) return true;
            if (trait_name && e->trait_name && strcmp(e->trait_name, trait_name) == 0)
                return true;
        } else {
            if (strcmp(e->trait_name, trait_name) == 0) return true;
        }
    }
    return false;
}

/* ════════════════════════════════════════
 *  方法派发表
 * ════════════════════════════════════════ */

void re0_model_register_method(Re0SemanticModel *m, const char *struct_name,
                               const char *method_name, const char *mangled) {
    Re0MethodEntry entry;
    entry.struct_name = strdup(struct_name);
    entry.method_name = strdup(method_name);
    entry.mangled_symbol = strdup(mangled);
    Re0MethodEntryVec_push(&m->method_table, entry);
}

const char *re0_model_lookup_method(Re0SemanticModel *m, const char *struct_name,
                                    const char *method_name) {
    for (size_t i = 0; i < Re0MethodEntryVec_len(&m->method_table); i++) {
        Re0MethodEntry *e = &m->method_table.data[i];
        if (strcmp(e->struct_name, struct_name) == 0 &&
            strcmp(e->method_name, method_name) == 0)
            return e->mangled_symbol;
    }
    return NULL;
}

/* ════════════════════════════════════════
 *  方法符号 mangling
 * ════════════════════════════════════════ */

const char *re0_model_method_symbol(const char *trait, const char *struct_name,
                                    const char *method) {
    static char buf[512];
    if (trait)
        snprintf(buf, sizeof(buf), "%s_%s_%s", trait, struct_name, method);
    else
        snprintf(buf, sizeof(buf), "%s_%s", struct_name, method);
    return buf;
}

/* ════════════════════════════════════════
 *  类型别名
 * ════════════════════════════════════════ */

void re0_model_register_type_alias(Re0SemanticModel *m, const char *name,
                                   const char *target) {
    Re0TypeAlias alias;
    alias.name = strdup(name);
    alias.target = strdup(target);
    Re0TypeAliasVec_push(&m->type_aliases, alias);
}

const char *re0_model_resolve_type_alias(Re0SemanticModel *m, const char *name) {
    if (!m || !name) return NULL;
    for (size_t i = 0; i < Re0TypeAliasVec_len(&m->type_aliases); i++)
        if (strcmp(m->type_aliases.data[i].name, name) == 0)
            return m->type_aliases.data[i].target;
    return NULL;
}

/* ════════════════════════════════════════
 *  函数签名
 * ════════════════════════════════════════ */

void re0_model_register_fn(Re0SemanticModel *m, const char *name,
                           char **param_types, int param_count,
                           const char *ret_type,
                           char **type_params, int type_param_count) {
    Re0FnSignature sig;
    memset(&sig, 0, sizeof(sig));
    sig.name = strdup(name);
    sig.param_count = param_count;
    sig.ret_type = ret_type ? strdup(ret_type) : NULL;
    sig.type_param_count = type_param_count;

    if (param_count > 0 && param_types) {
        sig.param_types = (char**)calloc((size_t)param_count, sizeof(char*));
        for (int i = 0; i < param_count; i++)
            sig.param_types[i] = strdup(param_types[i]);
    }
    if (type_param_count > 0 && type_params) {
        sig.type_params = (char**)calloc((size_t)type_param_count, sizeof(char*));
        for (int i = 0; i < type_param_count; i++)
            sig.type_params[i] = strdup(type_params[i]);
    }
    Re0FnSignatureVec_push(&m->fn_signatures, sig);
}

Re0FnSignature *re0_model_find_fn(Re0SemanticModel *m, const char *name) {
    if (!m || !name) return NULL;
    for (size_t i = 0; i < Re0FnSignatureVec_len(&m->fn_signatures); i++)
        if (strcmp(m->fn_signatures.data[i].name, name) == 0)
            return &m->fn_signatures.data[i];
    return NULL;
}

/* ════════════════════════════════════════
 *  标准类型
 * ════════════════════════════════════════ */

Re0Type *re0_model_std_type(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "i8") == 0) return re0_type_make(RE0_TYPE_I8, NULL);
    if (strcmp(name, "i16") == 0) return re0_type_make(RE0_TYPE_I16, NULL);
    if (strcmp(name, "i32") == 0) return re0_type_make(RE0_TYPE_I32, NULL);
    if (strcmp(name, "i64") == 0) return re0_type_make(RE0_TYPE_I64, NULL);
    if (strcmp(name, "u8") == 0) return re0_type_make(RE0_TYPE_U8, NULL);
    if (strcmp(name, "u16") == 0) return re0_type_make(RE0_TYPE_U16, NULL);
    if (strcmp(name, "u32") == 0) return re0_type_make(RE0_TYPE_U32, NULL);
    if (strcmp(name, "u64") == 0) return re0_type_make(RE0_TYPE_U64, NULL);
    if (strcmp(name, "f64") == 0) return re0_type_make(RE0_TYPE_F64, NULL);
    if (strcmp(name, "f32") == 0) return re0_type_make(RE0_TYPE_F32, NULL);
    if (strcmp(name, "bool") == 0) return re0_type_make(RE0_TYPE_BOOL, NULL);
    if (strcmp(name, "char") == 0) return re0_type_make(RE0_TYPE_CHAR, NULL);
    if (strcmp(name, "str") == 0) return re0_type_make(RE0_TYPE_STR, NULL);
    if (strcmp(name, "ptr") == 0) return re0_type_make(RE0_TYPE_PTR, NULL);
    if (strcmp(name, "unit") == 0) return re0_type_make(RE0_TYPE_UNIT, NULL);
    if (strcmp(name, "never") == 0) return re0_type_make(RE0_TYPE_NEVER, NULL);
    return NULL;
}

/* ════════════════════════════════════════
 *  释放
 * ════════════════════════════════════════ */

void re0_model_free(Re0SemanticModel *m) {
    /* struct defs */
    for (size_t i = 0; i < Re0StructDefVec_len(&m->struct_defs); i++) {
        free(m->struct_defs.data[i].name);
        for (int j = 0; j < m->struct_defs.data[i].field_count; j++)
            free(m->struct_defs.data[i].fields[j].name);
        free(m->struct_defs.data[i].fields);
        if (m->struct_defs.data[i].type_params) {
            for (int j = 0; j < m->struct_defs.data[i].type_param_count; j++)
                free(m->struct_defs.data[i].type_params[j]);
            free(m->struct_defs.data[i].type_params);
        }
    }
    Re0StructDefVec_free(&m->struct_defs);

    /* enum defs */
    for (size_t i = 0; i < Re0EnumDefVec_len(&m->enum_defs); i++) {
        free(m->enum_defs.data[i].name);
        for (int j = 0; j < m->enum_defs.data[i].variant_count; j++)
            free(m->enum_defs.data[i].variant_names[j]);
        free(m->enum_defs.data[i].variant_names);
        free(m->enum_defs.data[i].variant_has_payload);
    }
    Re0EnumDefVec_free(&m->enum_defs);

    /* trait defs */
    for (size_t i = 0; i < Re0TraitDefVec_len(&m->trait_defs); i++) {
        Re0TraitDef *td = &m->trait_defs.data[i];
        free(td->name);
        for (int j = 0; j < td->method_count; j++) {
            free(td->methods[j].name);
            free(td->methods[j].ret_type);
            for (int k = 0; k < td->methods[j].param_count; k++)
                free(td->methods[j].param_types[k]);
            free(td->methods[j].param_types);
        }
        free(td->methods);
    }
    Re0TraitDefVec_free(&m->trait_defs);

    /* impl entries */
    for (size_t i = 0; i < Re0ImplEntryVec_len(&m->impl_entries); i++) {
        free(m->impl_entries.data[i].struct_name);
        free(m->impl_entries.data[i].trait_name);
    }
    Re0ImplEntryVec_free(&m->impl_entries);

    /* method table */
    for (size_t i = 0; i < Re0MethodEntryVec_len(&m->method_table); i++) {
        free(m->method_table.data[i].struct_name);
        free(m->method_table.data[i].method_name);
        free(m->method_table.data[i].mangled_symbol);
    }
    Re0MethodEntryVec_free(&m->method_table);

    /* type aliases */
    for (size_t i = 0; i < Re0TypeAliasVec_len(&m->type_aliases); i++) {
        free(m->type_aliases.data[i].name);
        free(m->type_aliases.data[i].target);
    }
    Re0TypeAliasVec_free(&m->type_aliases);

    /* fn signatures */
    for (size_t i = 0; i < Re0FnSignatureVec_len(&m->fn_signatures); i++) {
        Re0FnSignature *fs = &m->fn_signatures.data[i];
        free(fs->name);
        free(fs->ret_type);
        if (fs->param_types) {
            for (int j = 0; j < fs->param_count; j++)
                free(fs->param_types[j]);
            free(fs->param_types);
        }
        if (fs->type_params) {
            for (int j = 0; j < fs->type_param_count; j++)
                free(fs->type_params[j]);
            free(fs->type_params);
        }
    }
    Re0FnSignatureVec_free(&m->fn_signatures);
}
