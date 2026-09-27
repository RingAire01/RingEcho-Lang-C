/* Reo big integer - an external (out-of-language) arbitrary-precision integer.
 *
 * RingEcho's primitive integers stop at 128 bits. Rather than grow the
 * language, big integers are provided as an ordinary external library that Reo
 * binds through the FFI channel:
 *
 *   rev run app.reo --lib-dir libs/bigint/build --link reo_bigint
 *
 * Handles are heap pointers passed as uint64_t so a Reo `u64` can hold them.
 * Every operation returns a freshly allocated value that the caller must
 * release with reo_big_free; inputs are never modified.
 */
#ifndef REO_BIGINT_H
#define REO_BIGINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a decimal string (optional leading '+', no sign otherwise) into a new
 * value. Returns 0 on invalid input or allocation failure. */
uint64_t reo_big_from_dec(const char *text);

/* Create a value from a machine word. */
uint64_t reo_big_from_u64(uint64_t value);

/* Release a value. NULL/0 is a no-op. */
void reo_big_free(uint64_t handle);

/* Decimal rendering in a rotating static buffer (valid until three more calls). */
const char *reo_big_to_dec(uint64_t handle);

/* 1 when the value is zero, else 0. */
int reo_big_is_zero(uint64_t handle);

/* Number of significant bits (0 for zero). */
int reo_big_bit_length(uint64_t handle);

/* Three-way comparison: -1, 0, or 1. */
int reo_big_cmp(uint64_t a, uint64_t b);

/* Arithmetic; results are new values. reo_big_sub saturates at zero. */
uint64_t reo_big_add(uint64_t a, uint64_t b);
uint64_t reo_big_sub(uint64_t a, uint64_t b);
uint64_t reo_big_mul(uint64_t a, uint64_t b);
uint64_t reo_big_div(uint64_t a, uint64_t b);
uint64_t reo_big_mod(uint64_t a, uint64_t b);
uint64_t reo_big_gcd(uint64_t a, uint64_t b);

/* (a mod m) reduced into a machine word; m must be non-zero. */
uint64_t reo_big_mod_small(uint64_t a, uint64_t m);

#ifdef __cplusplus
}
#endif

#endif /* REO_BIGINT_H */
