#ifndef RE0_PARSER_H
#define RE0_PARSER_H
#include "arena.h"
#include "error.h"
#include "stream.h"
#include "ast.h"
#include "re0_limits.h"

typedef struct {
    Re0Arena      *arena;
    Re0ErrorList  *errors;
    Re0TokenStream *stream;
    Re0StmtVec     stmts;
    int            depth;
    bool           had_error;
    bool           had_any_error;
} Re0Parser;

void re0_parser_init(Re0Parser *p, Re0Arena *arena, Re0ErrorList *errors);
bool re0_parser_parse(Re0Parser *p, Re0TokenStream *stream);
void re0_parser_destroy(Re0Parser *p);

#endif
