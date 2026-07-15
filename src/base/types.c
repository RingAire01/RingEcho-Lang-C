#include "types.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

const char *re0_type_kind_name(Re0TypeKind k) {
    switch (k) {
        case RE0_TYPE_I8: return "i8";
        case RE0_TYPE_I16: return "i16";
        case RE0_TYPE_I32: return "i32";
        case RE0_TYPE_I64: return "i64";
        case RE0_TYPE_I128: return "i128";
        case RE0_TYPE_ISIZE: return "isize";
        case RE0_TYPE_U8: return "u8";
        case RE0_TYPE_U16: return "u16";
        case RE0_TYPE_U32: return "u32";
        case RE0_TYPE_U64: return "u64";
        case RE0_TYPE_U128: return "u128";
        case RE0_TYPE_USIZE: return "usize";
        case RE0_TYPE_F32: return "f32";
        case RE0_TYPE_F64: return "f64";
        case RE0_TYPE_BOOL: return "bool";
        case RE0_TYPE_CHAR: return "char";
        case RE0_TYPE_STR: return "str";
        case RE0_TYPE_PTR: return "ptr";
        case RE0_TYPE_UNIT: return "unit";
        case RE0_TYPE_NEVER: return "never";
        case RE0_TYPE_ARRAY: return "array";
        case RE0_TYPE_SLICE: return "slice";
        case RE0_TYPE_VEC: return "vec";
        case RE0_TYPE_TUPLE: return "tuple";
        case RE0_TYPE_STRUCT: return "struct";
        case RE0_TYPE_ENUM: return "enum";
        case RE0_TYPE_FN: return "fn";
        case RE0_TYPE_REFERENCE: return "ref";
        case RE0_TYPE_TYPEVAR: return "typevar";
        case RE0_TYPE_GENERIC: return "generic";
        case RE0_TYPE_UNKNOWN: return "unknown";
        default: return "?";
    }
}

bool re0_type_is_integer(Re0TypeKind k) {
    return k >= RE0_TYPE_I8 && k <= RE0_TYPE_USIZE;
}

bool re0_type_is_float(Re0TypeKind k) {
    return k == RE0_TYPE_F32 || k == RE0_TYPE_F64;
}

bool re0_type_is_numeric(Re0TypeKind k) {
    return re0_type_is_integer(k) || re0_type_is_float(k);
}

bool re0_type_is_signed(Re0TypeKind k) {
    return k >= RE0_TYPE_I8 && k <= RE0_TYPE_ISIZE;
}

size_t re0_type_sizeof(Re0TypeKind k) {
    switch (k) {
        case RE0_TYPE_I8: case RE0_TYPE_U8: return 1;
        case RE0_TYPE_I16: case RE0_TYPE_U16: return 2;
        case RE0_TYPE_I32: case RE0_TYPE_U32: return 4;
        case RE0_TYPE_I64: case RE0_TYPE_U64: case RE0_TYPE_ISIZE: case RE0_TYPE_USIZE: return 8;
        case RE0_TYPE_F32: return 4;
        case RE0_TYPE_F64: return 8;
        case RE0_TYPE_BOOL: case RE0_TYPE_CHAR: return 1;
        case RE0_TYPE_STR: case RE0_TYPE_PTR: return 8;
        default: return 8;
    }
}

Re0Type *re0_type_make(Re0TypeKind k, void *arena) {
    Re0Type *t;
    if (arena) t = (Re0Type*)re0_arena_alloc_zero((Re0Arena*)arena, sizeof(Re0Type));
    else { t = (Re0Type*)calloc(1, sizeof(Re0Type)); }
    if (t) t->kind = k;
    return t;
}

Re0Type *re0_type_make_array(Re0Type *inner, size_t n, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_ARRAY, arena);
    if (t) { t->array.inner = inner; t->array.size = n; }
    return t;
}

Re0Type *re0_type_make_tuple(Re0Type **elems, int n, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_TUPLE, arena);
    if (t) { t->tuple.elems = elems; t->tuple.count = n; }
    return t;
}

Re0Type *re0_type_make_func(Re0Type **params, int n, Re0Type *ret,
                            bool variadic, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_FN, arena);
    if (!t) return NULL;

    t->func.params = NULL;
    t->func.param_count = n;
    t->func.ret = ret;
    t->func.variadic = variadic;
    if (n <= 0) return t;

    size_t bytes = (size_t)n * sizeof(*params);
    if (arena) {
        t->func.params = (Re0Type**)re0_arena_alloc_zero((Re0Arena*)arena, bytes);
    } else {
        t->func.params = (Re0Type**)malloc(bytes);
    }
    if (!t->func.params) {
        free(t);
        return NULL;
    }
    memcpy(t->func.params, params, bytes);
    return t;
}

Re0Type *re0_type_make_slice(Re0Type *inner, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_SLICE, arena);
    if (t) t->slice.inner = inner;
    return t;
}

Re0Type *re0_type_make_vec(Re0Type *inner, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_VEC, arena);
    if (t) t->vec.inner = inner;
    return t;
}

Re0Type *re0_type_make_reference(Re0Type *inner, bool mutable_, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_REFERENCE, arena);
    if (t) { t->ref_.inner = inner; t->ref_.mutable_ = mutable_; }
    return t;
}

Re0Type *re0_type_make_named(Re0TypeKind kind, const char *name, void *arena) {
    Re0Type *t = re0_type_make(kind, arena);
    if (!t || !name) return t;
    if (arena) t->named.name = re0_arena_strdup((Re0Arena*)arena, name);
    else       t->named.name = strdup(name);
    return t;
}

Re0Type *re0_type_make_generic(const char *name, Re0Type **args,
                                int arg_count, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_GENERIC, arena);
    if (!t) return NULL;
    if (name) {
        if (arena) t->generic.name = re0_arena_strdup((Re0Arena*)arena, name);
        else       t->generic.name = strdup(name);
    }
    t->generic.arg_count = arg_count;
    if (arg_count > 0 && args) {
        size_t bytes = (size_t)arg_count * sizeof(Re0Type*);
        if (arena) t->generic.args = (Re0Type**)re0_arena_alloc_zero((Re0Arena*)arena, bytes);
        else       t->generic.args = (Re0Type**)calloc(1, bytes);
        if (t->generic.args) memcpy(t->generic.args, args, bytes);
    }
    return t;
}

Re0Type *re0_type_make_typevar(const char *name, void *arena) {
    Re0Type *t = re0_type_make(RE0_TYPE_TYPEVAR, arena);
    if (!t || !name) return t;
    if (arena) t->named.name = re0_arena_strdup((Re0Arena*)arena, name);
    else       t->named.name = strdup(name);
    return t;
}

bool re0_type_equal(const Re0Type *a, const Re0Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case RE0_TYPE_I8: case RE0_TYPE_I16: case RE0_TYPE_I32:
        case RE0_TYPE_I64: case RE0_TYPE_I128: case RE0_TYPE_ISIZE:
        case RE0_TYPE_U8: case RE0_TYPE_U16: case RE0_TYPE_U32:
        case RE0_TYPE_U64: case RE0_TYPE_U128: case RE0_TYPE_USIZE:
        case RE0_TYPE_F32: case RE0_TYPE_F64:
        case RE0_TYPE_BOOL: case RE0_TYPE_CHAR: case RE0_TYPE_STR:
        case RE0_TYPE_PTR: case RE0_TYPE_UNIT: case RE0_TYPE_NEVER:
        case RE0_TYPE_UNKNOWN:
            return true;

        case RE0_TYPE_ARRAY:
            return re0_type_equal(a->array.inner, b->array.inner)
                && a->array.size == b->array.size;

        case RE0_TYPE_SLICE:
            return re0_type_equal(a->slice.inner, b->slice.inner);

        case RE0_TYPE_VEC:
            return re0_type_equal(a->vec.inner, b->vec.inner);

        case RE0_TYPE_TUPLE: {
            if (a->tuple.count != b->tuple.count) return false;
            for (int i = 0; i < a->tuple.count; i++)
                if (!re0_type_equal(a->tuple.elems[i], b->tuple.elems[i]))
                    return false;
            return true;
        }

        case RE0_TYPE_STRUCT:
        case RE0_TYPE_ENUM:
            return a->named.name && b->named.name
                ? strcmp(a->named.name, b->named.name) == 0
                : a->named.name == b->named.name;

        case RE0_TYPE_REFERENCE:
            return a->ref_.mutable_ == b->ref_.mutable_
                && re0_type_equal(a->ref_.inner, b->ref_.inner);

        case RE0_TYPE_FN: {
            if (a->func.param_count != b->func.param_count) return false;
            if (a->func.variadic != b->func.variadic) return false;
            if (!re0_type_equal(a->func.ret, b->func.ret)) return false;
            for (int i = 0; i < a->func.param_count; i++)
                if (!re0_type_equal(a->func.params[i], b->func.params[i]))
                    return false;
            return true;
        }

        case RE0_TYPE_GENERIC: {
            if (a->generic.arg_count != b->generic.arg_count) return false;
            if (a->generic.name && b->generic.name) {
                if (strcmp(a->generic.name, b->generic.name) != 0) return false;
            } else if (a->generic.name != b->generic.name) return false;
            for (int i = 0; i < a->generic.arg_count; i++)
                if (!re0_type_equal(a->generic.args[i], b->generic.args[i]))
                    return false;
            return true;
        }

        case RE0_TYPE_TYPEVAR:
            return a->named.name && b->named.name
                ? strcmp(a->named.name, b->named.name) == 0
                : a->named.name == b->named.name;
    }
    return false;
}

bool re0_type_coercible(const Re0Type *from, const Re0Type *to) {
    if (re0_type_equal(from, to)) return true;
    if (!from || !to) return false;
    /* numeric 隐式转换（整数之间、浮点之间、整数→浮点） */
    if (re0_type_is_numeric(from->kind) && re0_type_is_numeric(to->kind))
        return true;
    /* UNKNOWN 可赋值到任何类型（推断未完成时的宽容策略） */
    if (from->kind == RE0_TYPE_UNKNOWN || to->kind == RE0_TYPE_UNKNOWN)
        return true;
    return false;
}
