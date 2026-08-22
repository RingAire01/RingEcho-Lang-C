/*
 * lsp_json.h — minimal JSON parser (LSP only)
 */
#ifndef RE0_LSP_JSON_H
#define RE0_LSP_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    union {
        bool b;
        double n;
        char *s;
        struct { struct JVal **items; int count; } arr;
        struct { char **keys; struct JVal **vals; int count; } obj;
    };
} JVal;

/* parse a JSON string, return the root value (NULL on failure) */
JVal *json_parse(const char *text, size_t len);

/* free a JSON value tree */
void json_free(JVal *v);

/* look up a key in an object (returns NULL if absent) */
JVal *json_get(JVal *obj, const char *key);

/* get string/number value (returns fallback on type mismatch) */
const char *json_str(JVal *v, const char *fallback);
double json_num_val(JVal *v, double fallback);

#endif
