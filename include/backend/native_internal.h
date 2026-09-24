#ifndef RE0_NATIVE_INTERNAL_H
#define RE0_NATIVE_INTERNAL_H
#include "backend/native.h"
#include <stdint.h>

enum {
    N_MAX_FUNCTIONS = 1024, N_MAX_LOCALS = 1024, N_MAX_IR = 65536,
    N_MAX_TOTAL_IR = 262144, N_MAX_DEPTH = 128, N_MAX_PARAMS = 6,
    N_MAX_TEXT = 16 * 1024 * 1024, N_STACK_ALIGN = 16, N_WORD = 8
};
typedef enum {
    N_UNIT, N_I64, N_U64, N_BOOL,
    N_I8, N_I16, N_I32, N_ISIZE, N_U8, N_U16, N_U32, N_USIZE, N_CHAR,
    N_F32, N_F64,
    N_INVALID
} NType;
typedef enum {
    N_CONST, N_LOAD, N_STORE, N_DROP, N_BINARY, N_UNARY, N_CALL,
    N_LABEL, N_JUMP, N_JZ, N_RETURN, N_ASSERT, N_END, N_CONVERT
} NOp;
/* Stack IR: expressions produce one word (including unit = 0).
 * Branches consume their condition; calls consume args and produce one word.
 * Lowering checks types, verification checks CFG edges and stack heights. */
typedef struct {
    NOp op;
    NType type;
    uint64_t value;
    size_t arg;
    int depth; /* verified input stack height; -1 means unreachable */
} NInst;
typedef struct {
    const char *name; /* borrowed from AST */
    NType result, params[N_MAX_PARAMS];
    int param_count;
    Re0Stmt *ast; /* NULL for extern */
    NInst *ir;
    size_t count, capacity, slots, labels, offset, size;
} NFunction;
typedef struct { size_t offset, symbol; } NReloc;
typedef struct {
    Re0Codegen *codegen;
    NFunction *functions;
    size_t count, total_ir;
    Re0Buffer text;
    NReloc *relocs;
    size_t reloc_count, reloc_capacity;
    size_t entry_offset, entry_size;
} NModule;

void n_error(NModule *m, Re0Span span, const char *message);
Re0TypeKind n_type_kind(NType type);
unsigned n_type_bits(NType type);
bool n_type_signed(NType type);
bool n_type_integer(NType type);
bool n_type_float(NType type);
void n_x64_float_binary(Re0Buffer *b, Re0BinOpKind op, NType type);
void n_x64_float_convert(Re0Buffer *b, NType from, NType to);
bool n_lower(NModule *m, Re0StmtVec *checked);
bool n_verify(NModule *m, NFunction *f);
bool n_encode(NModule *m);
bool n_elf(NModule *m);
void n_put(Re0Buffer *b, uint64_t value, unsigned bytes);
void n_patch(Re0Buffer *b, size_t offset, uint64_t value, unsigned bytes);
#endif
