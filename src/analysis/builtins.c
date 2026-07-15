#include "builtins.h"
#include "types.h"
#include "model.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

static void add_builtin(Re0BuiltinRegistry *r, const char *name, const char *ret,
                         const char **pnames, const char **ptypes, int n) {
    Re0BuiltinFn fn;
    fn.name = strdup(name);
    fn.ret_type = re0_model_std_type(ret);
    fn.params = NULL;
    fn.param_count = n;
    if (n > 0) {
        fn.params = (Re0BuiltinParam*)calloc((size_t)n, sizeof(Re0BuiltinParam));
        for (int i = 0; i < n; i++) {
            fn.params[i].name = strdup(pnames[i]);
            fn.params[i].type = re0_model_std_type(ptypes[i]);
        }
    }
    Re0BuiltinVec_push(&r->fns, fn);
}

void re0_builtin_init(Re0BuiltinRegistry *r) {
    Re0BuiltinVec_init(&r->fns);
    const char *str_s[] = {"s"}; const char *str_t[] = {"str"};
    const char *ab_s[] = {"a","b"}; const char *ab_t[] = {"str","str"};
    const char *si_s[] = {"s","i"}; const char *si_t[] = {"str","i64"};
    const char *sse_s[] = {"s","start","end"}; const char *sse_t[] = {"str","i64","i64"};
    const char *vx_s[] = {"v","x"}; const char *vx_t[] = {"vec","i64"};
    const char *vix_s[] = {"v","i","x"}; const char *vix_t[] = {"vec","i64","i64"};
    const char *fd_s[] = {"f","d"}; const char *fd_t[] = {"str","str"};
    const char *c_s[] = {"cond"}; const char *c_t[] = {"bool"};
    add_builtin(r, "println", "unit", str_s, str_t, 1);
    add_builtin(r, "print", "unit", str_s, str_t, 1);
    add_builtin(r, "panic", "never", str_s, str_t, 1);
    add_builtin(r, "assert", "unit", c_s, c_t, 1);
    add_builtin(r, "str_len", "i64", str_s, str_t, 1);
    add_builtin(r, "str_char_at", "char", si_s, si_t, 2);
    add_builtin(r, "str_slice", "str", sse_s, sse_t, 3);
    add_builtin(r, "str_concat", "str", ab_s, ab_t, 2);
    add_builtin(r, "str_eq", "bool", ab_s, ab_t, 2);
    add_builtin(r, "str_to_int", "i64", str_s, str_t, 1);
    add_builtin(r, "vec_new", "vec", NULL, NULL, 0);
    add_builtin(r, "vec_push", "unit", vx_s, vx_t, 2);
    add_builtin(r, "vec_get", "i64", si_s, si_t, 2);
    add_builtin(r, "vec_set", "unit", vix_s, vix_t, 3);
    add_builtin(r, "vec_pop", "i64", str_s, str_t, 1);
    add_builtin(r, "vec_last", "i64", str_s, str_t, 1);
    add_builtin(r, "vec_len", "i64", str_s, str_t, 1);
    add_builtin(r, "file_read", "str", str_s, str_t, 1);
    add_builtin(r, "file_write", "unit", fd_s, fd_t, 2);
    add_builtin(r, "char_to_str", "str", (const char*[]){"c"}, (const char*[]){"char"}, 1);
    add_builtin(r, "to_string", "str", str_s, str_t, 1);
    add_builtin(r, "is_digit", "bool", (const char*[]){"c"}, (const char*[]){"char"}, 1);
    add_builtin(r, "is_alpha", "bool", (const char*[]){"c"}, (const char*[]){"char"}, 1);
    add_builtin(r, "is_alnum", "bool", (const char*[]){"c"}, (const char*[]){"char"}, 1);
    add_builtin(r, "free", "unit", str_s, str_t, 1);
    add_builtin(r, "exit", "never", (const char*[]){"code"}, (const char*[]){"i64"}, 1);
    add_builtin(r, "argv_len", "i64", NULL, NULL, 0);
    add_builtin(r, "argv_get", "str", si_s, si_t, 1);
    add_builtin(r, "stdin_read", "str", NULL, NULL, 0);
    /* GC API builtins */
    add_builtin(r, "gc_collect", "unit", NULL, NULL, 0);
    add_builtin(r, "gc_stats", "i64", NULL, NULL, 0);
    add_builtin(r, "gc_add_root", "unit",
                (const char*[]){"p"}, (const char*[]){"ptr"}, 1);
    add_builtin(r, "gc_remove_root", "unit",
                (const char*[]){"p"}, (const char*[]){"ptr"}, 1);
}

Re0BuiltinFn *re0_builtin_lookup(Re0BuiltinRegistry *r, const char *name) {
    for (size_t i = 0; i < Re0BuiltinVec_len(&r->fns); i++)
        if (strcmp(r->fns.data[i].name, name) == 0) return &r->fns.data[i];
    return NULL;
}

void re0_builtin_free(Re0BuiltinRegistry *r) {
    for (size_t i = 0; i < Re0BuiltinVec_len(&r->fns); i++) {
        free(r->fns.data[i].name);
        for (int j = 0; j < r->fns.data[i].param_count; j++)
            free(r->fns.data[i].params[j].name);
        free(r->fns.data[i].params);
    }
    Re0BuiltinVec_free(&r->fns);
}
