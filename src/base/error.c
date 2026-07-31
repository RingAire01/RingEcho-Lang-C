#include "safe.h"
#include "error.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void re0_error_list_init(Re0ErrorList *l) {
    Re0ErrorVec_init(&l->errors);
}

void re0_error_append(Re0ErrorList *l, Re0ErrorLevel lev, Re0Span sp,
                      const char *file, const char *fmt, ...) {
    Re0Error e;
    e.level = lev;
    e.span = sp;
    e.file = file;
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) need = 0;
    e.msg = (char*)malloc((size_t)need + 1);
    if (e.msg) {
        va_start(ap, fmt);
        vsnprintf(e.msg, (size_t)need + 1, fmt, ap);
        va_end(ap);
    }
    Re0ErrorVec_push(&l->errors, e);
}

bool re0_error_list_has_errors(Re0ErrorList *l) {
    if (!l) return false;
    for (size_t i = 0; i < Re0ErrorVec_len(&l->errors); i++) {
        if (l->errors.data[i].level != RE0_WARN) return true;
    }
    return false;
}

bool re0_error_list_has_warnings(Re0ErrorList *l) {
    if (!l) return false;
    for (size_t i = 0; i < Re0ErrorVec_len(&l->errors); i++) {
        if (l->errors.data[i].level == RE0_WARN) return true;
    }
    return false;
}

void re0_error_list_print(Re0ErrorList *l) {
    for (size_t i = 0; i < Re0ErrorVec_len(&l->errors); i++) {
        Re0Error *e = &l->errors.data[i];
        const char *tag = e->level == RE0_WARN ? "warning" : "error";
        fprintf(stderr, "%s:%zu:%zu: %s: %s\n",
                e->file ? e->file : "<unknown>",
                e->span.start.line + 1,
                e->span.start.column + 1,
                tag, e->msg ? e->msg : "unknown error");
    }
}

void re0_error_list_free(Re0ErrorList *l) {
    for (size_t i = 0; i < Re0ErrorVec_len(&l->errors); i++) {
        free(l->errors.data[i].msg);
        /* 注意：file 字段通常指向字符串常量或由调用者管理，不在此处释放。
         * 如果 file 是动态分配的，调用者应先释放或修改 re0_error_append 复制 file */
        /* free((void*)l->errors.data[i].file); */  /* 如果需要释放 file，取消注释此行 */
    }
    Re0ErrorVec_free(&l->errors);
}
