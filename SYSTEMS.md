# RingEcho systems model

This document specifies the low-level (systems) semantics of RingEcho: the
machine model, raw pointers and memory model, and — as they land — layout
control, volatile, atomics, ABI and the freestanding contract. It is the
normative reference for kernel/compiler work; backend support is tracked in
[NATIVE.md](NATIVE.md) and [TYPE_SYSTEM.md](TYPE_SYSTEM.md).

## 1. Machine model

A compile selects one `Re0TargetLayout` (see `include/base/target.h`):

| Target    | pointer | pointer align | int128 align | stack align | endian |
|-----------|--------:|--------------:|-------------:|------------:|--------|
| `x86_64`  | 8       | 8             | 16           | 16          | little |
| `x86`     | 4       | 4             | 16           | 16          | little |
| `aarch64` | 8       | 8             | 16           | 16          | little |
| `arm`     | 4       | 4             | 8            | 8           | little |

`isize`/`usize` are **exactly** the pointer width (not aliases of i64/u64).
`sizeof(usize) == sizeof(pointer)`. The semantic analyser uses the selected
target for literal range checks and integer promotion; the layout manager uses
it for `sizeof`/`alignof`/field offsets. Native compilation maps the target
triple to this layout.

## 1a. Mutable globals

`static NAME: T;` and `static NAME: T = init;` (top level) declare a mutable
global object. Storage is a zero-initialized file-scope object; a non-constant
initializer is executed by a generated constructor before `main`, so calls,
allocation and other runtime expressions are allowed. `const` remains an
immutable compile-time macro. Globals are writable from any function.

## 2. Raw pointers

Types:

- `*T` — mutable raw pointer (short form), `*mut T` — mutable, `*const T` — read-only.
- `ptr` — opaque pointer with no pointee (`void*` equivalent).
- `*mut void` / `*const void` — opaque typed pointer; `void` maps to unit here.

An **opaque** pointer (`ptr`, or a pointer whose pointee is `void`/unit) cannot
be dereferenced or used in pointer arithmetic; cast it to a typed pointer first.

Operations:

- address-of a place: `&place` / `&mut place` (yields `&T` / `&mut T`).
- dereference: `*p` (lvalue). Writing needs a mutable place/pointer.
- arithmetic: `p + i`, `i + p`, `p - i` (scaled by `sizeof(T)`); pointer
  difference `p - q` (identical pointee) yields `isize` (element count).
- comparison: `==`, `!=`, `<`, `<=`, `>`, `>=` between pointers with the same
  pointee; `==`/`!=` also compare against the null pointer constant.
- the integer literal `0` is the null pointer constant and is assignable to any
  pointer type and comparable with any pointer.
- casts: pointer ↔ pointer, pointer ↔ integer (`usize`/`isize`/any integer),
  reference ↔ pointer, pointer ↔ function pointer, and `ptr` ↔ `*T`.
  All are explicit (`as`); there are no implicit pointer conversions.

These operations do **not** participate in GC ownership: a raw pointer is a
plain address, never an owned reference. Dereferencing an invalid pointer is
undefined behaviour; the compiler does not insert checks (an `unsafe` boundary
is planned, see §5).

## 1b. Compile-time layout queries

`sizeof<T>()`, `alignof<T>()` and `offsetof<T>("field")` are compile-time
constants whose values are computed by **RingEcho's own target layout model**
(`base/target.h` + `analysis/layout.h`), never by the host C compiler.
Semantic analysis folds them to an integer literal of type `usize`, so every
backend consumes the same value and the result is target-correct (e.g.
`sizeof<usize>()` is 4 for a 32-bit target and 8 for a 64-bit target).
`T` must currently be a single identifier (scalar, pointer, or named
struct/enum); composite spellings are a later extension.

### Explicit layout

Struct layout is defined by RingEcho's layout model and then realized by each
backend (the C backend emits the matching `__attribute__` s; the model is the
source of truth, not the host compiler).

- `@repr(C)` — default C-compatible layout (explicit; no attribute emitted).
- `@repr(packed)` — fields are placed with no padding; the struct alignment
  is 1 unless `@align` raises it.
- `@align(N)` — N must be a power of two in `1..4096`; it raises the struct
  alignment (it must not weaken the natural alignment, otherwise it is
  rejected). Size is rounded up to the alignment.
- `@repr(transparent)` — exactly one non-zero-sized field; the struct has the
  field's size and alignment (newtype). Cannot be combined with packed.

Invalid requests (`@align` not a power of two / out of range / weakening,
`@repr(transparent)` with 0 or >1 fields, packed+transparent, unknown
`@repr(kind)`) are rejected at compile time, eagerly on declaration.

## 2a. Arrays, slices and byte buffers

- `[T; N]` is a contiguous value type; `sizeof([T; N]) == sizeof(T) * N`.
- `[T]` is a **non-owning** slice value (`{ T* data; size_t len; }`).
  Indexing and `len()` work on it; it never allocates.
- **No implicit decay.** A fixed array does not silently become a slice or
  pointer. Decay is explicit and must name a place (so the view cannot dangle):
  - `arr as *const T` / `arr as *mut T` — pointer to the first element;
  - `arr as [T]` — non-owning slice of the whole array.
  Decaying a temporary array is rejected.
- Byte-buffer primitives (over `ptr`, i.e. `void*`):
  - `mem_copy(dst: ptr, src: ptr, n: usize)` — like `memcpy`;
  - `mem_set(dst: ptr, value: u8, n: usize)` — like `memset`;
  - `mem_equal(a: ptr, b: ptr, n: usize) -> bool` — like `memcmp == 0`.
  Any pointer/reference/string converts implicitly to `ptr` for these calls.

## 3. Backend support

| Feature            | C | c-freestanding | native |
|--------------------|---|----------------|--------|
| raw pointer types  | ✅ | ✅             | ⏳ (diagnostic) |
| address-of/deref   | ✅ | ✅             | ⏳ |
| pointer arithmetic | ✅ | ✅             | ⏳ |
| pointer compare    | ✅ | ✅             | ⏳ |
| pointer ↔ integer  | ✅ | ✅             | ⏳ |

Backends that cannot lower a feature emit an explicit compile-time diagnostic;
they never silently fall back or emit wrong code.

## 4. Manual allocation

`alloc(n) -> ptr`, `alloc_zero(n) -> ptr`, `realloc(p, n) -> ptr`
and `free(p)` allocate raw memory outside the GC. These are **hosted**
(malloc/calloc/realloc/free) until the freestanding profile (Milestone C)
provides a pluggable allocator; a successful allocation may return a null
pointer and callers must check. Bytes can be initialised with the
`mem_*` primitives from §2a.

## 6. Foreign functions, linkage and calling conventions

Foreign functions are declared without a body:

```
extern "C" fn puts(s: *const u8) -> i32;
extern "C" { fn ffi_add(a: i32, b: i32) -> i32; }
extern "sysv64" fn f(a: i32) -> i32;
extern "win64"  fn g(a: i32) -> i32;
```

**Linkage and calling convention are distinct compiler-model concepts** (see
`include/base/abi.h`): `Re0Linkage` (REO / C) controls symbol naming and who
resolves the symbol; `Re0CallingConvention` (DEFAULT / SYSV64 / WIN64 /
AAPCS64 / AAPCS32 / SYSV32 / MS32 / MSARM64) controls argument/return
registers, stack layout and alignment. `extern "C"` means C linkage (no
RingEcho mangling) with the *target-default* C convention; `"C"` is never
hard-wired to one architecture.

The target layout carries `default_cc` and the set of `supported_cc`. DEFAULT
resolves to `target->default_cc` during semantic analysis; an explicit
convention is rejected unless the target supports it, so behaviour never
depends on the build host. A backend that cannot realize a convention for a
target (e.g. the native backend, which currently implements only the
target-default ABI) rejects it explicitly rather than falling back.

The C backend realises an explicit convention through the matching C
attribute (`__attribute__((sysv_abi))` / `((ms_abi))`); the attribute is a
realisation of the model's decision, not the source of it. Foreign arguments
and results use the normal RingEcho type checker; `ptr` maps to `void*`.

## 5. Planned (tracked milestones)

- `unsafe` boundary (raw operations gated).
- `@repr(C|packed|transparent)`, `@align(N)`, `sizeof`/`alignof`/`offsetof`.
- `PhysAddr`/`VirtAddr` newtypes.
- volatile memory semantics.
- atomics (`AtomicU*`, orderings).
- freestanding profile (no libc/pthread/host runtime).
