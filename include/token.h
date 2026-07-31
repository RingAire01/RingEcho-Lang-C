#ifndef RE0_TOKEN_H
#define RE0_TOKEN_H
#include "span.h"
#include <stdint.h>
#include <stdbool.h>
typedef enum {
    TK_IDENT, TK_NUMBER, TK_FLOAT, TK_STRING, TK_CHAR,
    TK_KW_FN, TK_KW_LET, TK_KW_MUT, TK_KW_IF, TK_KW_ELSE, TK_KW_FOR,
    TK_KW_WHILE, TK_KW_MATCH, TK_KW_RETURN, TK_KW_IMPORT, TK_KW_FROM,
    TK_KW_MODULE, TK_KW_PUB, TK_KW_STRUCT, TK_KW_ENUM, TK_KW_TRAIT,
    TK_KW_IMPL, TK_KW_AS, TK_KW_TYPE, TK_KW_SELF, TK_KW_TRUE, TK_KW_FALSE,
    TK_KW_BREAK, TK_KW_CONTINUE, TK_KW_CONST, TK_KW_EXTERN, TK_KW_COMPONENT,
    TK_KW_ASYNC, TK_KW_AWAIT, TK_KW_SPAWN,
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_COLON, TK_SEMICOLON, TK_DOUBLECOLON, TK_DOT, TK_DOUBLEDOT,
    TK_ELLIPSIS, TK_ARROW, TK_FATARROW, TK_PIPEARROW, TK_AT,
    TK_QUESTION, TK_DOUBLEQUESTION,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQUAL, TK_DOUBLEEQUAL, TK_BANGEQUAL,
    TK_GREATER, TK_GREATEREQUAL, TK_LESS, TK_LESSEQUAL,
    TK_RIGHTSHIFT, TK_LEFTSHIFT, TK_AMPERSAND, TK_DOUBLEAMPERSAND,
    TK_PIPE, TK_DOUBLEPIPE, TK_BANG, TK_CARET, TK_TILDE,
    TK_PLUSEQUAL, TK_MINUSEQUAL, TK_STAREQUAL, TK_SLASHEQUAL,
    TK_EOF, TK_ERROR,
} Re0TokenKind;

typedef struct {
    Re0TokenKind kind;
    Re0Span span;
    union { char *str_val; int64_t int_val; double float_val; char char_val; };
    char *lexeme;
} Re0Token;

const char *re0_token_kind_name(Re0TokenKind k);
bool        re0_token_is_keyword(Re0TokenKind k);
bool        re0_token_is_binop(Re0TokenKind k);
int         re0_token_binop_precedence(Re0TokenKind k);

#endif
