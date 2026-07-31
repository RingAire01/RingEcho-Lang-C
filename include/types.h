#ifndef RE0_TYPES_H
#define RE0_TYPES_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    RE0_TYPE_I8, RE0_TYPE_I16, RE0_TYPE_I32, RE0_TYPE_I64, RE0_TYPE_I128, RE0_TYPE_ISIZE,
    RE0_TYPE_U8, RE0_TYPE_U16, RE0_TYPE_U32, RE0_TYPE_U64, RE0_TYPE_U128, RE0_TYPE_USIZE,
    RE0_TYPE_F32, RE0_TYPE_F64,
    RE0_TYPE_BOOL, RE0_TYPE_CHAR, RE0_TYPE_STR, RE0_TYPE_PTR,
    RE0_TYPE_UNIT, RE0_TYPE_NEVER, RE0_TYPE_UNKNOWN,
    RE0_TYPE_ARRAY, RE0_TYPE_SLICE, RE0_TYPE_VEC, RE0_TYPE_TUPLE,
    RE0_TYPE_STRUCT, RE0_TYPE_ENUM,
    RE0_TYPE_FN, RE0_TYPE_REFERENCE,
    RE0_TYPE_TYPEVAR, RE0_TYPE_GENERIC,
} Re0TypeKind;

typedef struct Re0Type {
    Re0TypeKind kind;
    union {
        struct { struct Re0Type *inner; size_t size; } array;
        struct { struct Re0Type *inner; } slice;
        struct { struct Re0Type *inner; } vec;
        struct { struct Re0Type **elems; int count; } tuple;
        struct { char *name; } named;
        struct { struct Re0Type *inner; bool mutable_; } ref_;
        struct {
            struct Re0Type *ret;
            struct Re0Type **params;
            int param_count;
            bool variadic;
        } func;
        struct { char *name; struct Re0Type **args; int arg_count; } generic;
    };
} Re0Type;

const char *re0_type_kind_name(Re0TypeKind k);
bool        re0_type_is_integer(Re0TypeKind k);
bool        re0_type_is_float(Re0TypeKind k);
bool        re0_type_is_numeric(Re0TypeKind k);
bool        re0_type_is_signed(Re0TypeKind k);
size_t      re0_type_sizeof(Re0TypeKind k);
/* 递归计算完整类型尺寸（复合类型：Array=elem×n、Slice=16、Vec=24、Tuple=Σ）。
 * Struct/Enum/Fn/Generic 需 model 布局信息，保守返回 0。 */
size_t      re0_type_sizeof_full(const Re0Type *t);
Re0Type    *re0_type_make(Re0TypeKind k, void *arena);
Re0Type    *re0_type_make_array(Re0Type *inner, size_t n, void *arena);
Re0Type    *re0_type_make_tuple(Re0Type **elems, int n, void *arena);
Re0Type    *re0_type_make_func(Re0Type **params, int n, Re0Type *ret,
                               bool variadic, void *arena);

Re0Type    *re0_type_make_slice(Re0Type *inner, void *arena);
Re0Type    *re0_type_make_vec(Re0Type *inner, void *arena);
Re0Type    *re0_type_make_reference(Re0Type *inner, bool mutable_, void *arena);
Re0Type    *re0_type_make_named(Re0TypeKind kind, const char *name, void *arena);
Re0Type    *re0_type_make_generic(const char *name, Re0Type **args,
                                  int arg_count, void *arena);
Re0Type    *re0_type_make_typevar(const char *name, void *arena);

Re0Type    *re0_type_parse(const char *s);

/* ── 结构化类型比较 ──
 * 递归比较两个类型的结构。
 * NULL == NULL → true；任一为 NULL → false。
 * STRUCT/ENUM 按 name 比较；GENERIC 按 name + args 比较；余按结构递归。
 */
bool        re0_type_equal(const Re0Type *a, const Re0Type *b);

/* 类型是否可互相赋值（from → to）。
 * 目前：equal 或 numeric 隐式转换。
 */
bool        re0_type_coercible(const Re0Type *from, const Re0Type *to);

#endif
