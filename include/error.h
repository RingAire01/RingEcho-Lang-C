#ifndef RE0_ERROR_H
#define RE0_ERROR_H
#include "span.h"
#include "vec.h"
typedef enum { RE0_ERR_SYNTAX, RE0_ERR_SEMANTIC, RE0_ERR_IO, RE0_ERR_INTERNAL, RE0_WARN } Re0ErrorLevel;
typedef struct { Re0ErrorLevel level; Re0Span span; const char *file; char *msg; } Re0Error;
VEC_DECLARE(Re0ErrorVec, Re0Error)
typedef struct { Re0ErrorVec errors; } Re0ErrorList;
void re0_error_list_init(Re0ErrorList *l);
void re0_error_append(Re0ErrorList *l, Re0ErrorLevel lev, Re0Span sp, const char *file, const char *fmt, ...);
bool re0_error_list_has_errors(Re0ErrorList *l);
void re0_error_list_print(Re0ErrorList *l);
void re0_error_list_free(Re0ErrorList *l);
#endif
