#include "token.h"
#include <string.h>

static const char *kw_names[] = {
    [TK_KW_FN]="fn", [TK_KW_LET]="let", [TK_KW_MUT]="mut",
    [TK_KW_IF]="if", [TK_KW_ELSE]="else", [TK_KW_FOR]="for",
    [TK_KW_WHILE]="while", [TK_KW_MATCH]="match", [TK_KW_RETURN]="return",
    [TK_KW_IMPORT]="import", [TK_KW_FROM]="from", [TK_KW_MODULE]="module",
    [TK_KW_PUB]="pub", [TK_KW_STRUCT]="struct", [TK_KW_ENUM]="enum",
    [TK_KW_TRAIT]="trait", [TK_KW_IMPL]="impl", [TK_KW_AS]="as",
    [TK_KW_TYPE]="type", [TK_KW_SELF]="self", [TK_KW_TRUE]="true",
    [TK_KW_FALSE]="false", [TK_KW_BREAK]="break", [TK_KW_CONTINUE]="continue",
    [TK_KW_CONST]="const", [TK_KW_EXTERN]="extern", [TK_KW_COMPONENT]="component",
    [TK_KW_ASYNC]="async", [TK_KW_AWAIT]="await", [TK_KW_SPAWN]="spawn",
};

static const char *sym_names[] = {
    [TK_LBRACE]="{", [TK_RBRACE]="}", [TK_LPAREN]="(", [TK_RPAREN]=")",
    [TK_LBRACKET]="[", [TK_RBRACKET]="]", [TK_COMMA]=",", [TK_COLON]=":",
    [TK_SEMICOLON]=";", [TK_DOUBLECOLON]="::", [TK_DOT]=".",
    [TK_DOUBLEDOT]="..", [TK_ELLIPSIS]="...", [TK_ARROW]="->",
    [TK_FATARROW]="=>", [TK_PIPEARROW]="|>", [TK_AT]="@",
    [TK_QUESTION]="?", [TK_DOUBLEQUESTION]="??",
    [TK_PLUS]="+", [TK_MINUS]="-", [TK_STAR]="*", [TK_SLASH]="/",
    [TK_PERCENT]="%", [TK_EQUAL]="=", [TK_DOUBLEEQUAL]="==",
    [TK_BANGEQUAL]="!=", [TK_GREATER]=">", [TK_GREATEREQUAL]=">=",
    [TK_LESS]="<", [TK_LESSEQUAL]="<=", [TK_RIGHTSHIFT]=">>",
    [TK_LEFTSHIFT]="<<", [TK_AMPERSAND]="&", [TK_DOUBLEAMPERSAND]="&&",
    [TK_PIPE]="|", [TK_DOUBLEPIPE]="||", [TK_BANG]="!",
    [TK_CARET]="^", [TK_TILDE]="~",
    [TK_PLUSEQUAL]="+=", [TK_MINUSEQUAL]="-=", [TK_STAREQUAL]="*=",
    [TK_SLASHEQUAL]="/=",
};

const char *re0_token_kind_name(Re0TokenKind k) {
    if (k >= TK_KW_FN && k <= TK_KW_SPAWN) return kw_names[k];
    if (k >= TK_LBRACE && k <= TK_SLASHEQUAL) return sym_names[k];
    switch (k) {
        case TK_IDENT: return "identifier";
        case TK_NUMBER: return "number";
        case TK_FLOAT: return "float";
        case TK_STRING: return "string";
        case TK_CHAR: return "char";
        case TK_EOF: return "EOF";
        case TK_ERROR: return "error";
        default: return "unknown";
    }
}

bool re0_token_is_keyword(Re0TokenKind k) {
    return k >= TK_KW_FN && k <= TK_KW_SPAWN;
}

bool re0_token_is_binop(Re0TokenKind k) {
    return k == TK_DOUBLEDOT ||
           k == TK_CARET ||
           (k >= TK_PLUS && k <= TK_BANG) ||
           (k >= TK_PLUSEQUAL && k <= TK_SLASHEQUAL);
}

int re0_token_binop_precedence(Re0TokenKind k) {
    switch (k) {
        case TK_DOUBLEDOT: return 1;
        case TK_DOUBLEPIPE: return 1;
        case TK_DOUBLEAMPERSAND: return 2;
        case TK_DOUBLEEQUAL: case TK_BANGEQUAL: return 3;
        case TK_LESS: case TK_LESSEQUAL: case TK_GREATER: case TK_GREATEREQUAL: return 4;
        case TK_PLUS: case TK_MINUS: return 5;
        case TK_STAR: case TK_SLASH: case TK_PERCENT: return 6;
        case TK_PIPE: case TK_RIGHTSHIFT: case TK_LEFTSHIFT: return 7;
        case TK_AMPERSAND: case TK_CARET: return 8;
        default: return 0;
    }
}
