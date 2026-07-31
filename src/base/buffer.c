#include "buffer.h"
#include "safe.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

void re0_buffer_init(Re0Buffer *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = false;
}

bool re0_buffer_reserve(Re0Buffer *b, size_t extra) {
    if (!b || b->failed) return false;
    if (extra > SIZE_MAX - b->len) {
        b->failed = true;
        return false;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 256;
    if (nc < b->cap) nc = need;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { nc = need; break; }
        nc *= 2;
    }
    char *nd = (char*)xrealloc(b->data, nc);
    if (!nd) {
        b->failed = true;
        return false;
    }
    b->data = nd;
    b->cap = nc;
    return true;
}

void re0_buffer_write_char(Re0Buffer *b, char c) {
    if (!re0_buffer_reserve(b, 1)) return;
    b->data[b->len++] = c;
}

void re0_buffer_write_str(Re0Buffer *b, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (!re0_buffer_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

void re0_buffer_write_n(Re0Buffer *b, const char *s, size_t n) {
    if (!s || !n) return;
    if (!re0_buffer_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

void re0_buffer_write_fmt(Re0Buffer *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    re0_buffer_write_vfmt(b, fmt, ap);
    va_end(ap);
}

void re0_buffer_write_vfmt(Re0Buffer *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0) return;
    if (!re0_buffer_reserve(b, (size_t)need + 1)) return;
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    b->len += (size_t)need;
}

void re0_buffer_write_indent(Re0Buffer *b, int depth) {
    for (int i = 0; i < depth; i++) re0_buffer_write_str(b, "    ");
}

void re0_buffer_clear(Re0Buffer *b) {
    b->len = 0;
}

void re0_buffer_free(Re0Buffer *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = false;
}

bool re0_buffer_failed(const Re0Buffer *b) { return !b || b->failed; }
