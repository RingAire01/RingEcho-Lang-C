#include "backend/native_internal.h"
#include <stdlib.h>
#include <string.h>

/* AArch64 (AAPCS64) encoder for the integer subset. Every IR value is one
 * 8-byte slot on the machine stack (SP grows down); X0 is the accumulator and
 * X1/X2 are scratch. Callee-saved registers are untouched. Arguments use
 * X0-X7 and returns use X0; at most six parameters fit in registers, so no
 * stack arguments are emitted. Floating point is not implemented yet and is
 * rejected explicitly. */

typedef struct { size_t offset; size_t label; uint32_t base; unsigned rt; bool is26; } Branch;

static void inst(Re0Buffer *b, uint32_t word) { n_put(b, word, 4); }

/* MOVZ/MOVK a 64-bit constant into Rd. */
static void mov_imm(Re0Buffer *b, unsigned rd, uint64_t value) {
    inst(b, 0xD2800000u | ((uint32_t)(value & 0xFFFF) << 5) | rd); /* movz rd, #v0 */
    for (unsigned shift = 1; shift < 4; shift++) {
        uint32_t chunk = (uint32_t)((value >> (16 * shift)) & 0xFFFF);
        if (chunk) inst(b, 0xF2800000u | (shift << 21) | (chunk << 5) | rd); /* movk */
    }
}
static void mov_reg(Re0Buffer *b, unsigned rd, unsigned rm) { inst(b, 0xAA0003E0u | (rm << 16) | rd); }
static void str_sp(Re0Buffer *b, unsigned rt, uint32_t off) { inst(b, 0xF9000000u | ((off >> 3) << 10) | (31 << 5) | rt); }
static void ldr_sp(Re0Buffer *b, unsigned rt, uint32_t off) { inst(b, 0xF9400000u | ((off >> 3) << 10) | (31 << 5) | rt); }
/* AAPCS64 requires the stack pointer to be 16-byte aligned whenever memory is
 * accessed through it, so every value slot is 16 bytes. */
static void push_x0(Re0Buffer *b) { inst(b, 0xD10043FFu); str_sp(b, 0, 0); } /* sub sp,sp,#16; str x0,[sp] */
static void pop_x0(Re0Buffer *b) { ldr_sp(b, 0, 0); inst(b, 0x910043FFu); }   /* ldr x0,[sp]; add sp,sp,#16 */

static void ldr_x29(Re0Buffer *b, unsigned rt, int32_t off) {
    if (off >= -256 && off <= 255) {
        inst(b, 0xF8400000u | (((uint32_t)off & 0x1FF) << 12) | (29 << 5) | rt); /* ldur */
    } else {
        mov_imm(b, 2, (uint64_t)(-off));
        inst(b, 0xCB000000u | (2 << 16) | (29 << 5) | 2);   /* sub x2,x29,x2 */
        inst(b, 0xF9400000u | (2 << 5) | rt);               /* ldr rt,[x2] */
    }
}
static void str_x29(Re0Buffer *b, unsigned rt, int32_t off) {
    if (off >= -256 && off <= 255) {
        inst(b, 0xF8000000u | (((uint32_t)off & 0x1FF) << 12) | (29 << 5) | rt); /* stur */
    } else {
        mov_imm(b, 2, (uint64_t)(-off));
        inst(b, 0xCB000000u | (2 << 16) | (29 << 5) | 2);
        inst(b, 0xF9000000u | (2 << 5) | rt);               /* str rt,[x2] */
    }
}
static size_t branch_placeholder(Re0Buffer *b, uint32_t base, unsigned rt, bool is26) {
    size_t offset = b->len;
    inst(b, base | (is26 ? 0u : rt));
    return offset;
}
static size_t b_cond_placeholder(Re0Buffer *b, unsigned cond) {
    size_t offset = b->len;
    inst(b, 0x54000000u | cond);
    return offset;
}
static void patch_branch(Re0Buffer *b, size_t offset, size_t target, uint32_t base, unsigned rt, bool is26) {
    int64_t words = ((int64_t)target - (int64_t)offset) / 4;
    if (is26) {
        if (words < -(1 << 25) || words >= (1 << 25)) b->failed = true;
        n_patch(b, offset, base | ((uint64_t)words & 0x03FFFFFFu), 4);
    } else {
        if (words < -(1 << 18) || words >= (1 << 18)) b->failed = true;
        n_patch(b, offset, base | ((uint64_t)words & 0x7FFFFu) << 5 | rt, 4);
    }
}
static void normalize(Re0Buffer *b, const Re0NativeTarget *t, NType type) {
    unsigned bits = n_type_bits(t, type);
    bool sign = n_type_signed(type);
    if (type == N_BOOL) { inst(b, 0xF100001Fu); inst(b, 0x9A9F07E0u); return; } /* cmp x0,#0; cset x0,ne */
    if (bits == 8) inst(b, sign ? 0x93401C00u : 0x53001C00u);
    else if (bits == 16) inst(b, sign ? 0x93403C00u : 0x53003C00u);
    else if (bits == 32) inst(b, sign ? 0x93407C00u : 0x2A0003E0u);
}
static void fail(NModule *m, NFunction *f, const char *msg) { n_error(m, f->ast->span, msg); }

static unsigned compare_condition(Re0BinOpKind op, bool sign) {
    switch (op) {
        case BINOP_EQ: return 0;
        case BINOP_NE: return 1;
        case BINOP_LT: return sign ? 11 : 3;
        case BINOP_LE: return sign ? 13 : 9;
        case BINOP_GT: return sign ? 12 : 8;
        case BINOP_GE: return sign ? 10 : 2;
        default: return 0;
    }
}

static void binary_integer(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    unsigned bits = n_type_bits(m->target, in->type);
    bool sign = n_type_signed(in->type);
    Re0BinOpKind op = (Re0BinOpKind)in->arg;
    ldr_sp(b, 0, 16); ldr_sp(b, 1, 0);              /* x0 = left, x1 = right */
    if (op >= BINOP_EQ && op <= BINOP_GE) {
        inst(b, 0xEB00001F | (1 << 16));            /* cmp x0, x1 */
        unsigned cond = compare_condition(op, sign);
        inst(b, 0x9A9F07E0u | ((cond ^ 1) << 12));  /* cset x0, cond */
    } else {
        switch (op) {
            case BINOP_ADD: inst(b, 0x8B010000u); break;
            case BINOP_SUB: inst(b, 0xCB010000u); break;
            case BINOP_MUL: inst(b, 0x9B017C00u); break;
            case BINOP_DIV: case BINOP_MOD: {
                /* AArch64 division does not trap; reproduce the C/x86-64
                 * behaviour of trapping on divide-by-zero and INT64_MIN/-1. */
                size_t nonzero = branch_placeholder(b, 0xB5000000u, 1, false); /* cbnz x1 */
                inst(b, 0x00000000u);               /* udf */
                patch_branch(b, nonzero, b->len, 0xB5000000u, 1, false);
                if (sign) {
                    mov_imm(b, 2, UINT64_C(0x8000000000000000));
                    inst(b, 0xEB02001Fu);           /* cmp x0, x2 */
                    size_t ok = b_cond_placeholder(b, 1); /* b.ne */
                    mov_imm(b, 2, UINT64_C(0xFFFFFFFFFFFFFFFF));
                    inst(b, 0xEB02003Fu);           /* cmp x1, x2 */
                    size_t ok2 = b_cond_placeholder(b, 1);
                    inst(b, 0x00000000u);           /* udf */
                    patch_branch(b, ok, b->len, 0x54000000u, 1, false);
                    patch_branch(b, ok2, b->len, 0x54000000u, 1, false);
                }
                uint32_t div_base = sign ? 0x9AC00C00u : 0x9AC00800u;
                if (op == BINOP_DIV) {
                    inst(b, div_base | (1 << 16));          /* sdiv/udiv x0,x0,x1 */
                } else {
                    inst(b, div_base | (1 << 16) | 2);      /* sdiv/udiv x2,x0,x1 */
                    inst(b, 0x9B018040u);                   /* msub x0,x2,x1,x0 */
                }
                break;
            }
            case BINOP_BAND: inst(b, 0x8A010000u); break;
            case BINOP_BOR: inst(b, 0xAA010000u); break;
            case BINOP_BXOR: inst(b, 0xCA010000u); break;
            case BINOP_SHL: case BINOP_SHR:
                if (bits < 64) { mov_imm(b, 2, bits - 1); inst(b, 0x8A020021u); } /* and x1,x1,x2 */
                if (op == BINOP_SHL) inst(b, 0x9AC12000u);
                else if (sign) inst(b, 0x9AC12800u);
                else inst(b, 0x9AC12400u);
                break;
            default: fail(m, f, "invalid aarch64 binary operation"); return;
        }
    }
    inst(b, 0x910043FFu);                            /* add sp,sp,#16 (two slots -> one) */
    str_sp(b, 0, 0);
}

static void unary(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    pop_x0(b);
    switch ((Re0UnOpKind)in->arg) {
        case UNOP_NEG: inst(b, 0xCB0003E0u); break;   /* neg x0, x0 */
        case UNOP_BNOT: inst(b, 0xAA2003E0u); break;  /* mvn x0, x0 */
        case UNOP_NOT: inst(b, 0xF100001Fu); inst(b, 0x9A9F17E0u); break; /* cmp x0,#0; cset eq */
        default: fail(m, f, "invalid aarch64 unary operation"); return;
    }
    push_x0(b);
}

static void convert(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    NType from = (NType)in->arg, to = in->type;
    if (n_type_float(from) || n_type_float(to)) { fail(m, f, "floating point is not supported on aarch64 yet"); return; }
    pop_x0(b);
    normalize(b, m->target, from);
    normalize(b, m->target, to);
    push_x0(b);
}

static void function(NModule *m, NFunction *f) {
    Re0Buffer *b = &m->text;
    size_t *labels = calloc(f->labels + 1, sizeof(*labels));
    Branch *branches = calloc(f->count + 1, sizeof(*branches));
    if (!labels || !branches) {
        free(labels); free(branches); n_error(m, f->ast->span, "cannot allocate branch fixups"); return;
    }
    f->offset = b->len;
    inst(b, 0xA9BF7BFDu);                            /* stp x29,x30,[sp,#-16]! */
    inst(b, 0x910003FDu);                            /* mov x29,sp */
    size_t frame = (f->slots * N_WORD + 15) & ~(size_t)15;
    /* ADD/SUB (immediate) is the only encoding that permits SP as Rd; subtract
     * the frame in imm12-sized chunks. */
    while (frame) {
        size_t chunk = frame > 4095 ? 4095 : frame;
        inst(b, 0xD1000000u | ((uint32_t)chunk << 10) | (31 << 5) | 31);
        frame -= chunk;
    }
    unsigned gp = 0, fp = 0;
    for (int i = 0; i < f->param_count; i++) {
        if (n_type_float(f->params[i])) { fail(m, f, "floating point parameters are not supported on aarch64 yet"); break; }
        unsigned reg = gp++;
        int32_t disp = -(int32_t)(8 * (i + 1));
        if (reg) mov_reg(b, 0, reg);                 /* mov x0, xreg */
        normalize(b, m->target, f->params[i]);
        str_x29(b, 0, disp);
    }
    (void)fp;
    size_t branch_count = 0;
    for (size_t i = 0; i < f->count && !m->codegen->had_error; i++) {
        NInst *in = &f->ir[i];
        if (in->op == N_LABEL) labels[in->arg] = b->len;
        if (in->depth < 0) continue;
        switch (in->op) {
            case N_CONST:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on aarch64 yet"); break; }
                mov_imm(b, 0, in->value);
                normalize(b, m->target, in->type);
                push_x0(b);
                break;
            case N_LOAD: { int32_t disp = -(int32_t)(8 * ((int32_t)in->arg + 1));
                ldr_x29(b, 0, disp); push_x0(b); break; }
            case N_STORE: { int32_t disp = -(int32_t)(8 * ((int32_t)in->arg + 1));
                pop_x0(b); normalize(b, m->target, in->type); str_x29(b, 0, disp); break; }
            case N_DROP: pop_x0(b); break;
            case N_BINARY:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on aarch64 yet"); break; }
                binary_integer(m, f, in); break;
            case N_UNARY: unary(m, f, in); break;
            case N_CONVERT: convert(m, f, in); break;
            case N_CALL: {
                NFunction *target = &m->functions[in->arg];
                if (n_type_float(target->result)) { fail(m, f, "floating point is not supported on aarch64 yet"); break; }
                unsigned call_gp = 0;
                for (int j = 0; j < target->param_count; j++) {
                    if (n_type_float(target->params[j])) { fail(m, f, "floating point is not supported on aarch64 yet"); break; }
                    unsigned reg = call_gp++;
                    int32_t off = 16 * (target->param_count - 1 - j); /* value j from current sp */
                    ldr_sp(b, reg, (uint32_t)off);
                }
                /* 16-byte slots keep sp 16-byte aligned, so no call padding is
                 * needed; drop the argument slots and call. */
                size_t drop = 16 * (size_t)target->param_count;
                if (drop) inst(b, 0x91000000u | ((uint32_t)drop << 10) | (31 << 5) | 31); /* add sp,sp,#drop */
                size_t at = b->len;
                inst(b, 0x94000000u);                 /* bl */
                n_reloc_add(m, at, in->arg);
                if (target->result == N_UNIT) mov_imm(b, 0, 0);
                else normalize(b, m->target, target->result);
                push_x0(b);
                break;
            }
            case N_LABEL: break;
            case N_JUMP: case N_JZ:
                if (in->op == N_JZ) {
                    pop_x0(b);
                    branches[branch_count].offset = branch_placeholder(b, 0xB4000000u, 0, false);
                    branches[branch_count].base = 0xB4000000u; branches[branch_count].rt = 0;
                    branches[branch_count].is26 = false;
                } else {
                    branches[branch_count].offset = branch_placeholder(b, 0x14000000u, 0, true);
                    branches[branch_count].base = 0x14000000u; branches[branch_count].rt = 0;
                    branches[branch_count].is26 = true;
                }
                branches[branch_count].label = in->arg;
                branch_count++;
                break;
            case N_RETURN:
                pop_x0(b);
                if (n_type_bits(m->target, in->type) <= 32) normalize(b, m->target, in->type);
                inst(b, 0x910003BFu);                 /* mov sp,x29 */
                inst(b, 0xA8C17BFDu);                 /* ldp x29,x30,[sp],#16 */
                inst(b, 0xD65F03C0u);                 /* ret */
                break;
            case N_END:
                mov_imm(b, 0, 0);
                inst(b, 0x910003BFu); inst(b, 0xA8C17BFDu); inst(b, 0xD65F03C0u);
                break;
            case N_ASSERT: {
                pop_x0(b);
                size_t ok = branch_placeholder(b, 0xB5000000u, 0, false); /* cbnz x0 */
                inst(b, 0x00000000u);
                patch_branch(b, ok, b->len, 0xB5000000u, 0, false);
                break;
            }
        }
        if (b->len > N_MAX_TEXT) n_error(m, f->ast->span, "native text size limit exceeded");
        if (re0_buffer_failed(b)) n_error(m, f->ast->span, "cannot allocate machine code");
    }
    for (size_t i = 0; i < branch_count; i++) {
        Branch *fix = &branches[i];
        patch_branch(b, fix->offset, labels[fix->label], fix->base, fix->rt, fix->is26);
    }
    f->size = b->len - f->offset;
    free(labels); free(branches);
}

bool n_encode_a64(NModule *m) {
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
        mov_imm(&m->text, 29, 0);                    /* mov x29, xzr */
        size_t at = m->text.len;
        inst(&m->text, 0x94000000u);                 /* bl main */
        n_reloc_add(m, at, main_index);
        if (m->target->os == RE0_OS_MACOS) {
            mov_imm(&m->text, 16, 1);                /* x16 = 1 (BSD exit) */
            inst(&m->text, 0xD4001001u);             /* svc #0x80 */
        } else {
            mov_imm(&m->text, 8, 93);                /* x8 = 93 (Linux exit) */
            inst(&m->text, 0xD4000001u);             /* svc #0 */
        }
        inst(&m->text, 0x00000000u);                 /* udf */
        m->entry_size = m->text.len - m->entry_offset;
    }
    return !m->codegen->had_error && !re0_buffer_failed(&m->text);
}
