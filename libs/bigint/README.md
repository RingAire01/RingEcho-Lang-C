# Reo big integers (external)

Reo's primitive integers end at 128 bits (`i128`/`u128`). Instead of growing the
language, big integers are an **external library** bound through the existing FFI
channel: `rev build|run <file> --lib-dir <d> --link <l>` (plus `--include <h>`
for the header). A value is an opaque heap handle carried in a Reo `u64`.

## API

| Reo declaration | Purpose |
| --- | --- |
| `reo_big_from_dec(text: str) -> u64` | parse a decimal string into a new value |
| `reo_big_from_u64(value: u64) -> u64` | value from a machine word |
| `reo_big_to_dec(handle: u64) -> str` | decimal rendering (rotating static buffer) |
| `reo_big_free(handle: u64)` | release a value |
| `reo_big_is_zero / reo_big_bit_length / reo_big_cmp` | query |
| `reo_big_add / sub / mul / div / mod / gcd` | arithmetic (results are new) |
| `reo_big_mod_small(handle, m: u64) -> u64` | fast `value mod m` for small `m` |

Inputs are never modified; every arithmetic result must be freed with
`reo_big_free`. `reo_big_to_dec` renders into one of four rotating buffers, so
copy the string before making three more calls if you need to keep it.

## Build and run

```sh
make bigint            # builds libs/bigint/build/libreo_bigint.a
make test-bigint       # builds it and runs demo_bigprime.reo
```

Or manually:

```sh
cc -O2 -c libs/bigint/bigint.c -o libs/bigint/build/bigint.o
ar rcs libs/bigint/build/libreo_bigint.a libs/bigint/build/bigint.o
rev run libs/bigint/demo_bigprime.reo --lib-dir libs/bigint/build --link reo_bigint
```

`demo_bigprime.reo` loads a 768-bit prime, prints it, shows it has no factor
below one million, and exercises subtraction and multiplication — all beyond the
language's native range. The same pattern binds any external integer library
(GMP, libtommath, ...): declare its functions in `extern { ... }` and point
`--include`/`--lib-dir`/`--link` at it.
