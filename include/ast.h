#ifndef RE0_AST_H
#define RE0_AST_H
#include "span.h"
#include "types.h"
#include "vec.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EXPR_IDENT, EXPR_INT, EXPR_FLOAT, EXPR_STRING, EXPR_CHAR, EXPR_BOOL,
    EXPR_UNIT, EXPR_TUPLE, EXPR_BINARY, EXPR_UNARY,
    EXPR_CALL, EXPR_INDEX, EXPR_SELECT,
    EXPR_ARRAY, EXPR_ARRAY_REPEAT, EXPR_STRUCT_INIT,
    EXPR_LAMBDA, EXPR_IF, EXPR_BLOCK, EXPR_MATCH, EXPR_TRY,
} Re0ExprKind;

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ, BINOP_NE, BINOP_LT, BINOP_LE, BINOP_GT, BINOP_GE,
    BINOP_AND, BINOP_OR, BINOP_RANGE, BINOP_SHL, BINOP_SHR,
} Re0BinOpKind;

typedef enum {
    UNOP_NEG, UNOP_NOT, UNOP_REF, UNOP_REFMUT, UNOP_DEREF,
} Re0UnOpKind;

typedef enum {
    STMT_EXPR, STMT_LET, STMT_ASSIGN, STMT_FIELD_ASSIGN,
    STMT_IF, STMT_WHILE, STMT_FOR, STMT_RETURN, STMT_BREAK, STMT_CONTINUE,
    STMT_FUNCTION, STMT_STRUCT, STMT_ENUM, STMT_TRAIT, STMT_IMPL,
    STMT_IMPORT, STMT_MODULE, STMT_EXTERN, STMT_CONST,
    STMT_TYPE_ALIAS, STMT_COMPONENT, STMT_PUB, STMT_ATTRIBUTE,
} Re0StmtKind;

typedef struct Re0Expr Re0Expr;
typedef struct Re0Stmt Re0Stmt;

typedef struct { Re0Expr *callee; Re0Expr **args; int arg_count; } Re0ExprCall;
typedef struct { Re0BinOpKind op; Re0Expr *left; Re0Expr *right; } Re0ExprBinary;
typedef struct { Re0UnOpKind op; Re0Expr *operand; } Re0ExprUnary;
typedef struct { Re0Expr *target; Re0Expr *index; } Re0ExprIndex;
typedef struct { Re0Expr *object; char *field; } Re0ExprSelect;
typedef struct { char *name; Re0Expr **elems; int count; } Re0ExprTuple;
typedef struct { Re0Expr **elems; int count; } Re0ExprArray;
typedef struct { Re0Expr *value; Re0Expr *count; } Re0ExprArrayRepeat;
typedef struct { Re0Expr *cond; Re0Expr *then; Re0Expr *else_; } Re0ExprIf;
typedef struct { Re0Expr **stmts; int count; } Re0ExprBlock;
typedef struct { Re0Expr *pat; Re0Expr *body; } Re0MatchArm;
typedef struct { Re0Expr *scrutinee; Re0MatchArm *arms; int arm_count; } Re0ExprMatch;
typedef struct { char *name; char *type; } Re0LambdaParam;
typedef struct { char *field; Re0Expr *value; } Re0StructFieldInit;

struct Re0Expr {
    Re0ExprKind kind; Re0Span span;
    union {
        struct { char *name; } ident;
        struct { int64_t val; } int_lit;
        struct { double val; } float_lit;
        struct { char *val; } str_lit;
        struct { char val; } char_lit;
        struct { bool val; } bool_lit;
        Re0ExprTuple tuple;
        Re0ExprBinary binary;
        Re0ExprUnary unary;
        Re0ExprCall call;
        Re0ExprIndex index;
        Re0ExprSelect select;
        Re0ExprArray array;
        Re0ExprArrayRepeat array_repeat;
        Re0ExprIf if_expr;
        Re0ExprBlock block;
        struct { Re0Expr *inner; } try_;
        Re0ExprMatch match_;
        struct { Re0LambdaParam *params; int param_count; Re0Expr *body; } lambda;
        struct { char *name; Re0StructFieldInit *fields; int field_count; } struct_init;
    };
};

VEC_DECLARE(Re0ExprVec, Re0Expr*)

typedef struct { char *name; char *ptype; } Re0FnParam;
typedef struct { char *pname; char *ptype; } Re0FnCallParam;
typedef struct { char *name; char *type; } Re0StructFieldDecl;
typedef struct { char *vname; Re0Expr **types; int type_count; } Re0EnumVariantDecl;
typedef struct { char *mname; Re0FnCallParam *params; int param_count; char *ret_type; } Re0TraitMethodDecl;
typedef struct { char *name; Re0FnCallParam *params; int param_count; char *ret_type; bool variadic; } Re0ExternFnDecl;
typedef struct { Re0Expr *cond; Re0Stmt **body; int body_count; } Re0IfBranch;

struct Re0Stmt {
    Re0StmtKind kind; Re0Span span;
    union {
        struct { Re0Expr *expr; } expr_stmt;
        struct { char *name; char *type; Re0Expr *init; } let_stmt;
        struct { char *name; Re0Expr *value; Re0BinOpKind op; } assign;
        struct { Re0Expr *obj; char *field; Re0Expr *value; } field_assign;
        struct { Re0IfBranch *branches; int branch_count; Re0Stmt **else_body; int else_count; } if_stmt;
        struct { Re0Expr *cond; Re0Stmt **body; int body_count; } while_stmt;
        struct { char *var; Re0Expr *iter; Re0Stmt **body; int body_count; } for_stmt;
        struct { Re0Expr *value; } return_stmt;
        struct { Re0Expr *value; } break_stmt;
        struct { char *name; Re0FnParam *params; int param_count; char *ret_type; Re0Stmt **body; int body_count; char **type_params; int type_param_count; bool is_async; } function;
        struct { char *name; Re0StructFieldDecl *fields; int field_count; char **type_params; int type_param_count; } struct_decl;
        struct { char *name; Re0EnumVariantDecl *variants; int variant_count; } enum_decl;
        struct { char *name; Re0TraitMethodDecl *methods; int method_count; } trait_decl;
        struct { char *name; char *trait_name; Re0Stmt **methods; int method_count; char **type_params; int type_param_count; } impl;
        struct { char *module; char **items; int item_count; } import;
        struct { char *name; Re0Stmt **body; int body_count; } module;
        struct { Re0ExternFnDecl *funcs; int func_count; } extern_;
        struct { char *name; char *type; Re0Expr *value; } const_decl;
        struct { char *name; char *target; } type_alias;
        struct { char *name; Re0StructFieldDecl *state; int state_count; Re0Stmt **methods; int method_count; } component;
        struct { Re0Stmt *inner; } pub;
        struct { char *attr_name; char *attr_arg; Re0Stmt *inner; } attribute;
    };
};

VEC_DECLARE(Re0StmtVec, Re0Stmt*)

Re0Expr *re0_expr_make(Re0ExprKind kind, Re0Span span);
Re0Stmt *re0_stmt_make(Re0StmtKind kind, Re0Span span);

#endif
