#include "base/safe.h"
#include "front/lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define RE0_INITIAL_STRING_CAPACITY 64u
#define RE0_MAX_STRING_BYTES (16u * 1024u * 1024u)
#define RE0_NUMBER_BUF_CAPACITY 512

void re0_lexer_init(Re0Lexer *l, Re0Arena *arena, Re0ErrorList *errors) {
    l->arena = arena;
    l->errors = errors;
    re0_stream_init(&l->stream);
    l->source = NULL;
    l->pos = 0;
    l->line = 0;
    l->column = 0;
    l->bol = 0;
    l->file_path = NULL;
    l->had_error = false;
}

static char peek(Re0Lexer *l) { return l->source[l->pos]; }
static char peek_n(Re0Lexer *l, int n) { return l->source[l->pos + n]; }
static char advance(Re0Lexer *l) { return l->source[l->pos++]; }

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_part(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_oct_digit(char c) { return c >= '0' && c <= '7'; }
static bool is_bin_digit(char c) { return c == '0' || c == '1'; }

static Re0Pos cur_pos(Re0Lexer *l) {
    return re0_pos_make(l->line, l->column, l->pos);
}

static Re0Token make_token(Re0Lexer *l, Re0TokenKind kind, Re0Pos start) {
    Re0Token t = {0};
    t.kind = kind;
    t.span = re0_span_make(start, cur_pos(l));
    t.str_val = NULL;
    t.int_val = 0;
    t.float_val = 0.0;
    t.char_val = 0;
    t.lexeme = NULL;
    t.suffix = NULL;
    return t;
}

static Re0Token make_id(Re0Lexer *l, char *lexeme, Re0Pos start) {
    Re0Token t = make_token(l, TK_IDENT, start);
    t.str_val = re0_arena_strdup(l->arena, lexeme);
    t.lexeme = t.str_val;
    if (strcmp(lexeme, "fn") == 0) t.kind = TK_KW_FN;
    else if (strcmp(lexeme, "let") == 0) t.kind = TK_KW_LET;
    else if (strcmp(lexeme, "mut") == 0) t.kind = TK_KW_MUT;
    else if (strcmp(lexeme, "if") == 0) t.kind = TK_KW_IF;
    else if (strcmp(lexeme, "else") == 0) t.kind = TK_KW_ELSE;
    else if (strcmp(lexeme, "for") == 0) t.kind = TK_KW_FOR;
    else if (strcmp(lexeme, "while") == 0) t.kind = TK_KW_WHILE;
    else if (strcmp(lexeme, "match") == 0) t.kind = TK_KW_MATCH;
    else if (strcmp(lexeme, "return") == 0) t.kind = TK_KW_RETURN;
    else if (strcmp(lexeme, "import") == 0) t.kind = TK_KW_IMPORT;
    else if (strcmp(lexeme, "from") == 0) t.kind = TK_KW_FROM;
    else if (strcmp(lexeme, "module") == 0) t.kind = TK_KW_MODULE;
    else if (strcmp(lexeme, "pub") == 0) t.kind = TK_KW_PUB;
    else if (strcmp(lexeme, "struct") == 0) t.kind = TK_KW_STRUCT;
    else if (strcmp(lexeme, "enum") == 0) t.kind = TK_KW_ENUM;
    else if (strcmp(lexeme, "trait") == 0) t.kind = TK_KW_TRAIT;
    else if (strcmp(lexeme, "impl") == 0) t.kind = TK_KW_IMPL;
    else if (strcmp(lexeme, "as") == 0) t.kind = TK_KW_AS;
    else if (strcmp(lexeme, "type") == 0) t.kind = TK_KW_TYPE;
    else if (strcmp(lexeme, "self") == 0) t.kind = TK_KW_SELF;
    else if (strcmp(lexeme, "true") == 0) { t.kind = TK_KW_TRUE; t.int_val = 1; }
    else if (strcmp(lexeme, "false") == 0) t.kind = TK_KW_FALSE;
    else if (strcmp(lexeme, "break") == 0) t.kind = TK_KW_BREAK;
    else if (strcmp(lexeme, "continue") == 0) t.kind = TK_KW_CONTINUE;
    else if (strcmp(lexeme, "const") == 0) t.kind = TK_KW_CONST;
    else if (strcmp(lexeme, "extern") == 0) t.kind = TK_KW_EXTERN;
    else if (strcmp(lexeme, "component") == 0) t.kind = TK_KW_COMPONENT;
    else if (strcmp(lexeme, "async") == 0) t.kind = TK_KW_ASYNC;
    else if (strcmp(lexeme, "await") == 0) t.kind = TK_KW_AWAIT;
    else if (strcmp(lexeme, "spawn") == 0) t.kind = TK_KW_SPAWN;
    return t;
}

static void skip_line_comment(Re0Lexer *l) {
    while (peek(l) && peek(l) != '\n') advance(l);
}

static void skip_block_comment(Re0Lexer *l) {
    int depth = 1;
    while (depth > 0 && peek(l)) {
        if (peek(l) == '/' && peek_n(l, 1) == '*') { advance(l); advance(l); depth++; }
        else if (peek(l) == '*' && peek_n(l, 1) == '/') { advance(l); advance(l); depth--; }
        else if (peek(l) == '\n') { l->line++; l->column = 0; l->bol = l->pos + 1; advance(l); }
        else advance(l);
    }
}

static void skip_whitespace(Re0Lexer *l) {
    while (peek(l)) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r') { advance(l); l->column++; continue; }
        if (c == '\n') { l->line++; l->column = 0; l->bol = l->pos + 1; advance(l); continue; }
        if (c == '/' && peek_n(l, 1) == '/') { skip_line_comment(l); continue; }
        if (c == '/' && peek_n(l, 1) == '*') { advance(l); advance(l); skip_block_comment(l); continue; }
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Number scanning — with bounds-checked buffer & suffix preservation */
/* ------------------------------------------------------------------ */

/* Emit a diagnostic if the number buffer overflows. */
static bool num_buf_check(Re0Lexer *l, int bi, int cap, Re0Pos start) {
    if (bi >= cap - 1) {
        re0_error_append(l->errors, RE0_ERR_SYNTAX,
                         re0_span_make(start, cur_pos(l)), l->file_path,
                         "numeric literal too long (max %d digits)", cap - 2);
        l->had_error = true;
        return false;  /* signal: stop scanning */
    }
    return true;
}

static void parse_integer_token(Re0Lexer *l, Re0Token *t, const char *text, unsigned radix) {
    if (!re0_integer_parse(text, radix, &t->integer)) {
        re0_error_append(l->errors, RE0_ERR_SYNTAX, t->span, l->file_path,
                         "integer literal is invalid or exceeds 128 bits");
        l->had_error = true;
        t->kind = TK_ERROR;
        return;
    }
    uint64_t low = t->integer.low;
    if (t->integer.negative) low = UINT64_C(0) - low;
    t->int_val = low <= INT64_MAX ? (int64_t)low : -1 - (int64_t)(UINT64_MAX - low);
}

static Re0Token scan_number(Re0Lexer *l, Re0Pos start, bool negative, char lead) {
    char buf[RE0_NUMBER_BUF_CAPACITY];
    int length = 0;
    if (negative) buf[length++] = '-';
    unsigned radix = 10;
    if (lead) buf[length++] = lead;
    else if (peek(l) == '0') buf[length++] = advance(l);
    if (length > 0 && buf[length - 1] == '0') {
        char prefix = peek(l);
        if (prefix == 'x' || prefix == 'X') radix = 16;
        else if (prefix == 'b' || prefix == 'B') radix = 2;
        else if (prefix == 'o' || prefix == 'O') radix = 8;
        if (radix != 10) { advance(l); length--; }
    }
    int digit_start = negative ? 1 : 0;
    while (peek(l)) {
        char c = peek(l);
        bool digit = radix == 16 ? is_hex_digit(c) : radix == 8 ? is_oct_digit(c) :
                     radix == 2 ? is_bin_digit(c) : is_digit(c);
        if (!digit && c != '_') break;
        if (c == '_') { advance(l); continue; }
        if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start))
            return make_token(l, TK_ERROR, start);
        buf[length++] = advance(l);
    }
    bool floating = false;
    if (radix == 10 && peek(l) == '.' && peek_n(l, 1) != '.') {
        floating = true;
        if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start)) return make_token(l, TK_ERROR, start);
        buf[length++] = advance(l);
        while (is_digit(peek(l)) || peek(l) == '_') {
            if (peek(l) == '_') { advance(l); continue; }
            if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start)) return make_token(l, TK_ERROR, start);
            buf[length++] = advance(l);
        }
    }
    if (radix == 10 && (peek(l) == 'e' || peek(l) == 'E')) {
        floating = true;
        if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start)) return make_token(l, TK_ERROR, start);
        buf[length++] = advance(l);
        if (peek(l) == '+' || peek(l) == '-') {
            if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start)) return make_token(l, TK_ERROR, start);
            buf[length++] = advance(l);
        }
        while (is_digit(peek(l))) {
            if (!num_buf_check(l, length, RE0_NUMBER_BUF_CAPACITY, start)) return make_token(l, TK_ERROR, start);
            buf[length++] = advance(l);
        }
    }
    buf[length] = 0;
    char *suffix = NULL;
    if (peek(l) == 'u' || peek(l) == 'i' || peek(l) == 'f') {
        int begin = l->pos;
        while (is_ident_part(peek(l))) advance(l);
        size_t size = (size_t)(l->pos - begin);
        suffix = re0_arena_alloc(l->arena, size + 1);
        if (!suffix) {
            re0_error_append(l->errors, RE0_ERR_INTERNAL, re0_span_make(start, cur_pos(l)),
                             l->file_path, "cannot allocate numeric suffix");
            l->had_error = true;
            return make_token(l, TK_ERROR, start);
        }
        memcpy(suffix, l->source + begin, size); suffix[size] = 0;
        if (suffix[0] == 'f') floating = true;
    }
    Re0Token token = make_token(l, floating ? TK_FLOAT : TK_NUMBER, start);
    token.suffix = suffix;
    if (length == digit_start) {
        re0_error_append(l->errors, RE0_ERR_SYNTAX, token.span, l->file_path, "missing digits in numeric literal");
        l->had_error = true; token.kind = TK_ERROR;
    } else if (floating) {
        char *end = NULL;
        errno = 0;
        token.float_val = strtod(buf, &end);
        if (radix != 10 || !end || *end || errno == ERANGE) {
            re0_error_append(l->errors, RE0_ERR_SYNTAX, token.span, l->file_path, "invalid or out-of-range floating literal");
            l->had_error = true; token.kind = TK_ERROR;
        }
    } else parse_integer_token(l, &token, buf, radix);
    return token;
}

static Re0Token scan_string(Re0Lexer *l, Re0Pos start) {
    size_t cap = RE0_INITIAL_STRING_CAPACITY;
    size_t len = 0;
    char *buf = (char*)xmalloc(cap);
    if (!buf) {
        re0_error_append(l->errors, RE0_ERR_INTERNAL, re0_span_make(start, cur_pos(l)),
                         l->file_path, "out of memory while scanning string literal");
        l->had_error = true;
        return make_token(l, TK_ERROR, start);
    }
    while (peek(l) && peek(l) != '"') {
        char value;
        if (peek(l) == '\\') {
            advance(l);
            switch (peek(l)) {
                case 'n': value = '\n'; break;
                case 't': value = '\t'; break;
                case 'r': value = '\r'; break;
                case '\\': value = '\\'; break;
                case '"': value = '"'; break;
                case '0':
                    re0_error_append(l->errors, RE0_ERR_SYNTAX,
                                     re0_span_make(start, cur_pos(l)), l->file_path,
                                     "null bytes are not supported in C-backed string literals");
                    l->had_error = true;
                    free(buf);
                    return make_token(l, TK_ERROR, start);
                default:
                    re0_error_append(l->errors, RE0_ERR_SYNTAX,
                                     re0_span_make(start, cur_pos(l)), l->file_path,
                                     "unknown string escape sequence '\\%c'", peek(l));
                    l->had_error = true;
                    free(buf);
                    return make_token(l, TK_ERROR, start);
            }
            advance(l);
        } else {
            value = advance(l);
        }
        if (len >= RE0_MAX_STRING_BYTES) {
            re0_error_append(l->errors, RE0_ERR_SYNTAX,
                             re0_span_make(start, cur_pos(l)), l->file_path,
                             "string literal exceeds %u bytes", RE0_MAX_STRING_BYTES);
            l->had_error = true;
            free(buf);
            return make_token(l, TK_ERROR, start);
        }
        if (len + 1 >= cap) {
            size_t next_cap = cap > RE0_MAX_STRING_BYTES / 2 ? RE0_MAX_STRING_BYTES + 1 : cap * 2;
            char *next = (char*)xrealloc(buf, next_cap);
            if (!next) {
                re0_error_append(l->errors, RE0_ERR_INTERNAL,
                                 re0_span_make(start, cur_pos(l)), l->file_path,
                                 "out of memory while growing string literal");
                l->had_error = true;
                free(buf);
                return make_token(l, TK_ERROR, start);
            }
            buf = next;
            cap = next_cap;
        }
        buf[len++] = value;
    }
    if (peek(l) != '"') {
        re0_error_append(l->errors, RE0_ERR_SYNTAX, re0_span_make(start, cur_pos(l)),
                         l->file_path, "unterminated string literal");
        l->had_error = true;
        free(buf);
        return make_token(l, TK_ERROR, start);
    }
    advance(l);
    buf[len] = '\0';
    Re0Token t = make_token(l, TK_STRING, start);
    t.str_val = re0_arena_strdup(l->arena, buf);
    free(buf);
    if (!t.str_val) {
        re0_error_append(l->errors, RE0_ERR_INTERNAL, t.span, l->file_path,
                         "out of memory while storing string literal");
        l->had_error = true;
        t.kind = TK_ERROR;
    }
    return t;
}

static Re0Token scan_char(Re0Lexer *l, Re0Pos start) {
    char c = 0;

    /* Unterminated detection */
    if (peek(l) == '\0') {
        re0_error_append(l->errors, RE0_ERR_SYNTAX,
                         re0_span_make(start, cur_pos(l)), l->file_path,
                         "unterminated char literal");
        l->had_error = true;
        return make_token(l, TK_ERROR, start);
    }

    if (peek(l) == '\\') {
        advance(l);
        char esc = peek(l);
        if (esc == '\0') {
            re0_error_append(l->errors, RE0_ERR_SYNTAX,
                             re0_span_make(start, cur_pos(l)), l->file_path,
                             "unterminated escape in char literal");
            l->had_error = true;
            return make_token(l, TK_ERROR, start);
        }
        switch (esc) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '0': c = '\0'; break;
            default: c = esc; break;
        }
        advance(l);
    } else {
        c = advance(l);
    }

    if (peek(l) != '\'') {
        re0_error_append(l->errors, RE0_ERR_SYNTAX,
                         re0_span_make(start, cur_pos(l)), l->file_path,
                         "expected closing quote in char literal");
        l->had_error = true;
    } else {
        advance(l);
    }

    Re0Token t = make_token(l, TK_CHAR, start);
    t.char_val = c;
    return t;
}

static Re0Token scan_token(Re0Lexer *l) {
    skip_whitespace(l);
    if (!peek(l)) return make_token(l, TK_EOF, cur_pos(l));

    Re0Pos start = cur_pos(l);
    char c = advance(l);
    l->column++;

    if (is_ident_start(c)) {
        int cap = 64; int len = 0;
        char *buf = (char*)xmalloc(cap);
        if (!buf) {
            re0_error_append(l->errors, RE0_ERR_INTERNAL, re0_span_make(start, cur_pos(l)),
                             l->file_path, "out of memory while scanning identifier");
            l->had_error = true;
            return make_token(l, TK_ERROR, start);
        }
        buf[len++] = c;
        while (is_ident_part(peek(l))) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *next = (char*)xrealloc(buf, cap);
                if (!next) {
                    re0_error_append(l->errors, RE0_ERR_INTERNAL,
                                     re0_span_make(start, cur_pos(l)), l->file_path,
                                     "out of memory while growing identifier");
                    l->had_error = true;
                    free(buf);
                    return make_token(l, TK_ERROR, start);
                }
                buf = next;
            }
            buf[len++] = advance(l); l->column++;
        }
        buf[len] = '\0';
        Re0Token t = make_id(l, buf, start);
        free(buf);
        return t;
    }

    if (is_digit(c) || (c == '-' && is_digit(peek(l)))) {
        if (c == '-') return scan_number(l, start, true, 0);
        return scan_number(l, start, false, c);
    }

    switch (c) {
        case '"': return scan_string(l, start);
        case '\'': return scan_char(l, start);
        case '{': return make_token(l, TK_LBRACE, start);
        case '}': return make_token(l, TK_RBRACE, start);
        case '(': return make_token(l, TK_LPAREN, start);
        case ')': return make_token(l, TK_RPAREN, start);
        case '[': return make_token(l, TK_LBRACKET, start);
        case ']': return make_token(l, TK_RBRACKET, start);
        case ',': return make_token(l, TK_COMMA, start);
        case ';': return make_token(l, TK_SEMICOLON, start);
        case '?':
            if (peek(l) == '?') { advance(l); l->column++; return make_token(l, TK_DOUBLEQUESTION, start); }
            return make_token(l, TK_QUESTION, start);
        case '@': return make_token(l, TK_AT, start);
        case '#': { skip_line_comment(l); return scan_token(l); }
    }

    if (c == ':') {
        if (peek(l) == ':') { advance(l); l->column++; return make_token(l, TK_DOUBLECOLON, start); }
        return make_token(l, TK_COLON, start);
    }
    if (c == '.') {
        if (peek(l) == '.' && peek_n(l, 1) == '.') { advance(l); advance(l); l->column += 2; return make_token(l, TK_ELLIPSIS, start); }
        if (peek(l) == '.') { advance(l); l->column++; return make_token(l, TK_DOUBLEDOT, start); }
        return make_token(l, TK_DOT, start);
    }
    if (c == '=') {
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_DOUBLEEQUAL, start); }
        if (peek(l) == '>') { advance(l); l->column++; return make_token(l, TK_FATARROW, start); }
        return make_token(l, TK_EQUAL, start);
    }
    if (c == '!') {
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_BANGEQUAL, start); }
        return make_token(l, TK_BANG, start);
    }
    if (c == '>') {
        if (peek(l) == '>') { advance(l); l->column++; return make_token(l, TK_RIGHTSHIFT, start); }
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_GREATEREQUAL, start); }
        return make_token(l, TK_GREATER, start);
    }
    if (c == '<') {
        if (peek(l) == '<') { advance(l); l->column++; return make_token(l, TK_LEFTSHIFT, start); }
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_LESSEQUAL, start); }
        return make_token(l, TK_LESS, start);
    }
    if (c == '&') {
        if (peek(l) == '&') { advance(l); l->column++; return make_token(l, TK_DOUBLEAMPERSAND, start); }
        return make_token(l, TK_AMPERSAND, start);
    }
    if (c == '|') {
        if (peek(l) == '>') { advance(l); l->column++; return make_token(l, TK_PIPEARROW, start); }
        if (peek(l) == '|') { advance(l); l->column++; return make_token(l, TK_DOUBLEPIPE, start); }
        return make_token(l, TK_PIPE, start);
    }
    if (c == '-') {
        if (peek(l) == '>') { advance(l); l->column++; return make_token(l, TK_ARROW, start); }
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_MINUSEQUAL, start); }
        return make_token(l, TK_MINUS, start);
    }
    if (c == '+') {
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_PLUSEQUAL, start); }
        return make_token(l, TK_PLUS, start);
    }
    if (c == '*') {
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_STAREQUAL, start); }
        return make_token(l, TK_STAR, start);
    }
    if (c == '/') {
        if (peek(l) == '=') { advance(l); l->column++; return make_token(l, TK_SLASHEQUAL, start); }
        return make_token(l, TK_SLASH, start);
    }
    if (c == '%') return make_token(l, TK_PERCENT, start);
    if (c == '^') return make_token(l, TK_CARET, start);
    if (c == '~') return make_token(l, TK_TILDE, start);

    re0_error_append(l->errors, RE0_ERR_SYNTAX, re0_span_make(start, cur_pos(l)),
                     l->file_path, "unexpected character '%c'", c);
    l->had_error = true;
    return make_token(l, TK_ERROR, start);
}

bool re0_lexer_tokenize(Re0Lexer *l, const char *source, const char *file_path) {
    l->source = source;
    l->file_path = file_path;
    l->pos = 0;
    l->line = 0;
    l->column = 0;
    l->bol = 0;
    l->had_error = false;

    /* Skip UTF-8 BOM (EF BB BF) */
    if (source && (unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
        l->pos = 3;
        l->bol = 3;
    }

    /* Free any token vector from a previous file (recursive import reuses
     * this lexer; re-init without freeing would leak the prior vector). */
    re0_stream_free(&l->stream);
    Re0TokenVec_init(&l->stream.tokens);
    l->stream.cursor = 0;

    for (;;) {
        Re0Token t = scan_token(l);
        re0_stream_push(&l->stream, t);
        if (t.kind == TK_EOF || t.kind == TK_ERROR) break;
    }
    return !l->had_error;
}

void re0_lexer_destroy(Re0Lexer *l) {
    re0_stream_free(&l->stream);
    l->source = NULL;
}
