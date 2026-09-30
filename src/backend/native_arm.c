#include "backend/native_internal.h"
#include <stdlib.h>
#include <string.h>

/* ARM A32 (AAPCS, EABI) encoder for the integer subset. Every IR value is one
 * 8-byte slot on the value stack; the accumulator is R0:R1 (low:high) and the
 * right operand R2:R3. R4-R12 are scratch (saved by the prologue). SP is kept
 * 8-byte aligned. Division uses a software restoring long division, so no
 * ARMv7 integer-divide extension is required; division by zero and
 * INT64_MIN / -1 trap. Floating point is rejected explicitly. */

typedef struct { size_t offset; size_t label; uint32_t base; } Branch;

enum { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP = 13, LR = 14 };
/* Callee-saved R4-R11 plus IP(12) for alignment and LR: 10 registers (40 bytes). */
enum { SAVED_REGS = 0x5FF0 };

static void inst(Re0Buffer *b, uint32_t word) { n_put(b, word, 4); }

static void mov_imm(Re0Buffer *b, unsigned rd, uint32_t value) {
    inst(b, 0xE3000000u | (((value >> 12) & 0xF) << 16) | (rd << 12) | (value & 0xFFF));
    if (value >> 16)
        inst(b, 0xE3400000u | (((value >> 28) & 0xF) << 16) | (rd << 12) | ((value >> 16) & 0xFFF));
}
static void mov_reg(Re0Buffer *b, unsigned rd, unsigned rm) { inst(b, 0xE1A00000u | (rd << 12) | rm); }
static void dp_reg(Re0Buffer *b, uint32_t op, unsigned rn, unsigned rd, unsigned rm, bool s) {
    inst(b, op | (rn << 16) | (rd << 12) | rm | (s ? (1u << 20) : 0));
}
static void dp_imm(Re0Buffer *b, uint32_t op, unsigned rn, unsigned rd, unsigned imm) {
    inst(b, op | (rn << 16) | (rd << 12) | (imm & 0xFF));
}
static void cmp_reg(Re0Buffer *b, unsigned rn, unsigned rm) { inst(b, 0xE1500000u | (rn << 16) | rm); }
static void ldr_sp(Re0Buffer *b, unsigned rd, unsigned off) { inst(b, 0xE59D0000u | (rd << 12) | (off & 0xFFF)); }
static void str_sp(Re0Buffer *b, unsigned rd, unsigned off) { inst(b, 0xE58D0000u | (rd << 12) | (off & 0xFFF)); }
static void ldr_fp(Re0Buffer *b, unsigned rd, unsigned mag) {
    if (mag <= 4095) { inst(b, 0xE51B0000u | (rd << 12) | mag); return; }
    mov_imm(b, R12, mag); dp_reg(b, 0xE0400000u, R11, R12, R12, false); inst(b, 0xE59C0000u | (rd << 12));
}
static void str_fp(Re0Buffer *b, unsigned rd, unsigned mag) {
    if (mag <= 4095) { inst(b, 0xE50B0000u | (rd << 12) | mag); return; }
    mov_imm(b, R12, mag); dp_reg(b, 0xE0400000u, R11, R12, R12, false); inst(b, 0xE58C0000u | (rd << 12));
}
static void push_value(Re0Buffer *b) {
    inst(b, 0xE24DD008u); str_sp(b, R0, 0); str_sp(b, R1, 4);
}
static void pop_value(Re0Buffer *b) { ldr_sp(b, R0, 0); ldr_sp(b, R1, 4); inst(b, 0xE28DD008u); }
static size_t branch_placeholder(Re0Buffer *b, uint32_t base) {
    size_t offset = b->len; inst(b, base); return offset;
}
static void patch_branch(Re0Buffer *b, size_t offset, size_t target, uint32_t base) {
    int64_t delta = (int64_t)target - (int64_t)(offset + 8);
    n_patch(b, offset, base | ((uint32_t)(delta >> 2) & 0xFFFFFFu), 4);
}

static void normalize(Re0Buffer *b, const Re0NativeTarget *t, NType type) {
    unsigned bits = n_type_bits(t, type);
    bool sign = n_type_signed(type);
    if (type == N_BOOL) {
        dp_imm(b, 0xE3500000u, R0, 0, 0);      /* cmp r0, #0 */
        mov_imm(b, R0, 0);
        inst(b, 0x13A00001u);                  /* movne r0, #1 */
        mov_imm(b, R1, 0);
        return;
    }
    if (bits == 8) inst(b, (sign ? 0xE6AF0070u : 0xE6EF0070u) | R0);   /* sxtb/uxtb r0,r0 */
    else if (bits == 16) inst(b, (sign ? 0xE6BF0070u : 0xE6FF0070u) | R0);
    if (bits < 64) {
        if (sign) inst(b, 0xE1A01FC0u);        /* mov r1, r0, ASR #31 */
        else mov_imm(b, R1, 0);
    }
}
static void fail(NModule *m, NFunction *f, const char *msg) { n_error(m, f->ast->span, msg); }

/* AAPCS core-register assignment: r0-r3 hold the first arguments, 64-bit
 * values take an even/odd pair, and the rest spill to the stack in 4-byte
 * words. Returns the number of stack words. */
typedef struct { int reg_lo, reg_hi, stack_off; bool on_stack, is64; } ArgLoc;
static void arg_layout(const Re0NativeTarget *t, NFunction *fn, ArgLoc *locs, int *stack_words) {
    int gp = 0, st = 0;
    for (int i = 0; i < fn->param_count; i++) {
        bool is64 = n_type_bits(t, fn->params[i]) > 32;
        int size = is64 ? 2 : 1;
        int base = gp;
        if (is64 && (base & 1)) base++;          /* round up to an even register */
        if (base + size <= 4) {
            locs[i].on_stack = false; locs[i].is64 = is64;
            locs[i].reg_lo = base; locs[i].reg_hi = is64 ? base + 1 : -1;
            gp = base + size;
        } else {
            locs[i].on_stack = true; locs[i].is64 = is64;
            locs[i].reg_lo = locs[i].reg_hi = -1;
            if (is64 && (st & 1)) st++;         /* 8-byte align double-word args */
            locs[i].stack_off = st; st += size;
        }
    }
    *stack_words = st;
}
static void neg64(Re0Buffer *b, unsigned lo, unsigned hi) {
    inst(b, 0xE2700000u | (lo << 16) | (lo << 12)); /* rsbs lo, lo, #0 */
    inst(b, 0xE2E00000u | (hi << 16) | (hi << 12)); /* rsc  hi, hi, #0 */
}

/* Unsigned 64-bit restoring division. Dividend R0:R1, divisor R2:R3; quotient
 * R0:R1, remainder R6:R7. Scratch R4:R5, R8:R9, R10; R12 preserved for signs. */
static void udiv64(Re0Buffer *b) {
    mov_reg(b, R4, R0); mov_reg(b, R5, R1);
    mov_imm(b, R8, 0); mov_imm(b, R9, 0); mov_imm(b, R6, 0); mov_imm(b, R7, 0);
    mov_imm(b, R10, 64);
    size_t loop = b->len;
    inst(b, 0xE1B04084u);                      /* lsls r4, r4, #1 */
    inst(b, 0xE0B55005u);                      /* adcs r5, r5, r5 */
    inst(b, 0xE0B66006u);                      /* adcs r6, r6, r6 */
    inst(b, 0xE0B77007u);                      /* adcs r7, r7, r7 */
    inst(b, 0xE1B08088u);                      /* lsls r8, r8, #1 */
    inst(b, 0xE0B99009u);                      /* adcs r9, r9, r9 */
    cmp_reg(b, R7, R3);
    size_t bhi = branch_placeholder(b, 0x8A000000u);
    size_t blo1 = branch_placeholder(b, 0x3A000000u);
    cmp_reg(b, R6, R2);
    size_t blo2 = branch_placeholder(b, 0x3A000000u);
    size_t subtract = b->len;
    inst(b, 0xE0566002u);                      /* subs r6, r6, r2 */
    inst(b, 0xE0C77003u);                      /* sbc r7, r7, r3 */
    dp_imm(b, 0xE3880000u, R8, R8, 1);         /* orr r8, r8, #1 */
    size_t next = b->len;
    inst(b, 0xE25AA001u);                      /* subs r10, r10, #1 */
    size_t back = branch_placeholder(b, 0x1A000000u);
    patch_branch(b, back, loop, 0x1A000000u);
    patch_branch(b, bhi, subtract, 0x8A000000u);
    patch_branch(b, blo1, next, 0x3A000000u);
    patch_branch(b, blo2, next, 0x3A000000u);
    mov_reg(b, R0, R8); mov_reg(b, R1, R9);
}

/* Signed/unsigned 64-bit DIV/MOD of the two operand slots at [sp] and [sp+8].
 * R12 is free throughout udiv64 and used to carry the operand signs. */
static void divide64(NModule *m, NFunction *f, NInst *in, bool sign) {
    Re0Buffer *b = &m->text;
    ldr_sp(b, R0, 8); ldr_sp(b, R1, 12); ldr_sp(b, R2, 0); ldr_sp(b, R3, 4);
    if (sign) {
        /* r12 = (dividend_sign) | (divisor_sign << 1) */
        inst(b, 0xE1A0CFA1u);                              /* mov r12, r1, LSR #31 */
        inst(b, 0xE1A04FA3u);                              /* mov r4, r3, LSR #31 */
        inst(b, 0xE18CC084u);                              /* orr r12, r12, r4, LSL #1 */
        /* INT64_MIN / -1 traps. */
        mov_imm(b, R4, UINT32_C(0x80000000));
        cmp_reg(b, R1, R4);
        size_t o1 = branch_placeholder(b, 0x1A000000u);    /* bne ok */
        dp_imm(b, 0xE3500000u, R0, 0, 0);                  /* cmp r0, #0 */
        size_t o2 = branch_placeholder(b, 0x1A000000u);
        mov_imm(b, R4, UINT32_C(0xFFFFFFFF));
        cmp_reg(b, R3, R4);
        size_t o3 = branch_placeholder(b, 0x1A000000u);
        cmp_reg(b, R2, R4);
        size_t o4 = branch_placeholder(b, 0x1A000000u);
        inst(b, 0xE7F000F0u);                              /* udf */
        size_t ok = b->len;
        patch_branch(b, o1, ok, 0x1A000000u);
        patch_branch(b, o2, ok, 0x1A000000u);
        patch_branch(b, o3, ok, 0x1A000000u);
        patch_branch(b, o4, ok, 0x1A000000u);
        /* Negate negative operands into magnitudes. */
        dp_imm(b, 0xE3500000u, R1, 0, 0);                  /* cmp r1, #0 */
        size_t dpos = branch_placeholder(b, 0x5A000000u);  /* bpl (non-negative) */
        neg64(b, R0, R1);
        patch_branch(b, dpos, b->len, 0x5A000000u);
        dp_imm(b, 0xE3500000u, R3, 0, 0);                  /* cmp r3, #0 */
        size_t vpos = branch_placeholder(b, 0x5A000000u);
        neg64(b, R2, R3);
        patch_branch(b, vpos, b->len, 0x5A000000u);
    }
    /* Divide-by-zero trap. */
    dp_reg(b, 0xE1800000u, R2, R4, R3, false);             /* orr r4, r2, r3 */
    dp_imm(b, 0xE3500000u, R4, 0, 0);                      /* cmp r4, #0 */
    size_t nz = branch_placeholder(b, 0x1A000000u);
    inst(b, 0xE7F000F0u);
    patch_branch(b, nz, b->len, 0x1A000000u);
    udiv64(b);
    if (sign) {
        /* quotient sign = bit0 ^ bit1 of r12 */
        inst(b, 0xE02C20ACu);                              /* eor r2, r12, r12, LSR #1 */
        dp_imm(b, 0xE3120000u, R2, 0, 1);                  /* tst r2, #1 */
        size_t qpos = branch_placeholder(b, 0x0A000000u);
        neg64(b, R0, R1);
        patch_branch(b, qpos, b->len, 0x0A000000u);
        dp_imm(b, 0xE31C0000u, R12, 0, 1);                 /* tst r12, #1 */
        size_t rpos = branch_placeholder(b, 0x0A000000u);
        neg64(b, R6, R7);
        patch_branch(b, rpos, b->len, 0x0A000000u);
    }
    if (in->arg == BINOP_MOD) { mov_reg(b, R0, R6); mov_reg(b, R1, R7); }
    inst(b, 0xE28DD010u);                                  /* add sp, sp, #16 */
    push_value(b);
    (void)m; (void)f;
}

static void binary_integer(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    unsigned bits = n_type_bits(m->target, in->type);
    bool sign = n_type_signed(in->type);
    Re0BinOpKind op = (Re0BinOpKind)in->arg;
    ldr_sp(b, R0, 8); ldr_sp(b, R1, 12); ldr_sp(b, R2, 0); ldr_sp(b, R3, 4);
    if (op >= BINOP_EQ && op <= BINOP_GE) {
        if (bits <= 32) cmp_reg(b, R0, R2);
        else {
            cmp_reg(b, R1, R3);
            size_t eq = branch_placeholder(b, 0x0A000000u);
            size_t done = branch_placeholder(b, 0xEA000000u);
            patch_branch(b, eq, b->len, 0x0A000000u);
            cmp_reg(b, R0, R2);
            patch_branch(b, done, b->len, 0xEA000000u);
        }
        unsigned cond = 0;
        switch (op) {
            case BINOP_EQ: cond = 0; break;
            case BINOP_NE: cond = 1; break;
            case BINOP_LT: cond = sign ? 11 : 3; break;
            case BINOP_LE: cond = sign ? 13 : 9; break;
            case BINOP_GT: cond = sign ? 12 : 8; break;
            case BINOP_GE: cond = sign ? 10 : 2; break;
            default: break;
        }
        mov_imm(b, R0, 0);
        inst(b, (cond << 28) | 0x03A00001u);               /* mov<cond> r0, #1 */
        mov_imm(b, R1, 0);
    } else if (op == BINOP_DIV || op == BINOP_MOD) {
        divide64(m, f, in, sign);
        return;
    } else if (bits <= 32) {
        switch (op) {
            case BINOP_ADD: inst(b, 0xE0800002u); break;
            case BINOP_SUB: inst(b, 0xE0400002u); break;
            case BINOP_MUL: inst(b, 0xE0000092u); break;
            case BINOP_BAND: dp_reg(b, 0xE0000000u, R0, R0, R2, false); break;
            case BINOP_BOR: dp_reg(b, 0xE1800000u, R0, R0, R2, false); break;
            case BINOP_BXOR: dp_reg(b, 0xE0200000u, R0, R0, R2, false); break;
            case BINOP_SHL: case BINOP_SHR:
                dp_imm(b, 0xE2000000u, R2, R2, bits - 1);  /* and r2, r2, #mask */
                if (op == BINOP_SHL) inst(b, 0xE1A00210u | R0);
                else inst(b, (sign ? 0xE1A00250u : 0xE1A00230u) | R0);
                break;
            default: fail(m, f, "invalid arm binary operation"); return;
        }
        normalize(b, m->target, in->type);
    } else {
        switch (op) {
            case BINOP_ADD: inst(b, 0xE0900002u); inst(b, 0xE0A11003u); break; /* adds; adc */
            case BINOP_SUB: inst(b, 0xE0500002u); inst(b, 0xE0C11003u); break; /* subs; sbc */
            case BINOP_MUL:
                inst(b, 0xE0854092u);              /* umull r4, r5, r0, r2 */
                inst(b, 0xE0255093u);              /* mla r5, r0, r3, r5 */
                inst(b, 0xE0255192u);              /* mla r5, r1, r2, r5 */
                mov_reg(b, R0, R4); mov_reg(b, R1, R5);
                break;
            case BINOP_BAND: dp_reg(b, 0xE0000000u, R0, R0, R2, false); dp_reg(b, 0xE0000000u, R1, R1, R3, false); break;
            case BINOP_BOR: dp_reg(b, 0xE1800000u, R0, R0, R2, false); dp_reg(b, 0xE1800000u, R1, R1, R3, false); break;
            case BINOP_BXOR: dp_reg(b, 0xE0200000u, R0, R0, R2, false); dp_reg(b, 0xE0200000u, R1, R1, R3, false); break;
            case BINOP_SHL: case BINOP_SHR: {
                dp_imm(b, 0xE2100000u, R2, R2, 0x3F);      /* ands r2, r2, #63 */
                size_t zero = branch_placeholder(b, 0x0A000000u);
                size_t loop = b->len;
                if (op == BINOP_SHL) { inst(b, 0xE1B00080u); inst(b, 0xE0B11001u); } /* lsls r0,#1; adcs r1,r1,r1 */
                else if (sign) { inst(b, 0xE1B010C1u); inst(b, 0xE1A00060u); }       /* asrs r1,#1; rrx r0 */
                else { inst(b, 0xE1B010A1u); inst(b, 0xE1A00060u); }                 /* lsrs r1,#1; rrx r0 */
                inst(b, 0xE2522001u);                      /* subs r2, r2, #1 */
                size_t back = branch_placeholder(b, 0x1A000000u);
                patch_branch(b, back, loop, 0x1A000000u);
                patch_branch(b, zero, b->len, 0x0A000000u);
                break;
            }
            default: fail(m, f, "invalid arm 64-bit binary operation"); return;
        }
    }
    inst(b, 0xE28DD010u);                                  /* add sp, sp, #16 */
    push_value(b);
}

static void unary(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    pop_value(b);
    switch ((Re0UnOpKind)in->arg) {
        case UNOP_NEG: neg64(b, R0, R1); break;
        case UNOP_BNOT: dp_reg(b, 0xE1E00000u, 0, R0, R0, false); dp_reg(b, 0xE1E00000u, 0, R1, R1, false); break;
        case UNOP_NOT: dp_imm(b, 0xE3500000u, R0, 0, 0); mov_imm(b, R0, 0); inst(b, 0x03A00001u); mov_imm(b, R1, 0); break;
        default: fail(m, f, "invalid arm unary operation"); return;
    }
    push_value(b);
}

static void convert(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    NType from = (NType)in->arg, to = in->type;
    if (n_type_float(from) || n_type_float(to)) { fail(m, f, "floating point is not supported on arm yet"); return; }
    pop_value(b);
    normalize(b, m->target, from);
    normalize(b, m->target, to);
    push_value(b);
}

static void function(NModule *m, NFunction *f) {
    Re0Buffer *b = &m->text;
    size_t *labels = calloc(f->labels + 1, sizeof(*labels));
    Branch *branches = calloc(f->count + 1, sizeof(*branches));
    if (!labels || !branches) {
        free(labels); free(branches); n_error(m, f->ast->span, "cannot allocate branch fixups"); return;
    }
    f->offset = b->len;
    inst(b, 0xE92D0000u | SAVED_REGS);         /* push {r4-r12, lr} */
    mov_reg(b, R11, SP);                       /* r11 = fp */
    size_t frame = (f->slots * N_WORD + 7) & ~(size_t)7;
    if (frame) inst(b, 0xE24DD000u | (frame & 0xFFF)); /* sub sp, sp, #frame */
    ArgLoc locs[N_MAX_PARAMS]; int stack_words = 0;
    arg_layout(m->target, f, locs, &stack_words);
    for (int i = 0; i < f->param_count; i++) {
        if (n_type_float(f->params[i])) { fail(m, f, "floating point parameters are not supported on arm yet"); break; }
        if (locs[i].on_stack) {
            unsigned off = (unsigned)(40 + 4 * locs[i].stack_off); /* entry sp = r11 + 40 */
            inst(b, 0xE59B0000u | (R0 << 12) | off);           /* ldr r0, [r11, #off] */
            if (locs[i].is64) inst(b, 0xE59B1000u | (R1 << 12) | (off + 4));
        } else {
            mov_reg(b, R0, (unsigned)locs[i].reg_lo);
            if (locs[i].is64) mov_reg(b, R1, (unsigned)locs[i].reg_hi);
        }
        normalize(b, m->target, f->params[i]);
        str_fp(b, R0, (unsigned)(8 * (i + 1)));
        str_fp(b, R1, (unsigned)(8 * (i + 1) - 4));
    }
    (void)stack_words;
    size_t branch_count = 0;
    for (size_t i = 0; i < f->count && !m->codegen->had_error; i++) {
        NInst *in = &f->ir[i];
        if (in->op == N_LABEL) labels[in->arg] = b->len;
        if (in->depth < 0) continue;
        switch (in->op) {
            case N_CONST:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on arm yet"); break; }
                mov_imm(b, R0, (uint32_t)in->value);
                mov_imm(b, R1, (uint32_t)(in->value >> 32));
                normalize(b, m->target, in->type);
                push_value(b);
                break;
            case N_LOAD:
                ldr_fp(b, R0, (unsigned)(8 * ((int)in->arg + 1)));
                ldr_fp(b, R1, (unsigned)(8 * ((int)in->arg + 1) - 4));
                push_value(b); break;
            case N_STORE:
                pop_value(b); normalize(b, m->target, in->type);
                str_fp(b, R0, (unsigned)(8 * ((int)in->arg + 1)));
                str_fp(b, R1, (unsigned)(8 * ((int)in->arg + 1) - 4)); break;
            case N_DROP: pop_value(b); break;
            case N_BINARY:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on arm yet"); break; }
                binary_integer(m, f, in); break;
            case N_UNARY: unary(m, f, in); break;
            case N_CONVERT: convert(m, f, in); break;
            case N_CALL: {
                NFunction *target = &m->functions[in->arg];
                if (n_type_float(target->result)) { fail(m, f, "floating point is not supported on arm yet"); break; }
                ArgLoc clocs[N_MAX_PARAMS]; int cstack = 0;
                arg_layout(m->target, target, clocs, &cstack);
                for (int j = 0; j < target->param_count; j++) {
                    if (n_type_float(target->params[j])) { fail(m, f, "floating point parameters are not supported on arm yet"); break; }
                    if (clocs[j].on_stack) continue;
                    unsigned off = (unsigned)(8 * (target->param_count - 1 - j));
                    ldr_sp(b, (unsigned)clocs[j].reg_lo, off);
                    if (clocs[j].is64) ldr_sp(b, (unsigned)clocs[j].reg_hi, off + 4);
                }
                size_t pushed = (cstack & 1) ? 4 : 0;
                if (cstack & 1) inst(b, 0xE24DD004u);      /* sub sp, sp, #4 (8-byte align) */
                for (int j = target->param_count - 1; j >= 0; j--) {
                    if (!clocs[j].on_stack) continue;
                    unsigned off = (unsigned)(pushed + 8 * (target->param_count - 1 - j));
                    ldr_sp(b, R4, off);
                    if (clocs[j].is64) {
                        ldr_sp(b, R5, off + 4);
                        inst(b, 0xE24DD008u); str_sp(b, R4, 0); str_sp(b, R5, 4);
                        pushed += 8;
                    } else {
                        inst(b, 0xE24DD004u); str_sp(b, R4, 0);
                        pushed += 4;
                    }
                }
                size_t at = b->len;
                inst(b, 0xEB000000u);          /* bl (imm patched by relocation) */
                n_reloc_add(m, at, in->arg);
                if (pushed) inst(b, 0xE28DD000u | ((uint32_t)pushed & 0xFFF)); /* add sp, sp, #pushed */
                size_t drop = 8 * (size_t)target->param_count;
                if (drop) inst(b, 0xE28DD000u | ((uint32_t)drop & 0xFFF));     /* add sp, sp, #drop */
                if (target->result == N_UNIT) { mov_imm(b, R0, 0); mov_imm(b, R1, 0); }
                else normalize(b, m->target, target->result);
                push_value(b);
                break;
            }
            case N_LABEL: break;
            case N_JUMP: case N_JZ:
                if (in->op == N_JZ) {
                    pop_value(b);
                    dp_imm(b, 0xE3500000u, R0, 0, 0);
                    branches[branch_count].offset = branch_placeholder(b, 0x0A000000u);
                    branches[branch_count].base = 0x0A000000u;
                } else {
                    branches[branch_count].offset = branch_placeholder(b, 0xEA000000u);
                    branches[branch_count].base = 0xEA000000u;
                }
                branches[branch_count].label = in->arg; branch_count++;
                break;
            case N_RETURN:
                pop_value(b);
                if (n_type_bits(m->target, in->type) <= 32) normalize(b, m->target, in->type);
                inst(b, 0xE1A0D00Bu);              /* mov sp, r11 */
                inst(b, 0xE8BD0000u | SAVED_REGS);
                inst(b, 0xE12FFF1Eu);              /* bx lr */
                break;
            case N_END:
                mov_imm(b, R0, 0); mov_imm(b, R1, 0);
                inst(b, 0xE1A0D00Bu); inst(b, 0xE8BD0000u | SAVED_REGS); inst(b, 0xE12FFF1Eu);
                break;
            case N_ASSERT: {
                pop_value(b);
                dp_imm(b, 0xE3500000u, R0, 0, 0);
                size_t ok = branch_placeholder(b, 0x1A000000u);
                inst(b, 0xE7F000F0u);
                patch_branch(b, ok, b->len, 0x1A000000u);
                break;
            }
        }
        if (b->len > N_MAX_TEXT) n_error(m, f->ast->span, "native text size limit exceeded");
        if (re0_buffer_failed(b)) n_error(m, f->ast->span, "cannot allocate machine code");
    }
    for (size_t i = 0; i < branch_count; i++) {
        Branch *fix = &branches[i];
        patch_branch(b, fix->offset, labels[fix->label], fix->base);
    }
    f->size = b->len - f->offset;
    free(labels); free(branches);
}

bool n_encode_arm(NModule *m) {
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
        mov_imm(&m->text, R0, 0);
        mov_imm(&m->text, R11, 0);
        size_t at = m->text.len;
        inst(&m->text, 0xEB000000u);           /* bl main */
        n_reloc_add(m, at, main_index);
        mov_imm(&m->text, R7, 1);              /* __NR_exit */
        inst(&m->text, 0xEF000000u);           /* svc #0 */
        inst(&m->text, 0xE7F000F0u);           /* udf */
        m->entry_size = m->text.len - m->entry_offset;
    }
    return !m->codegen->had_error && !re0_buffer_failed(&m->text);
}
