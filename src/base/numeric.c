#include "base/numeric.h"
#include <stddef.h>
#include <string.h>

bool re0_integer_parse(const char *digits, unsigned radix, Re0Integer *out) {
    if (!digits || !out || radix < 2 || radix > 16) return false;
    Re0Integer value = {0, 0, false};
    if (*digits == '-') { value.negative = true; digits++; }
    else if (*digits == '+') digits++;
    if (!*digits) return false;
    size_t count = 0;
    for (; *digits; digits++) {
        if (++count > 512) return false;
        unsigned digit = *digits >= '0' && *digits <= '9' ? (unsigned)(*digits - '0') :
                         *digits >= 'a' && *digits <= 'f' ? (unsigned)(*digits - 'a' + 10) :
                         *digits >= 'A' && *digits <= 'F' ? (unsigned)(*digits - 'A' + 10) : 16;
        if (digit >= radix) return false;
        uint32_t words[4] = {(uint32_t)value.low, (uint32_t)(value.low >> 32),
                             (uint32_t)value.high, (uint32_t)(value.high >> 32)};
        uint64_t carry = digit;
        for (unsigned i = 0; i < 4; i++) {
            uint64_t product = (uint64_t)words[i] * radix + carry;
            words[i] = (uint32_t)product;
            carry = product >> 32;
        }
        if (carry) return false;
        value.low = ((uint64_t)words[1] << 32) | words[0];
        value.high = ((uint64_t)words[3] << 32) | words[2];
    }
    *out = value;
    return true;
}

bool re0_integer_fits(Re0Integer value, unsigned bits, bool signed_target) {
    if (!bits || bits > 128) return false;
    if (!signed_target && value.negative && (value.high || value.low)) return false;
    unsigned magnitude_bits = bits - (signed_target ? 1U : 0U);
    if (magnitude_bits == 128) return true;
    uint64_t high = magnitude_bits >= 64 ? (UINT64_C(1) << (magnitude_bits - 64)) : 0;
    uint64_t low = magnitude_bits < 64 ? (UINT64_C(1) << magnitude_bits) : 0;
    if (value.high < high || (value.high == high && value.low < low)) return true;
    return signed_target && value.negative && value.high == high && value.low == low;
}
