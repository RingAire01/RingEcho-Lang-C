#ifndef RE0_MODEL_H
#define RE0_MODEL_H
#include "base/types.h"
#include "base/vec.h"
#include <stdbool.h>

/* ── Field/variant definitions ── */
typedef struct { char *name; Re0Type *type; } Re0StructField;
typedef struct { char *name; Re0Type **types; int type_count; } Re0EnumVariant;

/* ── Struct definition ── */
typedef struct {
    char *name;
    Re0StructField *fields;
    int field_count;
    char **type_params;
    int type_param_count;
} Re0StructDef;

/* ── Enum definition ── */
typedef struct {
    char *name;
    char **variant_names;
    int *variant_has_payload;
    int variant_count;
} Re0EnumDef;

/* ── Trait method signature ── */
typedef struct {
    char *name;
    char **param_types;
    int param_count;
    char *ret_type;
} Re0TraitMethod;

/* ── Trait definition ── */
typedef struct {
    char *name;
    Re0TraitMethod *methods;
    int method_count;
} Re0TraitDef;

/* ── Impl block record: struct -> trait mapping ── */
typedef struct {
    char *struct_name;
    char *trait_name;       /* NULL = inherent impl */
} Re0ImplEntry;

/* ── Method dispatch entry: (struct, method) -> mangled symbol ── */
typedef struct {
    char *struct_name;
    char *method_name;
    char *mangled_symbol;
} Re0MethodEntry;

/* ── Type alias ── */
typedef struct {
    char *name;
    char *target;
} Re0TypeAlias;

/* ── Function signature ── */
typedef struct {
    char *name;
    char **param_types;
    int param_count;
    char *ret_type;
    char **type_params;
    int type_param_count;
} Re0FnSignature;

VEC_DECLARE(Re0StructFieldVec, Re0StructField)
VEC_DECLARE(Re0EnumVariantVec, Re0EnumVariant)
VEC_DECLARE(Re0StructDefVec, Re0StructDef)
VEC_DECLARE(Re0EnumDefVec, Re0EnumDef)
VEC_DECLARE(Re0TypeVec, Re0Type*)
VEC_DECLARE(Re0TraitDefVec, Re0TraitDef)
VEC_DECLARE(Re0ImplEntryVec, Re0ImplEntry)
VEC_DECLARE(Re0MethodEntryVec, Re0MethodEntry)
VEC_DECLARE(Re0TypeAliasVec, Re0TypeAlias)
VEC_DECLARE(Re0FnSignatureVec, Re0FnSignature)

typedef struct {
    Re0StructDefVec   struct_defs;
    Re0EnumDefVec     enum_defs;
    Re0TraitDefVec    trait_defs;
    Re0ImplEntryVec   impl_entries;
    Re0MethodEntryVec method_table;
    Re0TypeAliasVec   type_aliases;
    Re0FnSignatureVec fn_signatures;
} Re0SemanticModel;

void         re0_model_init(Re0SemanticModel *m);
void         re0_model_free(Re0SemanticModel *m);

void         re0_model_register_struct(Re0SemanticModel *m, const char *name,
                                       char **field_names, char **field_types, int n);
void         re0_model_register_enum(Re0SemanticModel *m, const char *name,
                                    char **variant_names, int *has_payload, int n);

/* ── Trait / Impl ── */
void         re0_model_register_trait(Re0SemanticModel *m, const char *name,
                                      Re0TraitMethod *methods, int method_count);
Re0TraitDef *re0_model_find_trait(Re0SemanticModel *m, const char *name);
void         re0_model_register_impl(Re0SemanticModel *m, const char *struct_name,
                                     const char *trait_name);
bool         re0_model_has_impl(Re0SemanticModel *m, const char *struct_name,
                                const char *trait_name);

/* ── Method dispatch table ── */
void         re0_model_register_method(Re0SemanticModel *m, const char *struct_name,
                                       const char *method_name, const char *mangled);
const char  *re0_model_lookup_method(Re0SemanticModel *m, const char *struct_name,
                                     const char *method_name);

/* ── Method symbol mangling ── */
/* trait=NULL -> "{struct}_{method}"; otherwise -> "{trait}_{struct}_{method}" */
const char  *re0_model_method_symbol(const char *trait, const char *struct_name,
                                     const char *method, char *out, size_t out_sz);

/* ── Type alias ── */
void         re0_model_register_type_alias(Re0SemanticModel *m, const char *name,
                                           const char *target);
const char  *re0_model_resolve_type_alias(Re0SemanticModel *m, const char *name);

/* ── Function signature ── */
void         re0_model_register_fn(Re0SemanticModel *m, const char *name,
                                   char **param_types, int param_count,
                                   const char *ret_type,
                                   char **type_params, int type_param_count);
Re0FnSignature *re0_model_find_fn(Re0SemanticModel *m, const char *name);

Re0StructDef *re0_model_find_struct(Re0SemanticModel *m, const char *name);
Re0EnumDef   *re0_model_find_enum(Re0SemanticModel *m, const char *name);
int           re0_model_variant_tag(Re0EnumDef *def, const char *variant_name);

Re0Type      *re0_model_std_type(const char *name);

#endif
