#ifndef RE0_C_STORAGE_H
#define RE0_C_STORAGE_H
#include "backend/backend.h"

typedef enum { C_STORAGE_ARRAY, C_STORAGE_SLICE, C_STORAGE_VECTOR,
               C_STORAGE_RECORD, C_STORAGE_ENUM, C_STORAGE_TUPLE,
               C_STORAGE_FUNCTION, C_STORAGE_RESULT, C_STORAGE_POINTER } CStorageKind;
typedef struct {
    CStorageKind kind;
    char name[128];
    char tag[128];
    char element[128];
    size_t length;
    const char *key;
    int state;
} CStorageType;
void c_storage_begin(Re0Codegen *c, Re0StmtVec *declarations);
void c_storage_finish(Re0Codegen *c);
void c_storage_destroy(void);
const char *c_storage_type(const Re0Type *type);
const char *c_storage_text(const char *text);
const char *c_storage_return(const char *text);
const CStorageType *c_storage_find(const char *name);
const char *c_storage_sequence(const char *element, size_t count, bool slice);
const char *c_storage_pointer(const char *element, bool mutable_);
const char *c_storage_generic(const char *name, const char *argument);
bool c_storage_expr_sequence(Re0Expr *e, char *name, size_t size);
void c_storage_fail(const char *message);
size_t c_storage_bind(char **parameters, char **arguments, int count);
void c_storage_unbind(size_t mark);
int c_storage_capture(char names[8][64],char arguments[8][128]);
void c_gen_lvalue(Re0Codegen *c, Re0Expr *e);
void c_gen_sequence_expr(Re0Codegen *c, Re0Expr *e);
void c_gen_sequence_set(Re0Codegen *c, Re0Stmt *s);
void c_gen_store(Re0Codegen *c, Re0Expr *target, Re0Expr *value, Re0BinOpKind op);
#endif
