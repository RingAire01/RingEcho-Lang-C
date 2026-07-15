/*
 * lsp_json.h — 精简 JSON 解析器（仅供 LSP 使用）
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

/* 解析 JSON 字符串，返回根值（失败返回 NULL） */
JVal *json_parse(const char *text, size_t len);

/* 释放 JSON 值树 */
void json_free(JVal *v);

/* 从对象中按键查找（不存在返回 NULL） */
JVal *json_get(JVal *obj, const char *key);

/* 获取字符串/数字值（类型不匹配返回默认值） */
const char *json_str(JVal *v, const char *fallback);
double json_num_val(JVal *v, double fallback);

#endif
