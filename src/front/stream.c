#include "stream.h"
#include <stdlib.h>
#include <stdio.h>

void re0_stream_init(Re0TokenStream *s) {
    Re0TokenVec_init(&s->tokens);
    s->cursor = 0;
}

void re0_stream_push(Re0TokenStream *s, Re0Token t) {
    Re0TokenVec_push(&s->tokens, t);
}

Re0Token *re0_stream_peek(Re0TokenStream *s) {
    if (s->cursor >= Re0TokenVec_len(&s->tokens)) return NULL;
    return &s->tokens.data[s->cursor];
}

Re0Token re0_stream_advance(Re0TokenStream *s) {
    Re0Token t = { TK_EOF, RE0_SPAN_ZERO, {0}, NULL };
    if (s->cursor < Re0TokenVec_len(&s->tokens))
        t = s->tokens.data[s->cursor++];
    return t;
}

bool re0_stream_check(Re0TokenStream *s, Re0TokenKind k) {
    Re0Token *t = re0_stream_peek(s);
    return t && t->kind == k;
}

Re0Token re0_stream_expect(Re0TokenStream *s, Re0TokenKind k) {
    if (re0_stream_check(s, k)) return re0_stream_advance(s);
    Re0Token t = { TK_ERROR, RE0_SPAN_ZERO, {0}, NULL };
    return t;
}

bool re0_stream_eof(Re0TokenStream *s) {
    Re0Token *t = re0_stream_peek(s);
    return !t || t->kind == TK_EOF;
}

size_t re0_stream_pos(Re0TokenStream *s) {
    return s->cursor;
}

void re0_stream_set_pos(Re0TokenStream *s, size_t pos) {
    if (pos <= Re0TokenVec_len(&s->tokens)) s->cursor = pos;
}

void re0_stream_free(Re0TokenStream *s) {
    Re0TokenVec_free(&s->tokens);
    s->cursor = 0;
}
