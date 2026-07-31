#include "safe.h"
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

size_t re0_type_sizeof_full(const Re0Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case RE0_TYPE_ARRAY:
            return t->array.inner
                ? re0_type_sizeof_full(t->array.inner) * t->array.size : 0;
        case RE0_TYPE_SLICE: return 16;   /* ptr + len */
        case RE0_TYPE_VEC:   return 24;   /* ptr + len + cap */
        case RE0_TYPE_TUPLE: {
            size_t sum = 0;
            for (int i = 0; i < t->tuple.count; i++)
                sum += re0_type_sizeof_full(t->tuple.elems[i]);
            return sum;
        }
        case RE0_TYPE_REFERENCE: return 8;
        case RE0_TYPE_TYPEVAR:   return 8;
        case RE0_TYPE_STRUCT: case RE0_TYPE_ENUM: case RE0_TYPE_FN:
        case RE0_TYPE_GENERIC: case RE0_TYPE_UNKNOWN:
            return 0;   /* 需 model 布局信息 */
        default:
            return re0_type_sizeof(t->kind);
    }
}

Re0Type *re0_type_make(Re0TypeKind k, void *arena) {
    Re0Type *t;
    if (arena) t = (Re0Type*)re0_arena_alloc_zero((Re0Arena*)arena, sizeof(Re0Type));
    else { t = (Re0Type*)xcalloc(1, sizeof(Re0Type)); }
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
        t->func.params = (Re0Type**)xmalloc(bytes);
    }
    if (!t->func.params) {
        /* 注意：在 arena 模式下，t 会在 arena 销毁时自动释放，无需手动 free */
        if (!arena) free(t);
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
        else       t->generic.args = (Re0Type**)xcalloc(1, bytes);
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

static bool re0_split_parse(const char *s, Re0Type **out, int *out_n, int max) {
    int n = 0;
    int sp = 0;
    char st[64];
    const char *start = s;
    const char *prev = NULL;
    const char *p = s;
    while (*p) {
        char c = *p;
        if (c == '<' || c == '[' || c == '(') {
            st[sp++] = c;
        } else if (c == '>') {
            if (prev && *prev == '-') { prev = p; p++; continue; }
            if (sp == 0) return false;
            sp--;
            if (st[sp] != '<') return false;
        } else if (c == ']') {
            if (sp == 0) return false;
            sp--;
            if (st[sp] != '[') return false;
        } else if (c == ')') {
            if (sp == 0) return false;
            sp--;
            if (st[sp] != '(') return false;
        } else if (c == ',' && sp == 0) {
            const char *a = start;
            const char *b = p;
            while (a < b && (*a == ' ' || *a == '\t')) a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
            if (b > a) {
                if (n >= max) return false;
                size_t sl = (size_t)(b - a);
                char *sub = (char*)malloc(sl + 1);
                if (!sub) return false;
                memcpy(sub, a, sl);
                sub[sl] = '\0';
                Re0Type *t = re0_type_parse(sub);
                free(sub);
                if (!t) return false;
                out[n++] = t;
            }
            start = p + 1;
        }
        prev = p;
        p++;
    }
    if (sp != 0) return false;
    {
        const char *a = start;
        const char *b = p;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (b > a) {
            if (n >= max) return false;
            size_t sl = (size_t)(b - a);
            char *sub = (char*)malloc(sl + 1);
            if (!sub) return false;
            memcpy(sub, a, sl);
            sub[sl] = '\0';
            Re0Type *t = re0_type_parse(sub);
            free(sub);
            if (!t) return false;
            out[n++] = t;
        }
    }
    *out_n = n;
    return true;
}

Re0Type *re0_type_parse(const char *s) {
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) return NULL;

    char *buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, s, len);
    buf[len] = '\0';

    if (strcmp(buf, "i8") == 0) { free(buf); return re0_type_make(RE0_TYPE_I8, NULL); }
    if (strcmp(buf, "i16") == 0) { free(buf); return re0_type_make(RE0_TYPE_I16, NULL); }
    if (strcmp(buf, "i32") == 0) { free(buf); return re0_type_make(RE0_TYPE_I32, NULL); }
    if (strcmp(buf, "i64") == 0) { free(buf); return re0_type_make(RE0_TYPE_I64, NULL); }
    if (strcmp(buf, "i128") == 0) { free(buf); return re0_type_make(RE0_TYPE_I128, NULL); }
    if (strcmp(buf, "isize") == 0) { free(buf); return re0_type_make(RE0_TYPE_ISIZE, NULL); }
    if (strcmp(buf, "u8") == 0) { free(buf); return re0_type_make(RE0_TYPE_U8, NULL); }
    if (strcmp(buf, "u16") == 0) { free(buf); return re0_type_make(RE0_TYPE_U16, NULL); }
    if (strcmp(buf, "u32") == 0) { free(buf); return re0_type_make(RE0_TYPE_U32, NULL); }
    if (strcmp(buf, "u64") == 0) { free(buf); return re0_type_make(RE0_TYPE_U64, NULL); }
    if (strcmp(buf, "u128") == 0) { free(buf); return re0_type_make(RE0_TYPE_U128, NULL); }
    if (strcmp(buf, "usize") == 0) { free(buf); return re0_type_make(RE0_TYPE_USIZE, NULL); }
    if (strcmp(buf, "f32") == 0) { free(buf); return re0_type_make(RE0_TYPE_F32, NULL); }
    if (strcmp(buf, "f64") == 0) { free(buf); return re0_type_make(RE0_TYPE_F64, NULL); }
    if (strcmp(buf, "bool") == 0) { free(buf); return re0_type_make(RE0_TYPE_BOOL, NULL); }
    if (strcmp(buf, "char") == 0) { free(buf); return re0_type_make(RE0_TYPE_CHAR, NULL); }
    if (strcmp(buf, "str") == 0 || strcmp(buf, "string") == 0) {
        free(buf);
        return re0_type_make(RE0_TYPE_STR, NULL);
    }
    if (strcmp(buf, "ptr") == 0) { free(buf); return re0_type_make(RE0_TYPE_PTR, NULL); }
    if (strcmp(buf, "unit") == 0 || strcmp(buf, "void") == 0) {
        free(buf);
        return re0_type_make(RE0_TYPE_UNIT, NULL);
    }
    if (strcmp(buf, "never") == 0 || strcmp(buf, "!") == 0) {
        free(buf);
        return re0_type_make(RE0_TYPE_NEVER, NULL);
    }
    if (strcmp(buf, "unknown") == 0 || strcmp(buf, "_") == 0) {
        free(buf);
        return re0_type_make(RE0_TYPE_UNKNOWN, NULL);
    }

    if (buf[0] == '&') {
        const char *rest = buf + 1;
        while (*rest == ' ' || *rest == '\t') rest++;
        bool is_mut = false;
        if (rest[0] == 'm' && rest[1] == 'u' && rest[2] == 't' &&
            (rest[3] == ' ' || rest[3] == '\t' || rest[3] == '\0')) {
            is_mut = true;
            rest += 3;
            while (*rest == ' ' || *rest == '\t') rest++;
        }
        if (*rest == '\0') { free(buf); return NULL; }
        Re0Type *inner = re0_type_parse(rest);
        if (!inner) { free(buf); return NULL; }
        Re0Type *r = re0_type_make_reference(inner, is_mut, NULL);
        free(buf);
        return r;
    }
    if (buf[0] == '*') { free(buf); return re0_type_make(RE0_TYPE_PTR, NULL); }

    if (len > 3 && memcmp(buf, "fn(", 3) == 0) {
        int depth = 0;
        const char *cp = buf + 3;
        const char *close = NULL;
        for (; *cp; cp++) {
            if (*cp == '(') depth++;
            else if (*cp == ')') {
                if (depth == 0) { close = cp; break; }
                depth--;
            }
        }
        if (!close) { free(buf); return NULL; }
        size_t pl = (size_t)(close - (buf + 3));
        char *params = (char*)malloc(pl + 1);
        if (!params) { free(buf); return NULL; }
        memcpy(params, buf + 3, pl);
        params[pl] = '\0';
        Re0Type *parr[32];
        int pn = 0;
        if (pl > 0) {
            if (!re0_split_parse(params, parr, &pn, 32)) { free(params); free(buf); return NULL; }
        }
        free(params);
        const char *rp = close + 1;
        while (*rp == ' ') rp++;
        Re0Type *ret;
        if (*rp == '-' && rp[1] == '>') {
            rp += 2;
            while (*rp == ' ') rp++;
            ret = re0_type_parse(rp);
            if (!ret) { free(buf); return NULL; }
        } else {
            ret = re0_type_make(RE0_TYPE_UNIT, NULL);
        }
        Re0Type *r = re0_type_make_func(parr, pn, ret, false, NULL);
        free(buf);
        return r;
    }

    if (buf[0] == '(' && buf[len - 1] == ')') {
        size_t il = len - 2;
        char *inner = (char*)malloc(il + 1);
        if (!inner) { free(buf); return NULL; }
        memcpy(inner, buf + 1, il);
        inner[il] = '\0';
        const char *ip = inner;
        while (*ip == ' ' || *ip == '\t') ip++;
        if (*ip == '\0') { free(inner); free(buf); return re0_type_make(RE0_TYPE_UNIT, NULL); }
        Re0Type *parr[32];
        int pn = 0;
        if (!re0_split_parse(inner, parr, &pn, 32)) { free(inner); free(buf); return NULL; }
        if (pn == 1 && inner[il - 1] != ',' && inner[il - 1] != ' ' && inner[il - 1] != '\t') {
            Re0Type *r = parr[0];
            free(inner);
            free(buf);
            return r;
        }
        Re0Type *r = re0_type_make_tuple(parr, pn, NULL);
        free(inner);
        free(buf);
        return r;
    }

    if (buf[0] == '[' && buf[len - 1] == ']') {
        size_t il = len - 2;
        char *inner = (char*)malloc(il + 1);
        if (!inner) { free(buf); return NULL; }
        memcpy(inner, buf + 1, il);
        inner[il] = '\0';
        char *is = inner;
        while (*is == ' ' || *is == '\t') is++;
        if (*is == '\0') { free(inner); free(buf); return NULL; }
        char *semi = NULL;
        for (char *q = is; *q; q++) if (*q == ';') semi = q;
        if (semi) {
            char *elem_end = semi;
            while (elem_end > is && (elem_end[-1] == ' ' || elem_end[-1] == '\t')) elem_end--;
            size_t el = (size_t)(elem_end - is);
            if (el == 0) { free(inner); free(buf); return NULL; }
            char *elem = (char*)malloc(el + 1);
            if (!elem) { free(inner); free(buf); return NULL; }
            memcpy(elem, is, el);
            elem[el] = '\0';
            Re0Type *et = re0_type_parse(elem);
            free(elem);
            if (!et) { free(inner); free(buf); return NULL; }
            char *sz = semi + 1;
            while (*sz == ' ' || *sz == '\t') sz++;
            if (*sz == '\0') { free(inner); free(buf); return re0_type_make_slice(et, NULL); }
            char *end;
            unsigned long long n = strtoull(sz, &end, 10);
            (void)n;
            while (*end == ' ' || *end == '\t') end++;
            if (*end != '\0') { free(inner); free(buf); return NULL; }
            Re0Type *r = re0_type_make_array(et, (size_t)n, NULL);
            free(inner);
            free(buf);
            return r;
        }
        Re0Type *et = re0_type_parse(is);
        if (!et) { free(inner); free(buf); return NULL; }
        Re0Type *r = re0_type_make_slice(et, NULL);
        free(inner);
        free(buf);
        return r;
    }

    {
        char *lt = strchr(buf, '<');
        if (lt && buf[len - 1] == '>') {
            size_t nl = (size_t)(lt - buf);
            if (nl == 0) { free(buf); return NULL; }
            char *name = (char*)malloc(nl + 1);
            if (!name) { free(buf); return NULL; }
            memcpy(name, buf, nl);
            name[nl] = '\0';
            char *ne = name + nl;
            while (ne > name && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            *ne = '\0';
            if (ne == name) { free(name); free(buf); return NULL; }
            size_t al = len - nl - 2;
            char *args = (char*)malloc(al + 1);
            if (!args) { free(name); free(buf); return NULL; }
            memcpy(args, lt + 1, al);
            args[al] = '\0';
            Re0Type *arr[32];
            int an = 0;
            if (al > 0) {
                if (!re0_split_parse(args, arr, &an, 32)) { free(args); free(name); free(buf); return NULL; }
            }
            free(args);
            if (strcmp(name, "Vec") == 0 && an == 1) {
                Re0Type *r = re0_type_make_vec(arr[0], NULL);
                free(name);
                free(buf);
                return r;
            }
            Re0Type *r = re0_type_make_generic(name, arr, an, NULL);
            free(name);
            free(buf);
            return r;
        }
        if (strchr(buf, '>')) { free(buf); return NULL; }
    }

    if (len == 1 && buf[0] >= 'A' && buf[0] <= 'Z') {
        Re0Type *r = re0_type_make_typevar(buf, NULL);
        free(buf);
        return r;
    }

    {
        Re0Type *r = re0_type_make_named(RE0_TYPE_STRUCT, buf, NULL);
        free(buf);
        return r;
    }
}
