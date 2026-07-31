#include "safe.h"
/*
 * lsp_json.c — 精简 JSON 解析器实现
 */
#include "lsp_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── 解析器状态 ── */
#include "re0_limits.h"

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int depth;
} Parser;

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static JVal *parse_value(Parser *p);

static char *parse_string_raw(Parser *p) {
    if (p->pos >= p->len || p->src[p->pos] != '"') return NULL;
    p->pos++;
    size_t cap = 64, len = 0;
    char *buf = (char*)xmalloc(cap);
    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->src[p->pos++];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                default: c = esc; break;
            }
        }
        if (len >= RE0_MAX_JSON_STRING) {
            free(buf);
            return NULL;  /* 字符串超过上限 */
        }
        if (len + 1 >= cap) {
            cap = cap * 2;
            if (cap > RE0_MAX_JSON_STRING + 1) cap = RE0_MAX_JSON_STRING + 1;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = c;
    }
    if (p->pos < p->len) p->pos++; /* skip closing " */
    buf[len] = '\0';
    return buf;
}

static JVal *parse_object(Parser *p) {
    p->pos++; /* skip { */
    JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
    v->type = J_OBJ;
    v->obj.keys = NULL; v->obj.vals = NULL; v->obj.count = 0;
    int cap = 0;
    skip_ws(p);
    while (p->pos < p->len && p->src[p->pos] != '}') {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) { free(v); return NULL; }
        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ':') p->pos++;
        skip_ws(p);
        JVal *val = parse_value(p);
        if (!val) { free(key); free(v); return NULL; }
        if (v->obj.count >= cap) {
            cap = cap ? cap * 2 : 8;
            v->obj.keys = xrealloc(v->obj.keys, cap * sizeof(char*));
            v->obj.vals = xrealloc(v->obj.vals, cap * sizeof(JVal*));
        }
        v->obj.keys[v->obj.count] = key;
        v->obj.vals[v->obj.count] = val;
        v->obj.count++;
        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        skip_ws(p);
    }
    if (p->pos < p->len) p->pos++; /* skip } */
    return v;
}

static JVal *parse_array(Parser *p) {
    p->pos++; /* skip [ */
    JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
    v->type = J_ARR;
    v->arr.items = NULL; v->arr.count = 0;
    int cap = 0;
    skip_ws(p);
    while (p->pos < p->len && p->src[p->pos] != ']') {
        JVal *item = parse_value(p);
        if (!item) { free(v); return NULL; }
        if (v->arr.count >= cap) {
            cap = cap ? cap * 2 : 8;
            v->arr.items = xrealloc(v->arr.items, cap * sizeof(JVal*));
        }
        v->arr.items[v->arr.count++] = item;
        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        skip_ws(p);
    }
    if (p->pos < p->len) p->pos++; /* skip ] */
    return v;
}

static JVal *parse_value_impl(Parser *p);

static JVal *parse_value(Parser *p) {
    if (p->depth > RE0_MAX_JSON_DEPTH) return NULL;
    p->depth++;
    JVal *v = parse_value_impl(p);
    p->depth--;
    return v;
}

static JVal *parse_value_impl(Parser *p) {
    skip_ws(p);
    if (p->pos >= p->len) return NULL;
    char c = p->src[p->pos];
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') {
        JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
        v->type = J_STR;
        v->s = parse_string_raw(p);
        if (!v->s) { free(v); return NULL; }
        return v;
    }
    if (c == 't' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
        v->type = J_BOOL; v->b = true;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
        v->type = J_BOOL; v->b = false;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
        v->type = J_NULL;
        return v;
    }
    /* number */
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(p->src + p->pos, &end);
        size_t consumed = (size_t)(end - (p->src + p->pos));
        p->pos += consumed;
        JVal *v = (JVal*)xcalloc(1, sizeof(JVal));
        v->type = J_NUM; v->n = d;
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *text, size_t len) {
    Parser p = { text, 0, len, 0 };
    return parse_value(&p);
}

void json_free(JVal *v) {
    if (!v) return;
    switch (v->type) {
        case J_STR: free(v->s); break;
        case J_ARR:
            for (int i = 0; i < v->arr.count; i++) json_free(v->arr.items[i]);
            free(v->arr.items);
            break;
        case J_OBJ:
            for (int i = 0; i < v->obj.count; i++) {
                free(v->obj.keys[i]);
                json_free(v->obj.vals[i]);
            }
            free(v->obj.keys);
            free(v->obj.vals);
            break;
        default: break;
    }
    free(v);
}

JVal *json_get(JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->obj.count; i++)
        if (strcmp(obj->obj.keys[i], key) == 0) return obj->obj.vals[i];
    return NULL;
}

const char *json_str(JVal *v, const char *fallback) {
    if (!v || v->type != J_STR) return fallback;
    return v->s;
}

double json_num_val(JVal *v, double fallback) {
    if (!v || v->type != J_NUM) return fallback;
    return v->n;
}
