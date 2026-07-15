#ifndef RE0_VEC_H
#define RE0_VEC_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define VEC_DEFAULT_CAP 8
#define VEC_DECLARE(NAME, T) \
    typedef struct { T *data; size_t len; size_t cap; } NAME; \
    static inline void NAME##_init(NAME *v) { (v)->data = NULL; (v)->len = 0; (v)->cap = 0; } \
    static inline void NAME##_push(NAME *v, T item) { \
        if ((v)->len >= (v)->cap) { size_t nc = (v)->cap ? (v)->cap * 2 : VEC_DEFAULT_CAP; (v)->data = (T*)realloc((v)->data, nc * sizeof(T)); (v)->cap = nc; } \
        (v)->data[(v)->len++] = item; } \
    static inline T NAME##_pop(NAME *v) { return (v)->data[--(v)->len]; } \
    static inline T NAME##_get(NAME *v, size_t i) { return (v)->data[i]; } \
    static inline void NAME##_set(NAME *v, size_t i, T item) { (v)->data[i] = item; } \
    static inline size_t NAME##_len(NAME *v) { return (v)->len; } \
    static inline bool NAME##_empty(NAME *v) { return (v)->len == 0; } \
    static inline T *NAME##_last(NAME *v) { return (v)->len ? &(v)->data[(v)->len - 1] : NULL; } \
    static inline void NAME##_free(NAME *v) { free((v)->data); (v)->data = NULL; (v)->len = 0; (v)->cap = 0; }
#endif
