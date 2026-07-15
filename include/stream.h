#ifndef RE0_STREAM_H
#define RE0_STREAM_H
#include "token.h"
#include "vec.h"
VEC_DECLARE(Re0TokenVec, Re0Token)
typedef struct {
    Re0TokenVec tokens;
    size_t cursor;
} Re0TokenStream;
void           re0_stream_init(Re0TokenStream *s);
void           re0_stream_push(Re0TokenStream *s, Re0Token t);
Re0Token      *re0_stream_peek(Re0TokenStream *s);
Re0Token       re0_stream_advance(Re0TokenStream *s);
bool           re0_stream_check(Re0TokenStream *s, Re0TokenKind k);
Re0Token       re0_stream_expect(Re0TokenStream *s, Re0TokenKind k);
bool           re0_stream_eof(Re0TokenStream *s);
size_t         re0_stream_pos(Re0TokenStream *s);
void           re0_stream_set_pos(Re0TokenStream *s, size_t pos);
void           re0_stream_free(Re0TokenStream *s);
#endif
