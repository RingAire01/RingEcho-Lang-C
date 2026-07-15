#ifndef RE0_LEXER_H
#define RE0_LEXER_H
#include "arena.h"
#include "error.h"
#include "stream.h"

typedef struct {
    Re0Arena    *arena;
    Re0ErrorList *errors;
    Re0TokenStream stream;
    const char *source;
    size_t pos;
    size_t line;
    size_t column;
    size_t bol;
    const char *file_path;
    bool had_error;
} Re0Lexer;

void re0_lexer_init(Re0Lexer *l, Re0Arena *arena, Re0ErrorList *errors);
bool re0_lexer_tokenize(Re0Lexer *l, const char *source, const char *file_path);
void re0_lexer_destroy(Re0Lexer *l);

#endif
