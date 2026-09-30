#include "backend/native_internal.h"
#include "backend/native_target.h"
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

unsigned n_type_bits(const Re0NativeTarget *target, NType type) {
    Re0TypeKind kind = n_type_kind(type);
    if (kind == RE0_TYPE_ISIZE || kind == RE0_TYPE_USIZE)
        return target->pointer_size * 8;
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

void n_reloc_add(NModule *m, size_t offset, size_t symbol) {
    if (m->reloc_count >= N_MAX_TOTAL_IR) { n_error(m, RE0_SPAN_ZERO, "relocation limit exceeded"); return; }
    if (m->reloc_count == m->reloc_capacity) {
        size_t cap = m->reloc_capacity ? m->reloc_capacity * 2 : 64;
        NReloc *p = realloc(m->relocs, cap * sizeof(*p));
        if (!p) { n_error(m, RE0_SPAN_ZERO, "cannot allocate relocations"); return; }
        m->relocs = p; m->reloc_capacity = cap;
    }
    m->relocs[m->reloc_count++] = (NReloc){offset, symbol};
}

bool n_encode(NModule *m) {
    switch (m->target->arch) {
        case RE0_ARCH_X86_64: return n_encode_x64(m);
        case RE0_ARCH_X86: return n_encode_x86(m);
        case RE0_ARCH_AARCH64: return n_encode_a64(m);
        case RE0_ARCH_ARM: return n_encode_arm(m);
        default:
            n_error(m, RE0_SPAN_ZERO, "code generation for this target is not implemented");
            return false;
    }
}

bool n_object(NModule *m) {
    switch (m->target->object) {
        case RE0_OBJ_ELF64: return n_elf64(m);
        case RE0_OBJ_ELF32: return n_elf32(m);
        case RE0_OBJ_MACHO64: return n_macho(m);
        default:
            n_error(m, RE0_SPAN_ZERO, "object format for this target is not implemented");
            return false;
    }
}

bool re0_native_generate(Re0Codegen *c, Re0StmtVec *checked) {
    if (!c || !checked) return false;
    const Re0NativeTarget *target = c->native_target ? c->native_target : re0_native_target_host();
    NModule m = {.codegen = c, .target = target};
    re0_buffer_init(&m.text);
    re0_buffer_clear(&c->output);
    if (!target) {
        n_error(&m, RE0_SPAN_ZERO, "no native target for this host; pass --target <triple>");
        re0_buffer_free(&m.text);
        return false;
    }
    m.functions = calloc(N_MAX_FUNCTIONS, sizeof(*m.functions));
    if (!m.functions) n_error(&m, RE0_SPAN_ZERO, "cannot allocate module");
    bool ok = m.functions && n_lower(&m, checked);
    for (size_t i = 0; ok && i < m.count; i++)
        if (m.functions[i].ast) ok = n_verify(&m, &m.functions[i]);
    if (ok) ok = n_encode(&m);
    if (ok) ok = n_object(&m);
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
