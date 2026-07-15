#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void re0_parser_init(Re0Parser *p, Re0Arena *arena, Re0ErrorList *errors) {
    p->arena = arena; p->errors = errors; p->stream = NULL;
    Re0StmtVec_init(&p->stmts); p->depth = 0; p->had_error = false;
}

static Re0Token *peek(Re0Parser *p) { return re0_stream_peek(p->stream); }
static Re0Token advance(Re0Parser *p) { return re0_stream_advance(p->stream); }
static bool check(Re0Parser *p, Re0TokenKind k) { return re0_stream_check(p->stream, k); }
static Re0Token expect(Re0Parser *p, Re0TokenKind k) {
    if (check(p, k)) return advance(p);
    Re0Token *t = peek(p);
    re0_error_append(p->errors, RE0_ERR_SYNTAX, t ? t->span : RE0_SPAN_ZERO, NULL,
        "expected '%s', got '%s'",
        re0_token_kind_name(k), t ? re0_token_kind_name(t->kind) : "EOF");
    p->had_error = true;
    Re0Token err = { TK_ERROR, RE0_SPAN_ZERO, {0}, NULL };
    return err;
}

static Re0Expr *parse_expr(Re0Parser *p);
static Re0Stmt *parse_stmt(Re0Parser *p);

/* ── helper: advance and return the token's string value (for type names) ── */
static char *parse_type_name(Re0Parser *p) {
    Re0Token t = advance(p);
    return t.str_val ? re0_arena_strdup(p->arena, t.str_val) : re0_arena_strdup(p->arena, "unknown");
}

static Re0Expr *expr_ident(Re0Parser *p) {
    Re0Token t = advance(p); Re0Expr *e = re0_expr_make(EXPR_IDENT, t.span);
    if (t.kind == TK_KW_SELF) e->ident.name = re0_arena_strdup(p->arena, "self");
    else if (t.str_val) e->ident.name = re0_arena_strdup(p->arena, t.str_val);
    else e->ident.name = re0_arena_strdup(p->arena, "");
    return e;
}

static Re0Expr *expr_int(Re0Parser *p) {
    Re0Token t = advance(p);
    if (t.kind == TK_KW_TRUE || t.kind == TK_KW_FALSE) {
        Re0Expr *e = re0_expr_make(EXPR_BOOL, t.span);
        e->bool_lit.val = (t.kind == TK_KW_TRUE); return e;
    }
    Re0Expr *e = re0_expr_make(EXPR_INT, t.span);
    e->int_lit.val = t.int_val; return e;
}

static Re0Expr *expr_float(Re0Parser *p) {
    Re0Token t = advance(p); Re0Expr *e = re0_expr_make(EXPR_FLOAT, t.span);
    e->float_lit.val = t.float_val; return e;
}

static Re0Expr *expr_string(Re0Parser *p) {
    Re0Token t = advance(p); Re0Expr *e = re0_expr_make(EXPR_STRING, t.span);
    if (t.str_val) e->str_lit.val = re0_arena_strdup(p->arena, t.str_val);
    return e;
}

static Re0Expr *expr_char(Re0Parser *p) {
    Re0Token t = advance(p); Re0Expr *e = re0_expr_make(EXPR_CHAR, t.span);
    e->char_lit.val = t.char_val; return e;
}

static Re0Expr *expr_paren(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    if (check(p, TK_RPAREN)) { advance(p); return re0_expr_make(EXPR_UNIT, span); }
    Re0Expr *e = parse_expr(p);
    if (check(p, TK_RPAREN)) advance(p);
    return e;
}

static Re0Expr *expr_array(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    if (check(p, TK_RBRACKET)) { advance(p); Re0Expr *e = re0_expr_make(EXPR_ARRAY, span); return e; }
    Re0Expr *first = parse_expr(p);
    if (check(p, TK_SEMICOLON)) {
        advance(p); Re0Expr *count = parse_expr(p); expect(p, TK_RBRACKET);
        Re0Expr *e = re0_expr_make(EXPR_ARRAY_REPEAT, span);
        e->array_repeat.value = first; e->array_repeat.count = count; return e;
    }
    Re0ExprVec elems; Re0ExprVec_init(&elems); Re0ExprVec_push(&elems, first);
    while (check(p, TK_COMMA)) {
        advance(p); if (check(p, TK_RBRACKET)) break;
        Re0ExprVec_push(&elems, parse_expr(p));
    }
    expect(p, TK_RBRACKET);
    Re0Expr *e = re0_expr_make(EXPR_ARRAY, span);
    e->array.elems = elems.data; e->array.count = (int)Re0ExprVec_len(&elems); return e;
}

static Re0Expr *expr_if(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Expr *cond = parse_expr(p);
    Re0Expr *then_body = NULL;
    if (check(p, TK_LBRACE)) {
        advance(p); Re0ExprVec stmts; Re0ExprVec_init(&stmts);
        while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream))
            Re0ExprVec_push(&stmts, parse_expr(p));
        expect(p, TK_RBRACE); then_body = re0_expr_make(EXPR_BLOCK, span);
        then_body->block.stmts = stmts.data; then_body->block.count = (int)Re0ExprVec_len(&stmts);
    } else { then_body = parse_expr(p); }
    Re0Expr *else_body = NULL;
    if (check(p, TK_KW_ELSE)) {
        advance(p);
        if (check(p, TK_KW_IF)) { else_body = expr_if(p); }
        else if (check(p, TK_LBRACE)) {
            advance(p); Re0ExprVec stmts; Re0ExprVec_init(&stmts);
            while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream))
                Re0ExprVec_push(&stmts, parse_expr(p));
            expect(p, TK_RBRACE); else_body = re0_expr_make(EXPR_BLOCK, span);
            else_body->block.stmts = stmts.data; else_body->block.count = (int)Re0ExprVec_len(&stmts);
        } else { else_body = parse_expr(p); }
    }
    Re0Expr *e = re0_expr_make(EXPR_IF, span);
    e->if_expr.cond = cond; e->if_expr.then = then_body; e->if_expr.else_ = else_body; return e;
}

static Re0Expr *expr_lambda(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Expr *e = re0_expr_make(EXPR_LAMBDA, span);
    e->lambda.param_count = 0; e->lambda.params = NULL;
    if (!check(p, TK_PIPE)) {
        e->lambda.param_count = 1;
        e->lambda.params = (void*)calloc(1, sizeof(e->lambda.params[0]));
        e->lambda.params[0].name = re0_arena_strdup(p->arena, advance(p).str_val);
        e->lambda.params[0].type = NULL;
        if (check(p, TK_COLON)) { advance(p); e->lambda.params[0].type = re0_arena_strdup(p->arena, parse_type_name(p)); }
        while (check(p, TK_COMMA)) {
            advance(p); e->lambda.param_count++;
            e->lambda.params = (void*)realloc(e->lambda.params, sizeof(e->lambda.params[0]) * (size_t)e->lambda.param_count);
            Re0Token pn = advance(p);
            e->lambda.params[e->lambda.param_count - 1].name = re0_arena_strdup(p->arena, pn.str_val);
            e->lambda.params[e->lambda.param_count - 1].type = NULL;
            if (check(p, TK_COLON)) { advance(p); e->lambda.params[e->lambda.param_count - 1].type = re0_arena_strdup(p->arena, parse_type_name(p)); }
        }
    }
    expect(p, TK_PIPE); e->lambda.body = parse_expr(p); return e;
}

/* ── struct init: Name { field: val, ... } ── */
static Re0Expr *parse_struct_init(Re0Parser *p, const char *name, Re0Span span) {
    expect(p, TK_LBRACE);
    Re0StructFieldInit *fields = NULL; int field_count = 0;
    fields = (Re0StructFieldInit*)calloc(32, sizeof(Re0StructFieldInit));
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        Re0Token fname = expect(p, TK_IDENT);
        Re0Expr *val = NULL;
        if (check(p, TK_COLON)) { advance(p); val = parse_expr(p); }
        fields[field_count].field = re0_arena_strdup(p->arena, fname.str_val);
        fields[field_count].value = val;
        field_count++;
        if (check(p, TK_COMMA)) advance(p); else break;
    }
    expect(p, TK_RBRACE);
    Re0Expr *e = re0_expr_make(EXPR_STRUCT_INIT, span);
    e->struct_init.name = (char*)name;
    e->struct_init.fields = fields;
    e->struct_init.field_count = field_count;
    return e;
}

/* ── match expression: match scrutinee { pat => body, ... } ── */
static Re0Expr *parse_match_expr(Re0Parser *p) {
    Re0Span span = peek(p)->span;
    advance(p); /* 'match' */
    Re0Expr *scrutinee = parse_expr(p);
    expect(p, TK_LBRACE);
    Re0MatchArm *arms = NULL; int arm_count = 0;
    arms = (Re0MatchArm*)calloc(32, sizeof(Re0MatchArm));
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        Re0Expr *pat = parse_expr(p);
        expect(p, TK_FATARROW);
        Re0Expr *body = parse_expr(p);
        arms[arm_count].pat = pat;
        arms[arm_count].body = body;
        arm_count++;
        if (check(p, TK_COMMA)) advance(p); else break;
    }
    expect(p, TK_RBRACE);
    Re0Expr *e = re0_expr_make(EXPR_MATCH, span);
    e->match_.scrutinee = scrutinee;
    e->match_.arms = arms;
    e->match_.arm_count = arm_count;
    return e;
}

static Re0Expr *parse_primary(Re0Parser *p) {
    if (check(p, TK_NUMBER)) return expr_int(p);
    if (check(p, TK_FLOAT)) return expr_float(p);
    if (check(p, TK_STRING)) return expr_string(p);
    if (check(p, TK_CHAR)) return expr_char(p);
    if (check(p, TK_KW_TRUE) || check(p, TK_KW_FALSE)) return expr_int(p);
    if (check(p, TK_IDENT) || check(p, TK_KW_SELF)) {
        size_t saved = re0_stream_pos(p->stream);
        Re0Token t = advance(p);
        const char *name = t.kind == TK_KW_SELF ? "self" : (t.str_val ? t.str_val : "");
        /* EnumName::Variant → qualified ident */
        if (check(p, TK_DOUBLECOLON)) {
            advance(p);
            Re0Token v = expect(p, TK_IDENT);
            char buf[256];
            snprintf(buf, sizeof(buf), "%s::%s", name, v.str_val ? v.str_val : "");
            Re0Expr *e = re0_expr_make(EXPR_IDENT, t.span);
            e->ident.name = re0_arena_strdup(p->arena, buf);
            return e;
        }
        /* Struct init only if uppercase name + { */
        char first = name[0];
        if (check(p, TK_LBRACE) && first >= 'A' && first <= 'Z')
            return parse_struct_init(p, name, t.span);
        re0_stream_set_pos(p->stream, saved);
        return expr_ident(p);
    }
    if (check(p, TK_LPAREN)) return expr_paren(p);
    if (check(p, TK_LBRACKET)) return expr_array(p);
    if (check(p, TK_KW_IF)) return expr_if(p);
    if (check(p, TK_KW_MATCH)) return parse_match_expr(p);
    if (check(p, TK_PIPE)) return expr_lambda(p);
    /* spawn f(args) → __reo_spawn(f, args...) */
    if (check(p, TK_KW_SPAWN)) {
        Re0Span span = advance(p).span;
        Re0Expr *callee = parse_primary(p);
        Re0ExprVec args; Re0ExprVec_init(&args);
        Re0ExprVec_push(&args, callee);
        if (check(p, TK_LPAREN)) {
            advance(p);
            if (!check(p, TK_RPAREN)) { Re0ExprVec_push(&args, parse_expr(p)); while (check(p, TK_COMMA)) { advance(p); Re0ExprVec_push(&args, parse_expr(p)); } }
            expect(p, TK_RPAREN);
        }
        Re0Expr *call = re0_expr_make(EXPR_CALL, span);
        call->call.callee = re0_expr_make(EXPR_IDENT, span);
        call->call.callee->ident.name = re0_arena_strdup(p->arena, "__reo_spawn");
        call->call.args = args.data; call->call.arg_count = (int)Re0ExprVec_len(&args);
        return call;
    }
    /* await expr → __reo_await(expr) */
    if (check(p, TK_KW_AWAIT)) {
        Re0Span span = advance(p).span;
        Re0Expr *inner = parse_primary(p);
        Re0Expr *call = re0_expr_make(EXPR_CALL, span);
        call->call.callee = re0_expr_make(EXPR_IDENT, span);
        call->call.callee->ident.name = re0_arena_strdup(p->arena, "__reo_await");
        Re0Expr **args = (Re0Expr**)calloc(1, sizeof(Re0Expr*));
        args[0] = inner;
        call->call.args = args; call->call.arg_count = 1;
        return call;
    }
    if (check(p, TK_MINUS) || check(p, TK_BANG) || check(p, TK_AMPERSAND) || check(p, TK_STAR)) {
        Re0Token op_token = advance(p); Re0Expr *e = re0_expr_make(EXPR_UNARY, op_token.span);
        bool ref_mut = false;
        if (op_token.kind == TK_AMPERSAND && check(p, TK_KW_MUT)) {
            advance(p);
            ref_mut = true;
        }
        e->unary.operand = parse_primary(p);
        switch (op_token.kind) { case TK_MINUS: e->unary.op = UNOP_NEG; break; case TK_BANG: e->unary.op = UNOP_NOT; break; case TK_AMPERSAND: e->unary.op = ref_mut ? UNOP_REFMUT : UNOP_REF; break; case TK_STAR: e->unary.op = UNOP_DEREF; break; default: break; }
        return e;
    }
    Re0Token *t = peek(p);
    re0_error_append(p->errors, RE0_ERR_SYNTAX, t ? t->span : RE0_SPAN_ZERO, NULL,
        "unexpected token '%s'", t ? re0_token_kind_name(t->kind) : "EOF");
    p->had_error = true;
    return re0_expr_make(EXPR_UNIT, t ? t->span : RE0_SPAN_ZERO);
}

static Re0BinOpKind token_to_binop(Re0TokenKind k) {
    switch (k) { case TK_PLUS: return BINOP_ADD; case TK_MINUS: return BINOP_SUB; case TK_STAR: return BINOP_MUL; case TK_SLASH: return BINOP_DIV; case TK_PERCENT: return BINOP_MOD; case TK_DOUBLEEQUAL: return BINOP_EQ; case TK_BANGEQUAL: return BINOP_NE; case TK_LESS: return BINOP_LT; case TK_LESSEQUAL: return BINOP_LE; case TK_GREATER: return BINOP_GT; case TK_GREATEREQUAL: return BINOP_GE; case TK_DOUBLEAMPERSAND: case TK_AMPERSAND: return BINOP_AND; case TK_DOUBLEPIPE: case TK_PIPE: return BINOP_OR;     case TK_DOUBLEDOT: return BINOP_RANGE; case TK_LEFTSHIFT: return BINOP_SHL; case TK_RIGHTSHIFT: return BINOP_SHR; default: return BINOP_ADD; }
}

static Re0Expr *parse_prec(Re0Parser *p, int min_prec) {
    p->depth++;
    Re0Expr *left = parse_primary(p);
    for (;;) {
        if (check(p, TK_LPAREN)) { advance(p); Re0ExprVec args; Re0ExprVec_init(&args);
            if (!check(p, TK_RPAREN)) { Re0ExprVec_push(&args, parse_expr(p)); while (check(p, TK_COMMA)) { advance(p); Re0ExprVec_push(&args, parse_expr(p)); } }
            expect(p, TK_RPAREN); Re0Expr *call = re0_expr_make(EXPR_CALL, left->span);
            call->call.callee = left; call->call.args = args.data; call->call.arg_count = (int)Re0ExprVec_len(&args); left = call; continue;
        }
        if (check(p, TK_DOT)) { advance(p); Re0Token t = expect(p, TK_IDENT);
            Re0Expr *sel = re0_expr_make(EXPR_SELECT, left->span); sel->select.object = left; sel->select.field = re0_arena_strdup(p->arena, t.str_val); left = sel; continue;
        }
        if (check(p, TK_LBRACKET)) { advance(p); Re0Expr *idx = parse_expr(p); expect(p, TK_RBRACKET);
            Re0Expr *e = re0_expr_make(EXPR_INDEX, left->span); e->index.target = left; e->index.index = idx; left = e; continue;
        }
        if (check(p, TK_QUESTION)) {
            advance(p);
            Re0Expr *t = re0_expr_make(EXPR_TRY, left->span);
            t->try_.inner = left; left = t; continue;
        }
        /* pipeline: a |> f(b) → f(a, b); a |> f → f(a) */
        if (min_prec == 0 && check(p, TK_PIPEARROW)) {
            advance(p);
            Re0Expr *right = parse_prec(p, 1);
            if (right && right->kind == EXPR_CALL) {
                /* 在参数列表首位插入 left */
                int n = right->call.arg_count;
                Re0Expr **new_args = (Re0Expr**)calloc((size_t)n + 1, sizeof(Re0Expr*));
                new_args[0] = left;
                for (int i = 0; i < n; i++) new_args[i + 1] = right->call.args[i];
                right->call.args = new_args;
                right->call.arg_count = n + 1;
                left = right;
            } else if (right) {
                Re0Expr *call = re0_expr_make(EXPR_CALL, left->span);
                call->call.callee = right;
                Re0Expr **args = (Re0Expr**)calloc(1, sizeof(Re0Expr*));
                args[0] = left;
                call->call.args = args;
                call->call.arg_count = 1;
                left = call;
            }
            continue;
        }
        Re0Token *t = peek(p); if (!t || !re0_token_is_binop(t->kind)) break;
        if (t->kind == TK_DOT || t->kind == TK_LBRACKET || t->kind == TK_LPAREN) break;
        int prec = re0_token_binop_precedence(t->kind); if (prec < min_prec) break;
        advance(p); Re0Expr *right = parse_prec(p, prec);
        Re0Expr *bin = re0_expr_make(EXPR_BINARY, left->span);
        bin->binary.op = token_to_binop(t->kind); bin->binary.left = left; bin->binary.right = right; left = bin;
    }
    p->depth--; return left;
}

static Re0Expr *parse_expr(Re0Parser *p) { return parse_prec(p, 0); }

static Re0Stmt *parse_let(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token name = expect(p, TK_IDENT); char *type = NULL;
    if (check(p, TK_COLON)) { advance(p); type = re0_arena_strdup(p->arena, parse_type_name(p)); }
    Re0Expr *init = NULL;
    if (check(p, TK_EQUAL)) { advance(p); init = parse_expr(p); }
    if (check(p, TK_SEMICOLON)) advance(p);
    Re0Stmt *s = re0_stmt_make(STMT_LET, span);
    s->let_stmt.name = re0_arena_strdup(p->arena, name.str_val); s->let_stmt.type = type; s->let_stmt.init = init;
    return s;
}

static Re0Stmt *parse_assign(Re0Parser *p) {
    Re0Token name = advance(p);
    if (check(p, TK_DOT)) {
        advance(p); Re0Token field = expect(p, TK_IDENT);
        Re0BinOpKind op = BINOP_ADD; (void)op;
        if (check(p, TK_EQUAL)) advance(p);
        Re0Expr *val = parse_expr(p); if (check(p, TK_SEMICOLON)) advance(p);
        Re0Stmt *s = re0_stmt_make(STMT_FIELD_ASSIGN, name.span);
        Re0Expr *obj = re0_expr_make(EXPR_IDENT, name.span);
        obj->ident.name = re0_arena_strdup(p->arena, name.str_val);
        s->field_assign.obj = obj; s->field_assign.field = re0_arena_strdup(p->arena, field.str_val);
        s->field_assign.value = val; return s;
    }
    Re0BinOpKind op = BINOP_ADD;
    if (check(p, TK_EQUAL)) advance(p);
    else if (check(p, TK_PLUSEQUAL)) { advance(p); op = BINOP_ADD; }
    else if (check(p, TK_MINUSEQUAL)) { advance(p); op = BINOP_SUB; }
    else if (check(p, TK_STAREQUAL)) { advance(p); op = BINOP_MUL; }
    else if (check(p, TK_SLASHEQUAL)) { advance(p); op = BINOP_DIV; }
    Re0Expr *val = parse_expr(p); if (check(p, TK_SEMICOLON)) advance(p);
    Re0Stmt *s = re0_stmt_make(STMT_ASSIGN, name.span);
    s->assign.name = re0_arena_strdup(p->arena, name.str_val); s->assign.value = val; s->assign.op = op;
    return s;
}

static Re0Stmt *parse_return(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Expr *val = NULL; if (!check(p, TK_SEMICOLON) && !check(p, TK_RBRACE)) val = parse_expr(p);
    if (check(p, TK_SEMICOLON)) advance(p); Re0Stmt *s = re0_stmt_make(STMT_RETURN, span);
    s->return_stmt.value = val; return s;
}

static Re0Stmt *parse_if_stmt(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Expr *cond = parse_expr(p); expect(p, TK_LBRACE);
    Re0StmtVec body; Re0StmtVec_init(&body);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) Re0StmtVec_push(&body, parse_stmt(p));
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_IF, span);
    s->if_stmt.branches = (void*)calloc(1, sizeof(s->if_stmt.branches[0]));
    s->if_stmt.branches[0].cond = cond; s->if_stmt.branches[0].body = body.data; s->if_stmt.branches[0].body_count = (int)Re0StmtVec_len(&body);
    s->if_stmt.branch_count = 1; s->if_stmt.else_body = NULL; s->if_stmt.else_count = 0;
    if (check(p, TK_KW_ELSE)) {
        advance(p);
        if (check(p, TK_LBRACE)) {
            expect(p, TK_LBRACE); Re0StmtVec eb; Re0StmtVec_init(&eb);
            while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) Re0StmtVec_push(&eb, parse_stmt(p));
            expect(p, TK_RBRACE);
            s->if_stmt.else_body = eb.data; s->if_stmt.else_count = (int)Re0StmtVec_len(&eb);
        }
    }
    return s;
}

static Re0Stmt *parse_while(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Expr *cond = parse_expr(p); expect(p, TK_LBRACE);
    Re0StmtVec body; Re0StmtVec_init(&body);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) Re0StmtVec_push(&body, parse_stmt(p));
    expect(p, TK_RBRACE); Re0Stmt *s = re0_stmt_make(STMT_WHILE, span);
    s->while_stmt.cond = cond; s->while_stmt.body = body.data; s->while_stmt.body_count = (int)Re0StmtVec_len(&body);
    return s;
}

static Re0Stmt *parse_for(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token var = expect(p, TK_IDENT);
    if (peek(p) && peek(p)->kind == TK_IDENT && strcmp(peek(p)->str_val ? peek(p)->str_val : "", "in") == 0) advance(p);
    Re0Expr *iter = parse_expr(p); expect(p, TK_LBRACE);
    Re0StmtVec body; Re0StmtVec_init(&body);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) Re0StmtVec_push(&body, parse_stmt(p));
    expect(p, TK_RBRACE); Re0Stmt *s = re0_stmt_make(STMT_FOR, span);
    s->for_stmt.var = re0_arena_strdup(p->arena, var.str_val); s->for_stmt.iter = iter;
    s->for_stmt.body = body.data; s->for_stmt.body_count = (int)Re0StmtVec_len(&body); return s;
}

static Re0Stmt *parse_fn(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token name = expect(p, TK_IDENT);
    char **type_params = NULL; int tp_count = 0;
    if (check(p, TK_LESS)) { advance(p);
        type_params = (void*)calloc(8, sizeof(char*));
        if (peek(p) && peek(p)->kind == TK_IDENT) { type_params[tp_count++] = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
        expect(p, TK_GREATER);
    }
    expect(p, TK_LPAREN);
    Re0FnParam *params = NULL; int param_count = 0;
    params = (Re0FnParam*)calloc(32, sizeof(Re0FnParam));
    if (!check(p, TK_RPAREN)) {
        Re0Token pname = advance(p); /* accept IDENT or TK_KW_SELF */
        params[param_count].name = re0_arena_strdup(p->arena, pname.kind == TK_KW_SELF ? "self" : (pname.str_val ? pname.str_val : ""));
        params[param_count].ptype = NULL;
        if (check(p, TK_COLON)) { advance(p); params[param_count].ptype = parse_type_name(p); }
        param_count++;
        while (check(p, TK_COMMA)) { advance(p);
            Re0Token pn = advance(p);
            params[param_count].name = re0_arena_strdup(p->arena, pn.kind == TK_KW_SELF ? "self" : (pn.str_val ? pn.str_val : ""));
            params[param_count].ptype = NULL;
            if (check(p, TK_COLON)) { advance(p); params[param_count].ptype = parse_type_name(p); }
            param_count++;
        }
    }
    expect(p, TK_RPAREN);
    char *ret_type = NULL;
    if (check(p, TK_ARROW)) { advance(p); ret_type = re0_arena_strdup(p->arena, parse_type_name(p)); }
    expect(p, TK_LBRACE); Re0StmtVec body; Re0StmtVec_init(&body);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) Re0StmtVec_push(&body, parse_stmt(p));
    expect(p, TK_RBRACE);
    /* 隐式 return：如果函数体最后一条是 expr 语句且函数有返回类型，自动包为 return */
    size_t blen = Re0StmtVec_len(&body);
    if (blen > 0 && ret_type) {
        Re0Stmt *last = body.data[blen - 1];
        if (last && last->kind == STMT_EXPR) {
            Re0Stmt *ret = re0_stmt_make(STMT_RETURN, last->span);
            ret->return_stmt.value = last->expr_stmt.expr;
            body.data[blen - 1] = ret;
        }
    }
    Re0Stmt *s = re0_stmt_make(STMT_FUNCTION, span);
    s->function.name = re0_arena_strdup(p->arena, name.str_val);
    s->function.params = params; s->function.param_count = param_count;
    s->function.ret_type = ret_type; s->function.body = body.data; s->function.body_count = (int)Re0StmtVec_len(&body);
    s->function.type_params = type_params; s->function.type_param_count = tp_count; s->function.is_async = false;
    return s;
}

static Re0Stmt *parse_extern(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p); expect(p, TK_LBRACE);
    Re0ExternFnDecl *funcs = NULL; int func_count = 0;
    funcs = (Re0ExternFnDecl*)calloc(32, sizeof(Re0ExternFnDecl));
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        expect(p, TK_KW_FN); Re0Token nm = expect(p, TK_IDENT);
        funcs[func_count].name = re0_arena_strdup(p->arena, nm.str_val);
        expect(p, TK_LPAREN);
        funcs[func_count].params = (Re0FnCallParam*)calloc(16, sizeof(Re0FnCallParam));
        int pc = 0;
        if (check(p, TK_ELLIPSIS)) {
            advance(p);
            funcs[func_count].variadic = true;
        } else if (!check(p, TK_RPAREN)) {
            Re0Token pn = expect(p, TK_IDENT); expect(p, TK_COLON);
            funcs[func_count].params[pc].pname = re0_arena_strdup(p->arena, pn.str_val);
            funcs[func_count].params[pc].ptype = re0_arena_strdup(p->arena, parse_type_name(p)); pc++;
            while (check(p, TK_COMMA)) { advance(p);
                if (check(p, TK_ELLIPSIS)) { advance(p); funcs[func_count].variadic = true; break; }
                pn = expect(p, TK_IDENT); expect(p, TK_COLON);
                funcs[func_count].params[pc].pname = re0_arena_strdup(p->arena, pn.str_val);
                funcs[func_count].params[pc].ptype = re0_arena_strdup(p->arena, parse_type_name(p)); pc++;
            }
        }
        expect(p, TK_RPAREN); funcs[func_count].param_count = pc;
        funcs[func_count].ret_type = NULL;
        if (check(p, TK_ARROW)) { advance(p); funcs[func_count].ret_type = re0_arena_strdup(p->arena, parse_type_name(p)); }
        expect(p, TK_SEMICOLON); func_count++;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_EXTERN, span);
    s->extern_.funcs = funcs; s->extern_.func_count = func_count; return s;
}

static Re0Stmt *parse_import(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    char *mod_name = NULL;
    /* 支持: import "path/file" 或 import module_name */
    if (check(p, TK_STRING)) {
        mod_name = re0_arena_strdup(p->arena, peek(p)->str_val);
        advance(p);
    } else {
        Re0Token mod = expect(p, TK_IDENT);
        mod_name = re0_arena_strdup(p->arena, mod.str_val);
    }
    char **items = NULL; int ic = 0;
    if (check(p, TK_LBRACE)) { advance(p); items = (void*)calloc(64, sizeof(char*));
        if (peek(p) && peek(p)->kind == TK_IDENT) { items[ic++] = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
        while (check(p, TK_COMMA)) { advance(p); items[ic++] = re0_arena_strdup(p->arena, expect(p, TK_IDENT).str_val); }
        expect(p, TK_RBRACE);
    }
    if (check(p, TK_SEMICOLON)) advance(p);
    Re0Stmt *s = re0_stmt_make(STMT_IMPORT, span);
    s->import.module = mod_name; s->import.items = items; s->import.item_count = ic;
    return s;
}

/* ── struct declaration: struct Name { field: Type, ... } ── */
static Re0Stmt *parse_struct(Re0Parser *p) {
    Re0Span span = peek(p)->span;
    advance(p);
    Re0Token nm = expect(p, TK_IDENT);
    char **type_params = NULL; int tp_count = 0;
    if (check(p, TK_LESS)) {
        advance(p); type_params = (void*)calloc(8, sizeof(char*));
        if (peek(p) && peek(p)->kind == TK_IDENT) { type_params[tp_count++] = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
        expect(p, TK_GREATER);
    }
    expect(p, TK_LBRACE);
    Re0StructFieldDecl *fields = NULL; int field_count = 0;
    fields = (Re0StructFieldDecl*)calloc(32, sizeof(Re0StructFieldDecl));
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        Re0Token fname = expect(p, TK_IDENT);
        if (check(p, TK_COLON)) advance(p);
        char *ftype = re0_arena_strdup(p->arena, parse_type_name(p));
        fields[field_count].name = re0_arena_strdup(p->arena, fname.str_val);
        fields[field_count].type = ftype;
        field_count++;
        if (check(p, TK_COMMA)) advance(p); else break;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_STRUCT, span);
    s->struct_decl.name = re0_arena_strdup(p->arena, nm.str_val);
    s->struct_decl.fields = fields;
    s->struct_decl.field_count = field_count;
    s->struct_decl.type_params = type_params;
    s->struct_decl.type_param_count = tp_count;
    return s;
}

/* ── enum declaration: enum Name { Variant, Variant2(Type), ... } ── */
static Re0Stmt *parse_enum(Re0Parser *p) {
    Re0Span span = peek(p)->span;
    advance(p); /* 'enum' */
    Re0Token nm = expect(p, TK_IDENT);
    expect(p, TK_LBRACE);
    Re0EnumVariantDecl *variants = NULL; int variant_count = 0;
    variants = (Re0EnumVariantDecl*)calloc(32, sizeof(Re0EnumVariantDecl));
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        Re0Token vname = expect(p, TK_IDENT);
        variants[variant_count].vname = re0_arena_strdup(p->arena, vname.str_val);
        variants[variant_count].types = NULL;
        variants[variant_count].type_count = 0;
        if (check(p, TK_LPAREN)) {
            advance(p);
            variants[variant_count].types = (Re0Expr**)calloc(8, sizeof(Re0Expr*));
            int tc = 0;
            if (!check(p, TK_RPAREN)) {
                variants[variant_count].types[tc++] = parse_expr(p);
                while (check(p, TK_COMMA)) { advance(p); variants[variant_count].types[tc++] = parse_expr(p); }
            }
            variants[variant_count].type_count = tc;
            expect(p, TK_RPAREN);
        }
        variant_count++;
        if (check(p, TK_COMMA)) advance(p); else break;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_ENUM, span);
    s->enum_decl.name = re0_arena_strdup(p->arena, nm.str_val);
    s->enum_decl.variants = variants;
    s->enum_decl.variant_count = variant_count;
    return s;
}

/* ── const NAME: Type = expr; ── */
static Re0Stmt *parse_const(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token nm = expect(p, TK_IDENT); expect(p, TK_COLON);
    char *type = parse_type_name(p); expect(p, TK_EQUAL);
    Re0Expr *val = parse_expr(p); expect(p, TK_SEMICOLON);
    Re0Stmt *s = re0_stmt_make(STMT_CONST, span);
    s->const_decl.name = re0_arena_strdup(p->arena, nm.str_val);
    s->const_decl.type = type; s->const_decl.value = val;
    return s;
}

/* ── type Alias = TargetType; ── */
static Re0Stmt *parse_type_alias(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token nm = expect(p, TK_IDENT); expect(p, TK_EQUAL);
    char *target = parse_type_name(p);
    if (check(p, TK_SEMICOLON)) advance(p);  /* 分号可选（与 Rust reo 一致） */
    Re0Stmt *s = re0_stmt_make(STMT_TYPE_ALIAS, span);
    s->type_alias.name = re0_arena_strdup(p->arena, nm.str_val);
    s->type_alias.target = target;
    return s;
}

/* ── pub stmt ── */
static Re0Stmt *parse_pub(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Stmt *inner = parse_stmt(p);
    Re0Stmt *s = re0_stmt_make(STMT_PUB, span);
    s->pub.inner = inner;
    return s;
}

/* ── trait Name { fn method(params) -> Ret; ... } ── */
static Re0Stmt *parse_trait(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token nm = expect(p, TK_IDENT); expect(p, TK_LBRACE);
    Re0TraitMethodDecl *methods = (Re0TraitMethodDecl*)calloc(32, sizeof(Re0TraitMethodDecl));
    int mc = 0;
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        if (!check(p, TK_KW_FN)) break;
        advance(p);
        Re0Token mname = expect(p, TK_IDENT);
        methods[mc].mname = re0_arena_strdup(p->arena, mname.str_val);
        expect(p, TK_LPAREN);
        methods[mc].params = (Re0FnCallParam*)calloc(16, sizeof(Re0FnCallParam));
        int pc = 0;
        if (!check(p, TK_RPAREN)) {
            Re0Token pn = advance(p); /* accept IDENT or self */
            if (check(p, TK_COLON)) { advance(p); methods[mc].params[pc].ptype = parse_type_name(p); }
            methods[mc].params[pc].pname = re0_arena_strdup(p->arena, pn.kind == TK_KW_SELF ? "self" : (pn.str_val ? pn.str_val : "")); pc++;
            while (check(p, TK_COMMA)) { advance(p); pn = advance(p);
                if (check(p, TK_COLON)) { advance(p); methods[mc].params[pc].ptype = parse_type_name(p); }
                methods[mc].params[pc].pname = re0_arena_strdup(p->arena, pn.kind == TK_KW_SELF ? "self" : (pn.str_val ? pn.str_val : "")); pc++; }
        }
        expect(p, TK_RPAREN); methods[mc].param_count = pc;
        methods[mc].ret_type = NULL;
        if (check(p, TK_ARROW)) { advance(p); methods[mc].ret_type = parse_type_name(p); }
        expect(p, TK_SEMICOLON); mc++;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_TRAIT, span);
    s->trait_decl.name = re0_arena_strdup(p->arena, nm.str_val);
    s->trait_decl.methods = methods; s->trait_decl.method_count = mc;
    return s;
}

/* ── impl Name { fn ... } or impl Trait for Type { fn ... } ── */
static Re0Stmt *parse_impl(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    char **type_params = NULL; int tp_count = 0;
    if (check(p, TK_LESS)) { advance(p); type_params = (void*)calloc(8, sizeof(char*));
        if (peek(p) && peek(p)->kind == TK_IDENT) { type_params[tp_count++] = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
        expect(p, TK_GREATER); }
    Re0Token nm = expect(p, TK_IDENT);
    char *trait_name = NULL;
    /* impl Trait for Type */
    if (check(p, TK_KW_FOR)) {
        advance(p); trait_name = re0_arena_strdup(p->arena, nm.str_val);
        nm = expect(p, TK_IDENT);
    }
    expect(p, TK_LBRACE);
    Re0StmtVec methods; Re0StmtVec_init(&methods);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        if (check(p, TK_KW_FN)) Re0StmtVec_push(&methods, parse_fn(p));
        else break;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_IMPL, span);
    s->impl.name = re0_arena_strdup(p->arena, nm.str_val);
    s->impl.trait_name = trait_name;
    s->impl.methods = methods.data; s->impl.method_count = (int)Re0StmtVec_len(&methods);
    s->impl.type_params = type_params; s->impl.type_param_count = tp_count;
    return s;
}

/* ── module Name { stmts } ── */
static Re0Stmt *parse_module(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token nm = expect(p, TK_IDENT); expect(p, TK_LBRACE);
    Re0StmtVec body; Re0StmtVec_init(&body);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream))
        Re0StmtVec_push(&body, parse_stmt(p));
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_MODULE, span);
    s->module.name = re0_arena_strdup(p->arena, nm.str_val);
    s->module.body = body.data; s->module.body_count = (int)Re0StmtVec_len(&body);
    return s;
}

/* ── component Name { state x: T, ... fn method() {} ... } ── */
static Re0Stmt *parse_component(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p);
    Re0Token nm = expect(p, TK_IDENT); expect(p, TK_LBRACE);
    Re0StructFieldDecl *state = (Re0StructFieldDecl*)calloc(32, sizeof(Re0StructFieldDecl));
    int sc = 0;
    Re0StmtVec methods; Re0StmtVec_init(&methods);
    while (!check(p, TK_RBRACE) && !re0_stream_eof(p->stream)) {
        /* "state" is a contextual keyword parsed as IDENT */
        if (peek(p) && peek(p)->kind == TK_IDENT &&
            strcmp(peek(p)->str_val ? peek(p)->str_val : "", "state") == 0) {
            advance(p);
            Re0Token fname = expect(p, TK_IDENT); expect(p, TK_COLON);
            char *ftype = parse_type_name(p);
            /* 可选默认值: state x: T = value */
            if (check(p, TK_EQUAL)) { advance(p); parse_expr(p); }
            state[sc].name = re0_arena_strdup(p->arena, fname.str_val);
            state[sc].type = ftype; sc++;
            if (check(p, TK_COMMA)) advance(p);
        } else if (check(p, TK_KW_FN)) {
            Re0StmtVec_push(&methods, parse_fn(p));
        } else break;
    }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_COMPONENT, span);
    s->component.name = re0_arena_strdup(p->arena, nm.str_val);
    s->component.state = state; s->component.state_count = sc;
    s->component.methods = methods.data; s->component.method_count = (int)Re0StmtVec_len(&methods);
    return s;
}

/* ── from module { item1, item2 } ── */
static Re0Stmt *parse_from_import(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p); /* 'from' */
    Re0Token mod = expect(p, TK_IDENT); expect(p, TK_LBRACE);
    char **items = (void*)calloc(64, sizeof(char*)); int ic = 0;
    if (peek(p) && peek(p)->kind == TK_IDENT) { items[ic++] = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
    while (check(p, TK_COMMA)) { advance(p); items[ic++] = re0_arena_strdup(p->arena, expect(p, TK_IDENT).str_val); }
    expect(p, TK_RBRACE);
    Re0Stmt *s = re0_stmt_make(STMT_IMPORT, span);
    s->import.module = re0_arena_strdup(p->arena, mod.str_val);
    s->import.items = items; s->import.item_count = ic;
    return s;
}

/* ── @attr stmt or @attr(arg) stmt ── */
static Re0Stmt *parse_attribute(Re0Parser *p) {
    Re0Span span = peek(p)->span; advance(p); /* '@' */
    Re0Token nm = expect(p, TK_IDENT);
    char *arg = NULL;
    if (check(p, TK_LPAREN)) { advance(p);
        if (peek(p) && peek(p)->kind == TK_IDENT) { arg = re0_arena_strdup(p->arena, peek(p)->str_val); advance(p); }
        expect(p, TK_RPAREN);
    }
    Re0Stmt *inner = parse_stmt(p);
    Re0Stmt *s = re0_stmt_make(STMT_ATTRIBUTE, span);
    s->attribute.attr_name = re0_arena_strdup(p->arena, nm.str_val);
    s->attribute.attr_arg = arg; s->attribute.inner = inner;
    return s;
}

static Re0Stmt *parse_stmt(Re0Parser *p) {
    if (p->depth > RE0_MAX_PARSE_DEPTH) { p->had_error = true; return re0_stmt_make(STMT_EXPR, RE0_SPAN_ZERO); }
    if (check(p, TK_KW_LET)) return parse_let(p);
    if (check(p, TK_KW_CONST)) return parse_const(p);
    if (check(p, TK_KW_TYPE)) return parse_type_alias(p);
    if (check(p, TK_KW_PUB)) return parse_pub(p);
    if (check(p, TK_KW_FN)) return parse_fn(p);
    if (check(p, TK_KW_ASYNC)) { advance(p); return parse_fn(p); } /* async fn */
    if (check(p, TK_KW_IF)) return parse_if_stmt(p);
    if (check(p, TK_KW_WHILE)) return parse_while(p);
    if (check(p, TK_KW_FOR)) return parse_for(p);
    if (check(p, TK_KW_RETURN)) return parse_return(p);
    if (check(p, TK_KW_BREAK)) {
        Re0Span span = advance(p).span;
        Re0Expr *val = NULL;
        if (!check(p, TK_SEMICOLON)) val = parse_expr(p);
        expect(p, TK_SEMICOLON);
        Re0Stmt *s = re0_stmt_make(STMT_BREAK, span);
        s->break_stmt.value = val;
        return s;
    }
    if (check(p, TK_KW_CONTINUE)) { advance(p); expect(p, TK_SEMICOLON); return re0_stmt_make(STMT_CONTINUE, RE0_SPAN_ZERO); }
    if (check(p, TK_KW_IMPORT)) return parse_import(p);
    if (check(p, TK_KW_FROM)) return parse_from_import(p);
    if (check(p, TK_KW_EXTERN)) return parse_extern(p);
    if (check(p, TK_KW_STRUCT)) return parse_struct(p);
    if (check(p, TK_KW_ENUM)) return parse_enum(p);
    if (check(p, TK_KW_TRAIT)) return parse_trait(p);
    if (check(p, TK_KW_IMPL)) return parse_impl(p);
    if (check(p, TK_KW_MODULE)) return parse_module(p);
    if (check(p, TK_KW_COMPONENT)) return parse_component(p);
    if (check(p, TK_AT)) return parse_attribute(p);
    if (check(p, TK_IDENT) && p->stream->cursor + 1 < Re0TokenVec_len(&p->stream->tokens)) {
        Re0Token *peek2 = &p->stream->tokens.data[p->stream->cursor + 1];
        /* 普通赋值: var = ... */
        if (peek2->kind == TK_EQUAL || peek2->kind == TK_PLUSEQUAL ||
            peek2->kind == TK_MINUSEQUAL || peek2->kind == TK_STAREQUAL ||
            peek2->kind == TK_SLASHEQUAL)
            return parse_assign(p);
        /* 字段赋值: obj.field = ... （需要 3 token lookahead 确认 = 在 . 之后） */
        if (peek2->kind == TK_DOT && p->stream->cursor + 3 < Re0TokenVec_len(&p->stream->tokens)) {
            Re0Token *peek4 = &p->stream->tokens.data[p->stream->cursor + 3];
            if (peek4->kind == TK_EQUAL || peek4->kind == TK_PLUSEQUAL ||
                peek4->kind == TK_MINUSEQUAL || peek4->kind == TK_STAREQUAL ||
                peek4->kind == TK_SLASHEQUAL)
                return parse_assign(p);
        }
    }
    Re0Expr *e = parse_expr(p); if (check(p, TK_SEMICOLON)) advance(p);
    Re0Stmt *s = re0_stmt_make(STMT_EXPR, e->span); s->expr_stmt.expr = e; return s;
}

bool re0_parser_parse(Re0Parser *p, Re0TokenStream *stream) {
    p->stream = stream; p->depth = 0;
    while (!re0_stream_eof(p->stream)) {
        Re0Token *t = peek(p); if (!t || t->kind == TK_EOF) break;
        size_t before = re0_stream_pos(p->stream);
        Re0Stmt *s = parse_stmt(p); if (s) Re0StmtVec_push(&p->stmts, s);
        /* error recovery: ensure progress to prevent infinite loops */
        if (re0_stream_pos(p->stream) == before) {
            advance(p); /* force-consume the stuck token */
        }
    }
    return !p->had_error;
}

void re0_parser_destroy(Re0Parser *p) { Re0StmtVec_free(&p->stmts); p->stream = NULL; }
