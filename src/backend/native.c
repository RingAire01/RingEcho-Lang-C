#include "backend/native_internal.h"
#include "analysis/layout.h"
#include <stdlib.h>

Re0Backend re0_backend_native = {"native", NULL, NULL, NULL, NULL};

Re0TypeKind n_type_kind(NType type) {
    static const Re0TypeKind kinds[] = {
        RE0_TYPE_UNIT, RE0_TYPE_I64, RE0_TYPE_U64, RE0_TYPE_BOOL,
        RE0_TYPE_I8, RE0_TYPE_I16, RE0_TYPE_I32, RE0_TYPE_ISIZE,
        RE0_TYPE_U8, RE0_TYPE_U16, RE0_TYPE_U32, RE0_TYPE_USIZE, RE0_TYPE_CHAR,
        RE0_TYPE_F32, RE0_TYPE_F64
    };
    return type < N_UNIT || type >= N_INVALID ? RE0_TYPE_UNKNOWN : kinds[type];
}

unsigned n_type_bits(NType type) {
    Re0TypeKind kind = n_type_kind(type);
    if (kind == RE0_TYPE_ISIZE || kind == RE0_TYPE_USIZE)
        return re0_target_x86_64_sysv.pointer_size * 8;
    return (unsigned)re0_type_sizeof(kind) * 8;
}
bool n_type_signed(NType type) { return re0_type_is_signed(n_type_kind(type)); }
bool n_type_integer(NType type) { return re0_type_is_integer(n_type_kind(type)); }
bool n_type_float(NType type) { return type == N_F32 || type == N_F64; }

void n_error(NModule *m, Re0Span span, const char *message) {
    if (!m->codegen->had_error)
        re0_error_append(m->codegen->errors, RE0_ERR_SEMANTIC, span, NULL,
                         "native backend: %s", message);
    m->codegen->had_error = true;
}

void n_put(Re0Buffer *b, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        re0_buffer_write_char(b, (char)(value & 255));
        value >>= 8;
    }
}

void n_patch(Re0Buffer *b, size_t offset, uint64_t value, unsigned bytes) {
    if (offset > b->len || bytes > b->len - offset) { b->failed = true; return; }
    for (unsigned i = 0; i < bytes; i++) {
        b->data[offset + i] = (char)(value & 255);
        value >>= 8;
    }
}

bool re0_native_generate(Re0Codegen *c, Re0StmtVec *checked) {
    if (!c || !checked) return false;
    NModule m = {.codegen = c};
    re0_buffer_init(&m.text);
    re0_buffer_clear(&c->output);
    m.functions = calloc(N_MAX_FUNCTIONS, sizeof(*m.functions));
    if (!m.functions) n_error(&m, RE0_SPAN_ZERO, "cannot allocate module");
    bool ok = m.functions && n_lower(&m, checked);
    for (size_t i = 0; ok && i < m.count; i++)
        if (m.functions[i].ast) ok = n_verify(&m, &m.functions[i]);
    if (ok) ok = n_encode(&m) && n_elf(&m);
    if (re0_buffer_failed(&m.text) || re0_buffer_failed(&c->output)) {
        n_error(&m, RE0_SPAN_ZERO, "cannot allocate generated output");
        ok = false;
    }
    for (size_t i = 0; i < m.count; i++) free(m.functions[i].ir);
    free(m.functions);
    free(m.relocs);
    re0_buffer_free(&m.text);
    return ok && !c->had_error;
}
