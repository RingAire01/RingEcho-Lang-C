#ifndef RE0_NUMERIC_H
#define RE0_NUMERIC_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint64_t high, low; bool negative; } Re0Integer;
bool re0_integer_parse(const char *digits, unsigned radix, Re0Integer *out);
bool re0_integer_fits(Re0Integer value, unsigned bits, bool signed_target);
#endif
