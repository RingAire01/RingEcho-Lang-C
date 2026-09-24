#include "backend/native_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct { size_t offset, label; } Branch;

/* All encodings are little endian; no host instruction execution or host ABI
 * assumptions are used while emitting. RAX is the accumulator, RCX/RDX scratch.
 * Expression values live on the stack, locals at fixed RBP-relative slots. */
static void bytes(Re0Buffer *b, const unsigned char *p, size_t n) {
    re0_buffer_write_n(b, (const char *)p, n);
}
#define CODE(b, ...) do { const unsigned char code_[] = {__VA_ARGS__}; bytes((b), code_, sizeof(code_)); } while (0)

static void imm64(Re0Buffer *b, uint64_t value) { CODE(b, 0x48, 0xb8); n_put(b, value, 8); }
static void boolean(Re0Buffer *b) { CODE(b, 0x48,0x85,0xc0, 0x0f,0x95,0xc0, 0x0f,0xb6,0xc0); }
static void normalize(Re0Buffer *b, NType type) {
    if (type == N_BOOL) { boolean(b); return; }
    unsigned bits = n_type_bits(type);
    bool sign = n_type_signed(type);
    if (bits == 8) {
        if (sign) CODE(b, 0x48,0x0f,0xbe,0xc0);
        else CODE(b, 0x0f,0xb6,0xc0);
    } else if (bits == 16) {
        if (sign) CODE(b, 0x48,0x0f,0xbf,0xc0);
        else CODE(b, 0x0f,0xb7,0xc0);
    } else if (bits == 32) {
        if (sign) CODE(b, 0x48,0x63,0xc0);
        else CODE(b, 0x89,0xc0);
    }
}
static void slot(Re0Buffer *b, size_t index, bool store) {
    CODE(b, 0x48, store ? 0x89 : 0x8b, 0x85);
    n_put(b, (uint32_t)(0u - (uint32_t)((index + 1) * N_WORD)), 4);
}

static void relocation(NModule *m, size_t symbol) {
    if (m->reloc_count >= N_MAX_TOTAL_IR) { n_error(m, RE0_SPAN_ZERO, "relocation limit exceeded"); return; }
    if (m->reloc_count == m->reloc_capacity) {
        size_t cap = m->reloc_capacity ? m->reloc_capacity * 2 : 64;
        NReloc *p = realloc(m->relocs, cap * sizeof(*p));
        if (!p) { n_error(m, RE0_SPAN_ZERO, "cannot allocate relocations"); return; }
        m->relocs = p; m->reloc_capacity = cap;
    }
    CODE(&m->text, 0xe8);
    m->relocs[m->reloc_count++] = (NReloc){m->text.len, symbol};
    n_put(&m->text, 0, 4);
}

static void binary(NModule *m, NInst *in) {
    Re0Buffer *b = &m->text;
    CODE(b, 0x59, 0x58); /* pop right -> rcx; left -> rax */
    if (n_type_float(in->type)) {
        n_x64_float_binary(b, (Re0BinOpKind)in->arg, in->type);
        CODE(b, 0x50);
        return;
    }
    switch ((Re0BinOpKind)in->arg) {
        case BINOP_ADD: CODE(b, 0x48,0x01,0xc8); break;
        case BINOP_SUB: CODE(b, 0x48,0x29,0xc8); break;
        case BINOP_MUL: CODE(b, 0x48,0x0f,0xaf,0xc1); break;
        case BINOP_DIV: case BINOP_MOD:
            if (n_type_signed(in->type) && n_type_bits(in->type) < 64) {
                /* idivq alone cannot detect overflow at a narrower width. */
                CODE(b, 0x48,0x83,0xf9,0xff, 0x75,0x0a, 0x48,0x3d);
                n_put(b, 0u - (UINT32_C(1) << (n_type_bits(in->type) - 1)), 4);
                CODE(b, 0x75,0x02, 0x0f,0x0b);
            }
            if (n_type_signed(in->type)) CODE(b, 0x48,0x99, 0x48,0xf7,0xf9);
            else CODE(b, 0x31,0xd2, 0x48,0xf7,0xf1);
            if (in->arg == BINOP_MOD) CODE(b, 0x48,0x89,0xd0);
            break;
        case BINOP_BAND: CODE(b, 0x48,0x21,0xc8); break;
        case BINOP_BOR: CODE(b, 0x48,0x09,0xc8); break;
        case BINOP_BXOR: CODE(b, 0x48,0x31,0xc8); break;
        case BINOP_SHL: case BINOP_SHR:
            if (n_type_bits(in->type) < 64) CODE(b, 0x80,0xe1,(unsigned char)(n_type_bits(in->type) - 1));
            CODE(b, 0x48,0xd3,in->arg == BINOP_SHL ? 0xe0 : n_type_signed(in->type) ? 0xf8 : 0xe8);
            break;
        case BINOP_EQ: case BINOP_NE: case BINOP_LT: case BINOP_LE: case BINOP_GT: case BINOP_GE: {
            unsigned char condition = 0x94;
            bool sign = n_type_signed(in->type);
            switch ((Re0BinOpKind)in->arg) {
                case BINOP_NE: condition = 0x95; break;
                case BINOP_LT: condition = sign ? 0x9c : 0x92; break;
                case BINOP_LE: condition = sign ? 0x9e : 0x96; break;
                case BINOP_GT: condition = sign ? 0x9f : 0x97; break;
                case BINOP_GE: condition = sign ? 0x9d : 0x93; break;
                default: break;
            }
            CODE(b, 0x48,0x39,0xc8, 0x0f,condition,0xc0, 0x0f,0xb6,0xc0); break;
        }
        default: n_error(m, RE0_SPAN_ZERO, "invalid binary IR opcode"); break;
    }
    normalize(b, in->type);
    CODE(b, 0x50);
}

static void function(NModule *m, NFunction *f) {
    Re0Buffer *b = &m->text;
    size_t *labels = calloc(f->labels + 1, sizeof(*labels));
    Branch *branches = calloc(f->count + 1, sizeof(*branches));
    if (!labels || !branches) {
        free(labels); free(branches); n_error(m, f->ast->span, "cannot allocate branch fixups"); return;
    }
    f->offset = b->len;
    CODE(b, 0x55, 0x48,0x89,0xe5, 0x48,0x81,0xec);
    size_t frame = (f->slots * N_WORD + N_STACK_ALIGN - 1) & ~(size_t)(N_STACK_ALIGN - 1);
    n_put(b, frame, 4);
    static const unsigned char registers[N_MAX_PARAMS] = {7,6,2,1,8,9};
    unsigned gp = 0, fp = 0;
    for (int i = 0; i < f->param_count; i++) {
        if (n_type_float(f->params[i])) {
            CODE(b, 0x66);
            if (f->params[i] == N_F64) CODE(b, 0x48);
            CODE(b, 0x0f,0x7e,(unsigned char)(0xc0 | (fp++ << 3)));
        } else {
            unsigned char reg = registers[gp++];
            CODE(b, reg >= 8 ? 0x4c : 0x48, 0x89, (unsigned char)(0xc0 | ((reg & 7) << 3)));
        }
        if (f->params[i] == N_BOOL) { CODE(b, 0x0f,0xb6,0xc0); boolean(b); }
        else normalize(b, f->params[i]);
        slot(b, (size_t)i, true);
    }
    size_t branch_count = 0;
    for (size_t i = 0; i < f->count && !m->codegen->had_error; i++) {
        NInst *in = &f->ir[i];
        if (in->op == N_LABEL) labels[in->arg] = b->len;
        if (in->depth < 0) continue;
        switch (in->op) {
            case N_CONST: imm64(b, in->value); normalize(b, in->type); CODE(b, 0x50); break;
            case N_LOAD: slot(b, in->arg, false); CODE(b, 0x50); break;
            case N_STORE: CODE(b, 0x58); normalize(b, in->type); slot(b, in->arg, true); break;
            case N_DROP: CODE(b, 0x58); break;
            case N_BINARY: binary(m, in); break;
            case N_UNARY:
                CODE(b, 0x58);
                if (in->arg == UNOP_NEG && n_type_float(in->type)) {
                    CODE(b, 0x48,0xb9); n_put(b, UINT64_C(1) << (n_type_bits(in->type) - 1), 8);
                    CODE(b, 0x48,0x31,0xc8);
                } else if (in->arg == UNOP_NEG) CODE(b, 0x48,0xf7,0xd8);
                else if (in->arg == UNOP_BNOT) CODE(b, 0x48,0xf7,0xd0);
                else if (in->arg == UNOP_NOT) CODE(b, 0x48,0x85,0xc0, 0x0f,0x94,0xc0, 0x0f,0xb6,0xc0);
                else n_error(m, f->ast->span, "invalid unary IR opcode");
                normalize(b, in->type);
                CODE(b, 0x50); break;
            case N_CONVERT:
                CODE(b, 0x58);
                if (n_type_float((NType)in->arg) || n_type_float(in->type))
                    n_x64_float_convert(b, (NType)in->arg, in->type);
                normalize(b, in->type); CODE(b, 0x50); break;
            case N_CALL: {
                NFunction *target = &m->functions[in->arg];
                unsigned assigned[N_MAX_PARAMS], call_gp = 0, call_fp = 0;
                for (int j = 0; j < target->param_count; j++)
                    assigned[j] = n_type_float(target->params[j]) ? call_fp++ : registers[call_gp++];
                for (int j = target->param_count; j > 0; j--) {
                    unsigned reg = assigned[j - 1];
                    if (n_type_float(target->params[j - 1])) {
                        CODE(b, 0x58, 0x66);
                        if (target->params[j - 1] == N_F64) CODE(b, 0x48);
                        CODE(b, 0x0f,0x6e,(unsigned char)(0xc0 | (reg << 3)));
                    } else {
                        if (reg >= 8) CODE(b, 0x41);
                        CODE(b, (unsigned char)(0x58 + (reg & 7)));
                    }
                }
                bool pad = ((in->depth - target->param_count) & 1) != 0;
                if (pad) CODE(b, 0x48,0x83,0xec,0x08);
                relocation(m, in->arg);
                if (pad) CODE(b, 0x48,0x83,0xc4,0x08);
                if (n_type_float(target->result)) {
                    CODE(b, 0x66);
                    if (target->result == N_F64) CODE(b, 0x48);
                    CODE(b, 0x0f,0x7e,0xc0);
                }
                if (target->result == N_BOOL) { CODE(b, 0x0f,0xb6,0xc0); boolean(b); }
                else normalize(b, target->result);
                if (target->result == N_UNIT) CODE(b, 0x31,0xc0);
                CODE(b, 0x50); break;
            }
            case N_LABEL: break;
            case N_JUMP: case N_JZ:
                if (in->op == N_JZ) CODE(b, 0x58, 0x48,0x85,0xc0, 0x0f,0x84);
                else CODE(b, 0xe9);
                branches[branch_count++] = (Branch){b->len, in->arg}; n_put(b, 0, 4); break;
            case N_RETURN:
                CODE(b, 0x58); normalize(b, in->type);
                if (n_type_float(in->type)) {
                    CODE(b, 0x66);
                    if (in->type == N_F64) CODE(b, 0x48);
                    CODE(b, 0x0f,0x6e,0xc0);
                }
                CODE(b, 0xc9,0xc3); break;
            case N_END: CODE(b, 0x31,0xc0, 0xc9,0xc3); break;
            case N_ASSERT: CODE(b, 0x58, 0x48,0x85,0xc0, 0x75,0x02, 0x0f,0x0b); break;
        }
        if (b->len > N_MAX_TEXT) n_error(m, f->ast->span, "native text size limit exceeded");
        if (re0_buffer_failed(b)) n_error(m, f->ast->span, "cannot allocate machine code");
    }
    for (size_t i = 0; i < branch_count; i++) {
        Branch *fix = &branches[i];
        int64_t delta = (int64_t)labels[fix->label] - (int64_t)(fix->offset + 4);
        n_patch(b, fix->offset, (uint32_t)delta, 4);
    }
    f->size = b->len - f->offset;
    free(labels); free(branches);
}

bool n_encode(NModule *m) {
    for (size_t i = 0; i < m->count && !m->codegen->had_error; i++)
        if (m->functions[i].ast) function(m, &m->functions[i]);
    if (m->codegen->emit_main && !m->codegen->had_error) {
        size_t main_index = 0;
        while (main_index < m->count && strcmp(m->functions[main_index].name, "main") != 0) main_index++;
        if (main_index == m->count || !m->functions[main_index].ast || m->functions[main_index].param_count) {
            n_error(m, RE0_SPAN_ZERO, "executable requires a defined main() with no parameters"); return false;
        }
        if (n_type_float(m->functions[main_index].result)) {
            n_error(m, RE0_SPAN_ZERO, "main must return unit or an integer/boolean exit status"); return false;
        }
        m->entry_offset = m->text.len;
        CODE(&m->text, 0x31,0xed, 0x48,0x83,0xe4,0xf0); /* clear rbp; align rsp */
        relocation(m, main_index);
        CODE(&m->text, 0x48,0x89,0xc7, 0xb8,0x3c,0,0,0, 0x0f,0x05, 0x0f,0x0b);
        m->entry_size = m->text.len - m->entry_offset;
    }
    return !m->codegen->had_error && !re0_buffer_failed(&m->text);
}
