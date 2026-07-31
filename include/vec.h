#ifndef RE0_VEC_H
#define RE0_VEC_H
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "re0_log.h"
#define VEC_DEFAULT_CAP 8
#define VEC_DECLARE(NAME, T) \
    typedef struct { T *data; size_t len; size_t cap; } NAME; \
    static inline void NAME##_init(NAME *v) { (v)->data = NULL; (v)->len = 0; (v)->cap = 0; } \
    static inline void NAME##_push(NAME *v, T item) { \
        if ((v)->len >= (v)->cap) { \
            size_t nc = (v)->cap ? (v)->cap * 2 : VEC_DEFAULT_CAP; \
            T *nd = (T*)realloc((v)->data, nc * sizeof(T)); \
            if (!nd) { re0_log(RE0_LOG_FATAL, "vec push OOM (%s)", #NAME); abort(); } \
            (v)->data = nd; (v)->cap = nc; \
        } \
        (v)->data[(v)->len++] = item; } \
    static inline T NAME##_pop(NAME *v) { \
        if ((v)->len == 0) { \
            re0_log(RE0_LOG_FATAL, "vec pop on empty vector (%s)", #NAME); \
            abort(); \
        } \
        return (v)->data[--(v)->len]; \
    } \
    static inline T NAME##_get(NAME *v, size_t i) { \
        if (i >= (v)->len) { re0_log(RE0_LOG_FATAL, "vec OOB access %zu >= %zu (%s)", i, (v)->len, #NAME); abort(); } \
        return (v)->data[i]; } \
    static inline void NAME##_set(NAME *v, size_t i, T item) { \
        if (i >= (v)->len) { re0_log(RE0_LOG_FATAL, "vec OOB set %zu >= %zu (%s)", i, (v)->len, #NAME); abort(); } \
        (v)->data[i] = item; } \
    static inline size_t NAME##_len(NAME *v) { return (v)->len; } \
    static inline bool NAME##_empty(NAME *v) { return (v)->len == 0; } \
    static inline T *NAME##_last(NAME *v) { return (v)->len ? &(v)->data[(v)->len - 1] : NULL; } \
    static inline void NAME##_free(NAME *v) { free((v)->data); (v)->data = NULL; (v)->len = 0; (v)->cap = 0; }
#endif
