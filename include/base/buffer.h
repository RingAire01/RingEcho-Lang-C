#ifndef RE0_BUFFER_H
#define RE0_BUFFER_H
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
typedef struct { char *data; size_t len; size_t cap; bool failed; } Re0Buffer;
void re0_buffer_init(Re0Buffer *b);
bool re0_buffer_reserve(Re0Buffer *b, size_t extra);
void re0_buffer_write_char(Re0Buffer *b, char c);
void re0_buffer_write_str(Re0Buffer *b, const char *s);
void re0_buffer_write_n(Re0Buffer *b, const char *s, size_t n);
void re0_buffer_write_fmt(Re0Buffer *b, const char *fmt, ...);
void re0_buffer_write_vfmt(Re0Buffer *b, const char *fmt, va_list ap);
void re0_buffer_write_indent(Re0Buffer *b, int depth);
void re0_buffer_clear(Re0Buffer *b);
void re0_buffer_free(Re0Buffer *b);
bool re0_buffer_failed(const Re0Buffer *b);
#endif
