#include "backend/native_internal.h"
#include <string.h>

#define CODE(b, ...) do { const unsigned char c_[] = {__VA_ARGS__}; re0_buffer_write_n((b), (const char *)c_, sizeof(c_)); } while (0)

/* Scalar SSE2 operations. Inputs are raw bits in RAX/RCX; result in RAX.
 * XMM0/XMM1 and RDX are caller-saved scratch. No host FP code executes in output. */
static void to_xmm0(Re0Buffer *b) { CODE(b, 0x66,0x48,0x0f,0x6e,0xc0); }
static void from_xmm0(Re0Buffer *b, NType type) {
    if (type == N_F32) CODE(b, 0x66,0x0f,0x7e,0xc0);
    else CODE(b, 0x66,0x48,0x0f,0x7e,0xc0);
}
static size_t jump(Re0Buffer *b, unsigned char condition) {
    if (condition) CODE(b, 0x0f,condition);
    else CODE(b, 0xe9);
    size_t offset = b->len; n_put(b, 0, 4); return offset;
}
static void bind(Re0Buffer *b, size_t offset) { n_patch(b, offset, (uint32_t)(b->len - offset - 4), 4); }
static void immediate(Re0Buffer *b, uint64_t value) { CODE(b, 0x48,0xb8); n_put(b, value, 8); }
static void bound(Re0Buffer *b, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    immediate(b, bits);
    CODE(b, 0x66,0x48,0x0f,0x6e,0xc8); /* xmm1 = rax */
}

void n_x64_float_binary(Re0Buffer *b, Re0BinOpKind op, NType type) {
    to_xmm0(b);
    CODE(b, 0x66,0x48,0x0f,0x6e,0xc9); /* xmm1 = rcx */
    if (op >= BINOP_EQ && op <= BINOP_GE) {
        if (type == N_F64) CODE(b, 0x66);
        CODE(b, 0x0f,0x2e,0xc1);
        unsigned char cc = op == BINOP_EQ ? 0x94 : op == BINOP_NE ? 0x95 :
                           op == BINOP_LT ? 0x92 : op == BINOP_LE ? 0x96 :
                           op == BINOP_GT ? 0x97 : 0x93;
        CODE(b, 0x0f,cc,0xc0);
        if (op == BINOP_NE) CODE(b, 0x0f,0x9a,0xc2, 0x08,0xd0); /* unordered is != */
        else if (op == BINOP_EQ || op == BINOP_LT || op == BINOP_LE)
            CODE(b, 0x0f,0x9b,0xc2, 0x20,0xd0); /* exclude unordered */
        CODE(b, 0x0f,0xb6,0xc0);
        return;
    }
    unsigned char opcode = op == BINOP_ADD ? 0x58 : op == BINOP_SUB ? 0x5c :
                           op == BINOP_MUL ? 0x59 : 0x5e;
    CODE(b, type == N_F32 ? 0xf3 : 0xf2, 0x0f,opcode,0xc1);
    from_xmm0(b, type);
}

static void float_to_integer(Re0Buffer *b, NType to) {
    /* Input is f64 in xmm0. Match the C runtime: NaN -> 0 and saturate
     * before truncation; unsigned 64-bit values need a 2^63 split. */
    CODE(b, 0x66,0x0f,0x2e,0xc0);
    size_t nan = jump(b, 0x8a);
    unsigned bits = n_type_bits(to);
    bool sign = n_type_signed(to);
    double upper = sign ? (double)(UINT64_C(1) << (bits - 1)) :
                   bits == 64 ? 18446744073709551616.0 : (double)(UINT64_C(1) << bits);
    bound(b, upper);
    CODE(b, 0x66,0x0f,0x2e,0xc1);
    size_t overflow = jump(b, 0x83); /* >= upper */
    bound(b, sign ? -upper : 0.0);
    CODE(b, 0x66,0x0f,0x2e,0xc1);
    size_t underflow = jump(b, 0x86); /* <= lower */
    if (!sign && bits == 64) {
        bound(b, 9223372036854775808.0);
        CODE(b, 0x66,0x0f,0x2e,0xc1);
        size_t small = jump(b, 0x82);
        CODE(b, 0xf2,0x0f,0x5c,0xc1, 0xf2,0x48,0x0f,0x2c,0xc0,
                0x48,0x0f,0xba,0xe8,0x3f); /* subtract 2^63, truncate, set bit 63 */
        size_t converted = jump(b, 0);
        bind(b, small);
        CODE(b, 0xf2,0x48,0x0f,0x2c,0xc0);
        bind(b, converted);
    } else CODE(b, 0xf2,0x48,0x0f,0x2c,0xc0);
    size_t done1 = jump(b, 0);
    bind(b, overflow);
    uint64_t maximum = sign ? (UINT64_C(1) << (bits - 1)) - 1 :
                       bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    immediate(b, maximum);
    size_t done2 = jump(b, 0);
    bind(b, underflow);
    immediate(b, sign ? UINT64_C(0) - (UINT64_C(1) << (bits - 1)) : 0);
    size_t done3 = jump(b, 0);
    bind(b, nan); CODE(b, 0x31,0xc0);
    bind(b, done1); bind(b, done2); bind(b, done3);
}

void n_x64_float_convert(Re0Buffer *b, NType from, NType to) {
    if (n_type_float(from)) {
        to_xmm0(b);
        if (n_type_float(to)) {
            if (from != to) CODE(b, from == N_F32 ? 0xf3 : 0xf2, 0x0f,0x5a,0xc0);
            from_xmm0(b, to);
        } else {
            if (from == N_F32) CODE(b, 0xf3,0x0f,0x5a,0xc0);
            if (to == N_BOOL) {
                CODE(b, 0x66,0x0f,0xef,0xc9, 0x66,0x0f,0x2e,0xc1,
                        0x0f,0x95,0xc0, 0x0f,0x9a,0xc2, 0x08,0xd0, 0x0f,0xb6,0xc0);
            } else float_to_integer(b, to);
        }
    } else {
        unsigned char prefix = to == N_F32 ? 0xf3 : 0xf2;
        if (!n_type_signed(from) && n_type_bits(from) == 64) {
            CODE(b, 0x48,0x85,0xc0);
            size_t small = jump(b, 0x89); /* sign bit not set */
            CODE(b, 0x48,0x89,0xc2, 0x83,0xe2,0x01, 0x48,0xd1,0xe8, 0x48,0x09,0xd0,
                    prefix,0x48,0x0f,0x2a,0xc0, prefix,0x0f,0x58,0xc0);
            size_t done = jump(b, 0);
            bind(b, small); CODE(b, prefix,0x48,0x0f,0x2a,0xc0); bind(b, done);
        } else CODE(b, prefix,0x48,0x0f,0x2a,0xc0);
        from_xmm0(b, to);
    }
}
