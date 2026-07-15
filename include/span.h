#ifndef RE0_SPAN_H
#define RE0_SPAN_H
#include <stddef.h>
typedef struct { size_t line; size_t column; size_t offset; } Re0Pos;
typedef struct { Re0Pos start; Re0Pos end; } Re0Span;
#define RE0_SPAN_ZERO ((Re0Span){{0,0,0},{0,0,0}})
Re0Pos  re0_pos_make(size_t line, size_t col, size_t offset);
Re0Span re0_span_make(Re0Pos start, Re0Pos end);
#endif
