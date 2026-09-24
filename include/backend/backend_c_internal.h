#ifndef RE0_BACKEND_C_INTERNAL_H
#define RE0_BACKEND_C_INTERNAL_H

/* Shared internals of the C backend, split by responsibility:
 *   backend_c_type.c    - variable/type tracking and C type inference
 *   backend_c_generic.c - generic struct/fn monomorphization
 *   backend_c_expr.c    - expression code generation
 *   backend_c_stmt.c    - statement code generation + state reset
 *   backend_c.c         - backend glue (c_begin/c_end, Re0Backend)
 * c_storage.c owns precise representations; c_sequence.c lowers sequence access.
 * This header is private to the C backend. */

#include "backend/backend.h"
#include "backend/runtime_c.h"
#include "backend/c_storage.h"
#include "base/safe.h"
#include "base/re0_limits.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_VAR_TYPES RE0_MAX_VAR_TYPES
#define MAX_FN_RETS RE0_MAX_FN_RETS
#define MAX_LAMBDAS RE0_MAX_LAMBDAS
#define MAX_GENERIC_STRUCTS RE0_MAX_GENERIC_STRUCTS
#define MAX_INSTANTIATED RE0_MAX_INSTANTIATED
#define MAX_GENERIC_FNS RE0_MAX_GENERIC_FNS
#define MAX_STRUCT_FIELDS 512

typedef struct {
    char *name;
    char c_type[128];
    bool is_float;
    bool is_string;
} Re0CVarType;
typedef struct { char name[128]; char ret_c_type[128]; } FnRetSlot;
typedef struct { char struct_name[64]; char field[64]; char c_type[128]; } StructFieldSlot;
typedef struct {
    char name[64]; Re0Expr *lambda;
    char result[128], parameters[64][128];
    char bindings[8][64], arguments[8][128];
    int binding_count;
    int parameter_count;
    bool emitted;
} LambdaSlot;
typedef struct { const char *name; Re0Stmt *def; } GenericStructSlot;
typedef struct { const char *name; Re0Stmt *def; } GenericFnSlot;
typedef struct { char name[256]; } InstantiatedSlot;
typedef struct {
    Re0Stmt *def;
    char type_args[8][64];
    int type_arg_count;
    char mangled[256];
    bool emitted;
} PendingInst;

#if defined(_MSC_VER)
#define RE0_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define RE0_THREAD_LOCAL __thread
#else
#define RE0_THREAD_LOCAL _Thread_local
#endif

/* global backend state (defined in backend_c_type.c, reset by reset_c_state) */
extern RE0_THREAD_LOCAL Re0CVarType var_types[MAX_VAR_TYPES];
extern RE0_THREAD_LOCAL int var_type_count;
extern RE0_THREAD_LOCAL FnRetSlot g_fn_rets[MAX_FN_RETS];
extern RE0_THREAD_LOCAL int g_fn_ret_count;
extern RE0_THREAD_LOCAL StructFieldSlot g_struct_fields[MAX_STRUCT_FIELDS];
extern RE0_THREAD_LOCAL int g_struct_field_count;
extern RE0_THREAD_LOCAL LambdaSlot g_lambdas[MAX_LAMBDAS];
extern RE0_THREAD_LOCAL int g_lambda_count;
extern RE0_THREAD_LOCAL int g_lambda_counter;
extern RE0_THREAD_LOCAL size_t g_fwd_insert_pos;
extern RE0_THREAD_LOCAL GenericStructSlot g_generic_structs[MAX_GENERIC_STRUCTS];
extern RE0_THREAD_LOCAL int g_generic_struct_count;
extern RE0_THREAD_LOCAL GenericFnSlot g_generic_fns[MAX_GENERIC_FNS];
extern RE0_THREAD_LOCAL int g_generic_fn_count;
extern RE0_THREAD_LOCAL InstantiatedSlot g_instantiated[MAX_INSTANTIATED];
extern RE0_THREAD_LOCAL int g_instantiated_count;
extern RE0_THREAD_LOCAL PendingInst g_pending_list[MAX_INSTANTIATED];
extern RE0_THREAD_LOCAL int g_pending_count;

/* shared functions (see the owning file above) */
void track_var(const char *name, const char *ctype);
const char *var_c_type(const char *name);
bool var_is_float(const char *name);
bool var_is_string(const char *name);
bool builtin_returns_string(const char *fn);
bool builtin_returns_float(const char *fn);
bool builtin_returns_vec(const char *fn);
bool builtin_returns_svec(const char *fn);
void clear_var_types(void);
bool infer_expr_c_type(Re0Expr *e, char *type, size_t type_size);
const char *reo_type_to_c(const char *t);
int c_gen_cast(Re0Codegen *c, Re0Expr *e);
Re0TypeKind c_expr_scalar_kind(Re0Expr *e);
const char *binop_c(Re0BinOpKind op);
bool split_qualified(const char *name, char *enum_name, int elen, char *variant, int vlen);
bool expr_is_float(Re0Expr *e);
bool expr_is_string(Re0Expr *e);
bool expr_is_array_var(Re0Expr *e);
bool expr_is_vec(Re0Expr *e);
bool expr_is_pointer_obj(Re0Expr *e);
bool expr_is_u128(Re0Expr *e);
bool expr_is_i128(Re0Expr *e);
const char *c_wider_int_type(const char *a, const char *b);
void c_write_string_literal(Re0Buffer *b, const char *value);
void c_write_char_literal(Re0Buffer *b, char c);
void track_fn_ret(const char *name, const char *reo_ret);
const char *fn_ret_c_type(const char *name);
void track_struct_field(const char *struct_name, const char *field, const char *reo_type);
const char *struct_field_c_type(const char *struct_name, const char *field);
void register_generic_struct(const char *name, Re0Stmt *def);
Re0Stmt *find_generic_struct(const char *name);
const char *instantiate_generic_struct(Re0Codegen *c, const char *base_name, const char *inferred, char *out, size_t out_sz);
const char *try_instantiate_generic_struct_init(Re0Codegen *c, Re0Expr *e, char *out, size_t out_sz);
void register_generic_fn(const char *name, Re0Stmt *def);
Re0Stmt *find_generic_fn(const char *name);
bool is_already_instantiated(const char *mangled);
void mark_instantiated(const char *mangled);
void instantiate_generic_fn(Re0Codegen *c, Re0Stmt *def, char **type_args, int type_arg_count);
bool c_generic_mangle(const char *name, char **arguments, int count, char *out, size_t size);
void flush_pending_instantiations(Re0Codegen *c);
void flush_lambdas(Re0Codegen *c);
const char *try_instantiate_generic_call(Re0Codegen *c, const char *fn_name, Re0Expr **args, int arg_count, char *out, size_t out_sz);
int c_gen_expr(Re0Codegen *c, Re0Expr *e);
void c_gen_stmt(Re0Codegen *c, Re0Stmt *s, int depth);
void c_gen_body(Re0Codegen *c, Re0Stmt **body, int count, int depth);
void c_gen_extern_decl(Re0Codegen *c, const Re0ExternFnDecl *decl);
void reset_c_state(void);

#endif
