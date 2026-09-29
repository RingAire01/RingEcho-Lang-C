#include "backend/native_internal.h"
#include <stdlib.h>
#include <string.h>

/* i386 (IA-32) System V encoder for the integer subset. Every IR value is one
 * 8-byte slot on the machine stack. 64-bit integers live in the EDX:EAX pair.
 * EBX/ESI/EDI are callee-saved and preserved by the prologue, so they are used
 * as scratch. Floating point is not implemented for this target yet; float
 * types are rejected explicitly rather than miscompiled.
 *
 * ABI: arguments are pushed right-to-left as 4-byte units (8 bytes for 64-bit
 * integers); ESP is 16-byte aligned at the call. Integers up to 32 bits return
 * in EAX, 64-bit integers in EDX:EAX. */

typedef struct { size_t offset, label; } Branch;

enum { EAX = 0, ECX = 1, EDX = 2, EBX = 3, ESP = 4, EBP = 5, ESI = 6, EDI = 7 };

static void bytes(Re0Buffer *b, const unsigned char *p, size_t n) {
    re0_buffer_write_n(b, (const char *)p, n);
}
#define CODE(b, ...) do { const unsigned char c_[] = {__VA_ARGS__}; bytes((b), c_, sizeof(c_)); } while (0)

static void modrm_ebp(Re0Buffer *b, unsigned reg, int32_t disp) {
    CODE(b, (unsigned char)(0x80 | ((reg & 7) << 3) | 5));
    n_put(b, (uint32_t)disp, 4);
}
static void modrm_esp(Re0Buffer *b, unsigned reg, int32_t disp) {
    CODE(b, (unsigned char)(0x80 | ((reg & 7) << 3) | 4), 0x24);
    n_put(b, (uint32_t)disp, 4);
}
static void mov_reg_ebp(Re0Buffer *b, unsigned reg, int32_t disp) { CODE(b, 0x8b); modrm_ebp(b, reg, disp); }
static void mov_ebp_reg(Re0Buffer *b, unsigned reg, int32_t disp) { CODE(b, 0x89); modrm_ebp(b, reg, disp); }
static void mov_reg_esp(Re0Buffer *b, unsigned reg, int32_t disp) { CODE(b, 0x8b); modrm_esp(b, reg, disp); }
static void mov_esp_reg(Re0Buffer *b, unsigned reg, int32_t disp) { CODE(b, 0x89); modrm_esp(b, reg, disp); }
static void mov_reg_imm(Re0Buffer *b, unsigned reg, uint32_t value) { CODE(b, (unsigned char)(0xb8 + reg)); n_put(b, value, 4); }
static void push_reg(Re0Buffer *b, unsigned reg) { CODE(b, (unsigned char)(0x50 + reg)); }
static void push64(Re0Buffer *b, unsigned lo, unsigned hi) { push_reg(b, hi); push_reg(b, lo); }
static void push_imm(Re0Buffer *b, uint32_t value) { CODE(b, 0x68); n_put(b, value, 4); }
static void lea_ebp(Re0Buffer *b, unsigned reg, int32_t disp) { CODE(b, 0x8d); modrm_ebp(b, reg, disp); }
static size_t jcc(Re0Buffer *b, unsigned char cc) {
    CODE(b, 0x0f, cc); size_t at = b->len; n_put(b, 0, 4); return at;
}
static size_t jmp_near(Re0Buffer *b) {
    CODE(b, 0xe9); size_t at = b->len; n_put(b, 0, 4); return at;
}
static void patch_to(Re0Buffer *b, size_t at, size_t target) {
    n_patch(b, at, (uint32_t)((int64_t)target - (int64_t)(at + 4)), 4);
}
static void bind(Re0Buffer *b, size_t at) { patch_to(b, at, b->len); }
static void replace_top(Re0Buffer *b) { mov_esp_reg(b, EAX, 0); mov_esp_reg(b, EDX, 4); }

static void normalize(Re0Buffer *b, const Re0NativeTarget *t, NType type) {
    unsigned bits = n_type_bits(t, type);
    bool sign = n_type_signed(type);
    if (type == N_BOOL) {
        CODE(b, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x31, 0xd2);
        return;
    }
    if (bits == 8 && sign) CODE(b, 0x0f, 0xbe, 0xc0);
    else if (bits == 8) CODE(b, 0x0f, 0xb6, 0xc0);
    else if (bits == 16 && sign) CODE(b, 0x0f, 0xbf, 0xc0);
    else if (bits == 16) CODE(b, 0x0f, 0xb7, 0xc0);
    if (bits < 64) {
        if (sign) CODE(b, 0x99);      /* cdq */
        else CODE(b, 0x31, 0xd2);     /* edx = 0 */
    }
}

static void fail(NModule *m, NFunction *f, const char *msg) { n_error(m, f->ast->span, msg); }

/* Unsigned restoring long division. On entry EDX:EAX is the dividend and
 * EBX:ECX the divisor; on exit quotient is in [esp] / [esp+4] and remainder in
 * ESI:EDI. 32 bytes of scratch are allocated below ESP and released on exit. */
/* Unsigned restoring long division. On entry EDX:EAX is the dividend and
 * EBX:ECX the divisor; on exit the quotient is in EDX:EAX and the remainder in
 * ESI:EDI. Scratch is allocated below ESP and released before returning. */
static void udiv64(Re0Buffer *b) {
    CODE(b, 0x83, 0xec, 0x20);                      /* sub esp, 32 */
    CODE(b, 0x89, 0xde, 0x09, 0xce);                /* mov esi,ebx; or esi,ecx */
    size_t zero = jcc(b, 0x84);                     /* jz zero */
    CODE(b, 0xc7, 0x04, 0x24, 0, 0, 0, 0);          /* mov dword [esp], 0 */
    CODE(b, 0xc7, 0x44, 0x24, 0x04, 0, 0, 0, 0);    /* mov dword [esp+4], 0 */
    CODE(b, 0xc7, 0x44, 0x24, 0x08, 64, 0, 0, 0);   /* mov dword [esp+8], 64 */
    CODE(b, 0x31, 0xf6, 0x31, 0xff);                /* xor esi,esi; xor edi,edi */
    size_t loop = b->len;
    CODE(b, 0xd1, 0x24, 0x24);                      /* shl dword [esp], 1 */
    CODE(b, 0xd1, 0x54, 0x24, 0x04);                /* rcl dword [esp+4], 1 */
    CODE(b, 0xd1, 0xe0, 0xd1, 0xd2);                /* shl eax,1; rcl edx,1 */
    CODE(b, 0xd1, 0xd7, 0xd1, 0xd6);                /* rcl edi,1; rcl esi,1 */
    CODE(b, 0x39, 0xde);                            /* cmp esi, ebx */
    size_t ja = jcc(b, 0x87);                       /* ja subtract */
    size_t jb1 = jcc(b, 0x82);                      /* jb next */
    CODE(b, 0x39, 0xcf);                            /* cmp edi, ecx */
    size_t jb2 = jcc(b, 0x82);                      /* jb next */
    size_t subtract = b->len;
    CODE(b, 0x29, 0xcf, 0x19, 0xde);                /* sub edi,ecx; sbb esi,ebx */
    CODE(b, 0x83, 0x0c, 0x24, 0x01);                /* or dword [esp], 1 */
    size_t next = b->len;
    CODE(b, 0xff, 0x4c, 0x24, 0x08);                /* dec dword [esp+8] */
    size_t back = jcc(b, 0x85);                     /* jnz loop */
    patch_to(b, back, loop);
    patch_to(b, ja, subtract);
    patch_to(b, jb1, next);
    patch_to(b, jb2, next);
    mov_reg_esp(b, EAX, 0);                         /* quotient -> edx:eax */
    mov_reg_esp(b, EDX, 4);
    CODE(b, 0x83, 0xc4, 0x20);                      /* add esp, 32 */
    size_t done = jmp_near(b);                      /* jmp done */
    bind(b, zero);
    CODE(b, 0x0f, 0x0b);                            /* ud2: division by zero */
    bind(b, done);
}

/* 64-bit DIV/MOD for the two operand slots at [esp] and [esp+8]. Signed forms
 * convert to magnitudes first, then restore the signs. */
static void divide64(NModule *m, NFunction *f, NInst *in, bool sign) {
    Re0Buffer *b = &m->text;
    mov_reg_esp(b, EAX, 8); mov_reg_esp(b, EDX, 12);/* dividend -> edx:eax */
    mov_reg_esp(b, ECX, 0); mov_reg_esp(b, EBX, 4); /* divisor -> ebx:ecx */
    if (sign) {
        /* INT64_MIN / -1 overflows; trap like the x86-64 backend. */
        CODE(b, 0x81, 0xfa, 0x00, 0x00, 0x00, 0x80); /* cmp edx, 0x80000000 */
        size_t o1 = jcc(b, 0x85), o2, o3, o4;
        CODE(b, 0x85, 0xc0);                        /* test eax, eax */
        o2 = jcc(b, 0x85);
        CODE(b, 0x83, 0xfb, 0xff);                  /* cmp ebx, -1 */
        o3 = jcc(b, 0x85);
        CODE(b, 0x83, 0xf9, 0xff);                  /* cmp ecx, -1 */
        o4 = jcc(b, 0x85);
        CODE(b, 0x0f, 0x0b);                        /* ud2 */
        bind(b, o1); bind(b, o2); bind(b, o3); bind(b, o4);
        CODE(b, 0x83, 0xec, 0x10);                  /* sub esp, 16: [esp]=su, [esp+4]=sv */
        CODE(b, 0x89, 0xd6, 0xc1, 0xfe, 0x1f);      /* mov esi,edx; sar esi,31 */
        mov_esp_reg(b, ESI, 0);
        CODE(b, 0x89, 0xdf, 0xc1, 0xff, 0x1f);      /* mov edi,ebx; sar edi,31 */
        mov_esp_reg(b, EDI, 4);
        CODE(b, 0x85, 0xd2);                        /* test edx, edx */
        size_t upos = jcc(b, 0x89);                 /* jns */
        CODE(b, 0xf7, 0xd0, 0xf7, 0xd2, 0x83, 0xc0, 0x01, 0x83, 0xd2, 0x00);
        bind(b, upos);
        CODE(b, 0x85, 0xdb);                        /* test ebx, ebx */
        size_t vpos = jcc(b, 0x89);
        CODE(b, 0xf7, 0xd1, 0xf7, 0xd3, 0x83, 0xc1, 0x01, 0x83, 0xd3, 0x00);
        bind(b, vpos);
    }
    CODE(b, 0x85, 0xdb, 0x75, 0x06, 0x85, 0xc9, 0x75, 0x02, 0x0f, 0x0b); /* divisor == 0 -> ud2 */
    udiv64(b);
    if (sign) {
        mov_reg_esp(b, ECX, 0); mov_reg_esp(b, EBX, 4);
        CODE(b, 0x31, 0xd9);                        /* xor ecx,ebx: quotient sign */
        size_t qok = jcc(b, 0x84);
        CODE(b, 0xf7, 0xd0, 0xf7, 0xd2, 0x83, 0xc0, 0x01, 0x83, 0xd2, 0x00);
        bind(b, qok);
        mov_reg_esp(b, ECX, 0);
        CODE(b, 0x85, 0xc9);                        /* remainder sign = dividend sign */
        size_t rok = jcc(b, 0x84);
        CODE(b, 0xf7, 0xd7, 0xf7, 0xd6, 0x83, 0xc7, 0x01, 0x83, 0xd6, 0x00);
        bind(b, rok);
        CODE(b, 0x83, 0xc4, 0x10);                  /* add esp, 16 (sign scratch) */
    }
    if (in->arg == BINOP_MOD) CODE(b, 0x89, 0xf8, 0x89, 0xf2); /* mov eax,edi; mov edx,esi */
    CODE(b, 0x83, 0xc4, 0x10);                      /* drop the two operand slots */
    push64(b, EAX, EDX);
    (void)f;
}

static void binary_integer(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    unsigned bits = n_type_bits(m->target, in->type);
    bool sign = n_type_signed(in->type);
    Re0BinOpKind op = (Re0BinOpKind)in->arg;
    bool comparison = op >= BINOP_EQ && op <= BINOP_GE;
    if (comparison) {
        if (bits <= 32) {
            mov_reg_esp(b, EAX, 8); mov_reg_esp(b, ECX, 0);
            CODE(b, 0x39, 0xc8);
        } else {
            mov_reg_esp(b, EAX, 8); mov_reg_esp(b, EDX, 12);
            mov_reg_esp(b, ECX, 0); mov_reg_esp(b, EBX, 4);
            CODE(b, 0x39, 0xda, 0x75, 0x02, 0x39, 0xc8);
        }
        unsigned char cc = 0x94;
        switch (op) {
            case BINOP_EQ: cc = 0x94; break;
            case BINOP_NE: cc = 0x95; break;
            case BINOP_LT: cc = sign ? 0x9c : 0x92; break;
            case BINOP_LE: cc = sign ? 0x9e : 0x96; break;
            case BINOP_GT: cc = sign ? 0x9f : 0x97; break;
            case BINOP_GE: cc = sign ? 0x9d : 0x93; break;
            default: break;
        }
        CODE(b, 0x0f, cc, 0xc0, 0x0f, 0xb6, 0xc0, 0x31, 0xd2, 0x83, 0xc4, 0x10);
        push64(b, EAX, EDX);
        return;
    }
    if (bits <= 32) {
        mov_reg_esp(b, EAX, 8); mov_reg_esp(b, ECX, 0);
        switch (op) {
            case BINOP_ADD: CODE(b, 0x01, 0xc8); break;
            case BINOP_SUB: CODE(b, 0x29, 0xc8); break;
            case BINOP_MUL: CODE(b, 0x0f, 0xaf, 0xc1); break;
            case BINOP_DIV: case BINOP_MOD:
                if (sign) {
                    CODE(b, 0x83, 0xf9, 0xff, 0x75, 0x09);
                    CODE(b, 0x3d); n_put(b, 0u - (UINT32_C(1) << (bits - 1)), 4);
                    CODE(b, 0x75, 0x02, 0x0f, 0x0b);
                    CODE(b, 0x99, 0xf7, 0xf9);
                } else {
                    CODE(b, 0x31, 0xd2, 0xf7, 0xf1);
                }
                if (op == BINOP_MOD) CODE(b, 0x89, 0xd0);
                break;
            case BINOP_BAND: CODE(b, 0x21, 0xc8); break;
            case BINOP_BOR: CODE(b, 0x09, 0xc8); break;
            case BINOP_BXOR: CODE(b, 0x31, 0xc8); break;
            case BINOP_SHL: case BINOP_SHR:
                CODE(b, 0x83, 0xe1, (unsigned char)(bits - 1));
                if (op == BINOP_SHL) CODE(b, 0xd3, 0xe0);
                else if (sign) CODE(b, 0xd3, 0xf8);
                else CODE(b, 0xd3, 0xe8);
                break;
            default: fail(m, f, "invalid i386 binary operation"); return;
        }
        normalize(b, m->target, in->type);
        CODE(b, 0x83, 0xc4, 0x10);
        push64(b, EAX, EDX);
        return;
    }
    mov_reg_esp(b, EAX, 8); mov_reg_esp(b, EDX, 12);
    switch (op) {
        case BINOP_ADD:
            CODE(b, 0x03, 0x04, 0x24, 0x13, 0x54, 0x24, 0x04); break;
        case BINOP_SUB:
            CODE(b, 0x2b, 0x04, 0x24, 0x1b, 0x54, 0x24, 0x04); break;
        case BINOP_MUL: {
            mov_reg_esp(b, ECX, 0); mov_reg_esp(b, EBX, 4);
            CODE(b, 0x89, 0xd6, 0x0f, 0xaf, 0xf1);  /* mov esi,edx; imul esi,ecx */
            CODE(b, 0x89, 0xc7, 0x0f, 0xaf, 0xfb);  /* mov edi,eax; imul edi,ebx */
            CODE(b, 0x01, 0xfe, 0xf7, 0xe1, 0x01, 0xf2); /* add esi,edi; mul ecx; add edx,esi */
            break;
        }
        case BINOP_DIV: case BINOP_MOD:
            divide64(m, f, in, sign);
            return;
        case BINOP_BAND:
            CODE(b, 0x23, 0x04, 0x24, 0x23, 0x54, 0x24, 0x04); break;
        case BINOP_BOR:
            CODE(b, 0x0b, 0x04, 0x24, 0x0b, 0x54, 0x24, 0x04); break;
        case BINOP_BXOR:
            CODE(b, 0x33, 0x04, 0x24, 0x33, 0x54, 0x24, 0x04); break;
        case BINOP_SHL: case BINOP_SHR: {
            mov_reg_esp(b, ECX, 0);
            CODE(b, 0x83, 0xe1, 0x3f);
            size_t zero = jcc(b, 0x84);
            size_t loop = b->len;
            if (op == BINOP_SHL) CODE(b, 0xd1, 0xe0, 0xd1, 0xd2);
            else if (sign) CODE(b, 0xd1, 0xfa, 0xd1, 0xd8);
            else CODE(b, 0xd1, 0xea, 0xd1, 0xd8);
            CODE(b, 0xfe, 0xc9);
            size_t back = jcc(b, 0x85);
            patch_to(b, back, loop);
            bind(b, zero);
            break;
        }
        default: fail(m, f, "invalid i386 64-bit binary operation"); return;
    }
    CODE(b, 0x83, 0xc4, 0x10);
    push64(b, EAX, EDX);
}

static void unary(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    mov_reg_esp(b, EAX, 0); mov_reg_esp(b, EDX, 4);
    switch ((Re0UnOpKind)in->arg) {
        case UNOP_NEG: CODE(b, 0xf7, 0xd8, 0x83, 0xd2, 0x00, 0xf7, 0xda); break;
        case UNOP_BNOT: CODE(b, 0xf7, 0xd0, 0xf7, 0xd2); break;
        case UNOP_NOT: CODE(b, 0x85, 0xc0, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x31, 0xd2); break;
        default: fail(m, f, "invalid i386 unary operation"); return;
    }
    normalize(b, m->target, in->type);
    replace_top(b);
}

static void convert(NModule *m, NFunction *f, NInst *in) {
    Re0Buffer *b = &m->text;
    NType from = (NType)in->arg, to = in->type;
    if (n_type_float(from) || n_type_float(to)) { fail(m, f, "floating point is not supported on i386 yet"); return; }
    mov_reg_esp(b, EAX, 0); mov_reg_esp(b, EDX, 4);
    normalize(b, m->target, from);
    if (to == N_BOOL)
        CODE(b, 0x09, 0xd0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x31, 0xd2); /* or eax,edx; setne */
    else if (n_type_bits(m->target, to) <= 32) normalize(b, m->target, to);
    /* to == 64: normalize(from) already produced a canonical 64-bit value. */
    replace_top(b);
}

static void function(NModule *m, NFunction *f) {
    Re0Buffer *b = &m->text;
    size_t *labels = calloc(f->labels + 1, sizeof(*labels));
    Branch *branches = calloc(f->count + 1, sizeof(*branches));
    if (!labels || !branches) {
        free(labels); free(branches); n_error(m, f->ast->span, "cannot allocate branch fixups"); return;
    }
    f->offset = b->len;
    CODE(b, 0x55, 0x89, 0xe5, 0x53, 0x56, 0x57);    /* push ebp; mov ebp,esp; push ebx;esi;edi */
    size_t frame = ((f->slots * N_WORD + 15) & ~(size_t)15) + 12;
    CODE(b, 0x81, 0xec); n_put(b, (uint32_t)frame, 4); /* sub esp, frame */
    int32_t param_off = 8;
    for (int i = 0; i < f->param_count; i++) {
        if (n_type_float(f->params[i])) { fail(m, f, "floating point parameters are not supported on i386 yet"); break; }
        int32_t disp = -(int32_t)(12 + 8 * (i + 1));
        unsigned bits = n_type_bits(m->target, f->params[i]);
        unsigned size = bits > 32 ? 8 : 4;
        if (bits > 32) {
            mov_reg_ebp(b, EAX, param_off); mov_reg_ebp(b, EDX, param_off + 4);
            mov_ebp_reg(b, EAX, disp); mov_ebp_reg(b, EDX, disp + 4);
        } else {
            mov_reg_ebp(b, EAX, param_off);
            normalize(b, m->target, f->params[i]);
            mov_ebp_reg(b, EAX, disp); mov_ebp_reg(b, EDX, disp + 4);
        }
        param_off += (int32_t)size;
    }
    size_t branch_count = 0;
    for (size_t i = 0; i < f->count && !m->codegen->had_error; i++) {
        NInst *in = &f->ir[i];
        if (in->op == N_LABEL) labels[in->arg] = b->len;
        if (in->depth < 0) continue;
        switch (in->op) {
            case N_CONST:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on i386 yet"); break; }
                if (n_type_bits(m->target, in->type) <= 32) {
                    mov_reg_imm(b, EAX, (uint32_t)in->value);
                    normalize(b, m->target, in->type);
                    push64(b, EAX, EDX);
                } else {
                    push_imm(b, (uint32_t)(in->value >> 32));
                    push_imm(b, (uint32_t)in->value);
                }
                break;
            case N_LOAD: { int32_t disp = -(int32_t)(12 + 8 * ((int32_t)in->arg + 1));
                mov_reg_ebp(b, EAX, disp); mov_reg_ebp(b, EDX, disp + 4); push64(b, EAX, EDX); break; }
            case N_STORE: { int32_t disp;
                CODE(b, 0x58, 0x5a);
                normalize(b, m->target, in->type);
                disp = -(int32_t)(12 + 8 * ((int32_t)in->arg + 1));
                mov_ebp_reg(b, EAX, disp); mov_ebp_reg(b, EDX, disp + 4); break; }
            case N_DROP: CODE(b, 0x58, 0x5a); break;
            case N_BINARY:
                if (n_type_float(in->type)) { fail(m, f, "floating point is not supported on i386 yet"); break; }
                binary_integer(m, f, in); break;
            case N_UNARY: unary(m, f, in); break;
            case N_CONVERT: convert(m, f, in); break;
            case N_CALL: {
                NFunction *target = &m->functions[in->arg];
                if (n_type_float(target->result)) { fail(m, f, "floating point is not supported on i386 yet"); break; }
                /* i386 SysV lays arguments sequentially in 4-byte units with no
                 * extra alignment for 64-bit values (unlike x86-64). */
                size_t arg_bytes = 0;
                for (int j = 0; j < target->param_count; j++)
                    arg_bytes += n_type_bits(m->target, target->params[j]) > 32 ? 8 : 4;
                size_t d0 = (size_t)(in->depth - target->param_count);
                size_t pad = ((8 * ((size_t)in->depth & 1)) - arg_bytes) & 15;
                if (pad) CODE(b, 0x83, 0xec, (unsigned char)pad);
                for (int j = target->param_count - 1; j >= 0; j--) {
                    unsigned pbits = n_type_bits(m->target, target->params[j]);
                    int32_t disp = -(int32_t)(12 + (int32_t)frame + 8 * ((int32_t)d0 + j + 1));
                    if (pbits > 32) {
                        mov_reg_ebp(b, EAX, disp); mov_reg_ebp(b, EDX, disp + 4);
                        push64(b, EAX, EDX);
                    } else {
                        mov_reg_ebp(b, EAX, disp);
                        push_reg(b, EAX);
                    }
                }
                CODE(b, 0xe8); n_reloc_add(m, b->len, in->arg); n_put(b, 0, 4);
                int32_t result_disp = -(int32_t)(12 + (int32_t)frame + 8 * ((int32_t)d0 + 1));
                lea_ebp(b, ESP, result_disp);
                if (target->result == N_UNIT) CODE(b, 0x31, 0xc0, 0x31, 0xd2);
                else normalize(b, m->target, target->result);
                mov_esp_reg(b, EAX, 0); mov_esp_reg(b, EDX, 4);
                break;
            }
            case N_LABEL: break;
            case N_JUMP: case N_JZ:
                if (in->op == N_JZ) CODE(b, 0x58, 0x5a, 0x09, 0xd0, 0x0f, 0x84);
                else CODE(b, 0xe9);
                branches[branch_count++] = (Branch){b->len, in->arg}; n_put(b, 0, 4); break;
            case N_RETURN:
                mov_reg_esp(b, EAX, 0); mov_reg_esp(b, EDX, 4);
                if (n_type_bits(m->target, in->type) <= 32) normalize(b, m->target, in->type);
                CODE(b, 0x8d, 0x65, 0xf4, 0x5f, 0x5e, 0x5b, 0x5d, 0xc3);
                break;
            case N_END: CODE(b, 0x31, 0xc0, 0x8d, 0x65, 0xf4, 0x5f, 0x5e, 0x5b, 0x5d, 0xc3); break;
            case N_ASSERT: CODE(b, 0x58, 0x5a, 0x09, 0xd0, 0x75, 0x02, 0x0f, 0x0b); break;
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

bool n_encode_x86(NModule *m) {
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
        CODE(&m->text, 0x31, 0xed, 0x83, 0xe4, 0xf0); /* xor ebp,ebp; and esp,-16 */
        CODE(&m->text, 0xe8); n_reloc_add(m, m->text.len, main_index); n_put(&m->text, 0, 4);
        CODE(&m->text, 0x89, 0xc3, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x0f, 0x0b);
        m->entry_size = m->text.len - m->entry_offset;
    }
    return !m->codegen->had_error && !re0_buffer_failed(&m->text);
}
