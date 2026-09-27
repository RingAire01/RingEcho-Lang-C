/* Reo big integer - external arbitrary-precision unsigned integers.
 * Little-endian base-2^32 limbs. See bigint.h for the Reo-facing contract. */

#include "bigint.h"

#include <stdlib.h>
#include <string.h>

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 reo_u128;
#else
typedef unsigned long long reo_u128;
#endif

typedef struct {
    uint32_t *limbs; /* little-endian, base 2^32 */
    size_t len;      /* at least 1; never has leading zero limbs */
} Big;

static uint64_t to_handle(Big *b) { return (uint64_t)(uintptr_t)b; }
static Big *from_handle(uint64_t h) { return (Big *)(uintptr_t)h; }

static Big *big_new(size_t len) {
    if (len == 0) len = 1;
    Big *b = (Big *)malloc(sizeof(Big));
    if (!b) return NULL;
    b->limbs = (uint32_t *)calloc(len, sizeof(uint32_t));
    if (!b->limbs) {
        free(b);
        return NULL;
    }
    b->len = len;
    return b;
}

static void big_normalize(Big *b) {
    while (b->len > 1 && b->limbs[b->len - 1] == 0) b->len--;
}

static uint64_t big_from_u64_internal(uint64_t value) {
    Big *b = big_new(2);
    if (!b) return 0;
    b->limbs[0] = (uint32_t)value;
    b->limbs[1] = (uint32_t)(value >> 32);
    big_normalize(b);
    return to_handle(b);
}

static uint64_t big_copy(uint64_t handle) {
    Big *a = from_handle(handle);
    if (!a) return 0;
    Big *b = big_new(a->len);
    if (!b) return 0;
    memcpy(b->limbs, a->limbs, a->len * sizeof(uint32_t));
    b->len = a->len;
    return to_handle(b);
}

uint64_t reo_big_from_u64(uint64_t value) { return big_from_u64_internal(value); }

uint64_t reo_big_from_dec(const char *text) {
    if (!text) return 0;
    const char *p = text;
    if (*p == '+') p++;
    if (*p == '\0') return 0;

    Big *acc = big_new(1);
    if (!acc) return 0;
    acc->limbs[0] = 0;

    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            reo_big_free(to_handle(acc));
            return 0;
        }
        uint64_t carry = (uint64_t)(*p - '0');
        for (size_t i = 0; i < acc->len; i++) {
            uint64_t cur = (uint64_t)acc->limbs[i] * 10u + carry;
            acc->limbs[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
        while (carry) {
            uint32_t *grown = (uint32_t *)realloc(acc->limbs, (acc->len + 1) * sizeof(uint32_t));
            if (!grown) {
                reo_big_free(to_handle(acc));
                return 0;
            }
            acc->limbs = grown;
            acc->limbs[acc->len++] = (uint32_t)carry;
            carry >>= 32;
        }
    }
    big_normalize(acc);
    return to_handle(acc);
}

void reo_big_free(uint64_t handle) {
    Big *b = from_handle(handle);
    if (!b) return;
    free(b->limbs);
    free(b);
}

int reo_big_is_zero(uint64_t handle) {
    Big *b = from_handle(handle);
    if (!b) return 1;
    return (b->len == 1 && b->limbs[0] == 0) ? 1 : 0;
}

int reo_big_bit_length(uint64_t handle) {
    Big *b = from_handle(handle);
    if (!b) return 0;
    uint32_t top = b->limbs[b->len - 1];
    int bits = 0;
    while (top) {
        bits++;
        top >>= 1;
    }
    return (int)((b->len - 1) * 32) + bits;
}

int reo_big_cmp(uint64_t ah, uint64_t bh) {
    Big *a = from_handle(ah);
    Big *b = from_handle(bh);
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    for (size_t i = a->len; i-- > 0;) {
        if (a->limbs[i] != b->limbs[i]) return a->limbs[i] < b->limbs[i] ? -1 : 1;
    }
    return 0;
}

uint64_t reo_big_add(uint64_t ah, uint64_t bh) {
    Big *a = from_handle(ah);
    Big *b = from_handle(bh);
    if (!a || !b) return 0;
    size_t n = a->len > b->len ? a->len : b->len;
    Big *r = big_new(n + 1);
    if (!r) return 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t cur = carry;
        if (i < a->len) cur += a->limbs[i];
        if (i < b->len) cur += b->limbs[i];
        r->limbs[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    r->limbs[n] = (uint32_t)carry;
    big_normalize(r);
    return to_handle(r);
}

uint64_t reo_big_sub(uint64_t ah, uint64_t bh) {
    Big *a = from_handle(ah);
    Big *b = from_handle(bh);
    if (!a || !b) return 0;
    if (reo_big_cmp(ah, bh) <= 0) return big_from_u64_internal(0);
    Big *r = big_new(a->len);
    if (!r) return 0;
    uint64_t borrow = 0;
    for (size_t i = 0; i < a->len; i++) {
        uint64_t cur = (uint64_t)a->limbs[i] - borrow - (i < b->len ? b->limbs[i] : 0);
        r->limbs[i] = (uint32_t)cur;
        borrow = (cur >> 32) != 0 ? 1 : 0;
    }
    big_normalize(r);
    return to_handle(r);
}

uint64_t reo_big_mul(uint64_t ah, uint64_t bh) {
    Big *a = from_handle(ah);
    Big *b = from_handle(bh);
    if (!a || !b) return 0;
    Big *r = big_new(a->len + b->len + 1);
    if (!r) return 0;
    for (size_t i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b->len; j++) {
            uint64_t cur = (uint64_t)r->limbs[i + j] + (uint64_t)a->limbs[i] * b->limbs[j] + carry;
            r->limbs[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        size_t k = i + b->len;
        while (carry) {
            uint64_t cur = (uint64_t)r->limbs[k] + carry;
            r->limbs[k] = (uint32_t)cur;
            carry = cur >> 32;
            k++;
        }
    }
    big_normalize(r);
    return to_handle(r);
}

static uint64_t big_shl1(uint64_t handle) {
    Big *a = from_handle(handle);
    if (!a) return 0;
    Big *r = big_new(a->len + 1);
    if (!r) return 0;
    uint32_t carry = 0;
    for (size_t i = 0; i < a->len; i++) {
        uint32_t v = a->limbs[i];
        r->limbs[i] = (v << 1) | carry;
        carry = v >> 31;
    }
    r->limbs[a->len] = carry;
    big_normalize(r);
    return to_handle(r);
}

static void big_set_bit(uint64_t handle, size_t pos) {
    Big *a = from_handle(handle);
    if (!a) return;
    size_t idx = pos / 32;
    if (idx >= a->len) {
        uint32_t *grown = (uint32_t *)realloc(a->limbs, (idx + 1) * sizeof(uint32_t));
        if (!grown) return;
        a->limbs = grown;
        for (size_t i = a->len; i <= idx; i++) a->limbs[i] = 0;
        a->len = idx + 1;
    }
    a->limbs[idx] |= (uint32_t)(1u << (pos % 32));
}

/* Binary long division: 0 if b is zero. */
static void big_divmod(uint64_t a, uint64_t b, uint64_t *quotient, uint64_t *remainder) {
    if (reo_big_is_zero(b)) {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    Big *A = from_handle(a);
    uint64_t q = big_from_u64_internal(0);
    uint64_t r = big_from_u64_internal(0);
    for (size_t i = A->len; i-- > 0;) {
        for (int bit = 31; bit >= 0; bit--) {
            uint64_t shifted = big_shl1(r);
            if (shifted) {
                reo_big_free(r);
                r = shifted;
            }
            if ((A->limbs[i] >> bit) & 1u) {
                Big *rb = from_handle(r);
                rb->limbs[0] |= 1u;
            }
            if (reo_big_cmp(r, b) >= 0) {
                uint64_t next = reo_big_sub(r, b);
                reo_big_free(r);
                r = next;
                big_set_bit(q, i * 32 + (size_t)bit);
            }
        }
    }
    *quotient = q;
    *remainder = r;
}

uint64_t reo_big_div(uint64_t a, uint64_t b) {
    uint64_t q, r;
    big_divmod(a, b, &q, &r);
    reo_big_free(r);
    return q;
}

uint64_t reo_big_mod(uint64_t a, uint64_t b) {
    uint64_t q, r;
    big_divmod(a, b, &q, &r);
    reo_big_free(q);
    return r;
}

uint64_t reo_big_gcd(uint64_t ah, uint64_t bh) {
    uint64_t a = big_copy(ah);
    uint64_t b = big_copy(bh);
    while (!reo_big_is_zero(b)) {
        uint64_t r = reo_big_mod(a, b);
        reo_big_free(a);
        a = b;
        b = r;
    }
    reo_big_free(b);
    return a;
}

uint64_t reo_big_mod_small(uint64_t ah, uint64_t m) {
    Big *a = from_handle(ah);
    if (!a || m == 0) return 0;
    reo_u128 rem = 0;
    for (size_t i = a->len; i-- > 0;) {
        rem = ((rem << 32) | (reo_u128)a->limbs[i]) % (reo_u128)m;
    }
    return (uint64_t)rem;
}

const char *reo_big_to_dec(uint64_t handle) {
    static char ring[4][1200];
    static unsigned slot = 0;
    char *out = ring[slot & 3u];
    slot++;

    Big *b = from_handle(handle);
    if (!b) {
        memcpy(out, "0", 2);
        return out;
    }
    size_t n = b->len;
    uint32_t *tmp = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!tmp) {
        memcpy(out, "<oom>", 6);
        return out;
    }
    memcpy(tmp, b->limbs, n * sizeof(uint32_t));

    char digits[1200];
    size_t pos = 0;
    int done;
    do {
        uint64_t rem = 0;
        for (size_t i = n; i-- > 0;) {
            uint64_t cur = (rem << 32) | tmp[i];
            tmp[i] = (uint32_t)(cur / 1000000000ULL);
            rem = cur % 1000000000ULL;
        }
        while (n > 1 && tmp[n - 1] == 0) n--;
        done = (n == 1 && tmp[0] == 0);
        for (int k = 0; k < 9 && pos + 1 < sizeof(digits); k++) {
            digits[pos++] = (char)('0' + (int)(rem % 10));
            rem /= 10;
        }
    } while (!done);

    while (pos > 1 && digits[pos - 1] == '0') pos--;
    for (size_t i = 0; i < pos; i++) out[i] = digits[pos - 1 - i];
    out[pos] = '\0';
    free(tmp);
    return out;
}
