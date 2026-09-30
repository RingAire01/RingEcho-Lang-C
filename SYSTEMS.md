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

## 5. Planned (tracked milestones)

- `unsafe` boundary (raw operations gated).
- `@repr(C|packed|transparent)`, `@align(N)`, `sizeof`/`alignof`/`offsetof`.
- `PhysAddr`/`VirtAddr` newtypes.
- volatile memory semantics.
- atomics (`AtomicU*`, orderings).
- freestanding profile (no libc/pthread/host runtime).
