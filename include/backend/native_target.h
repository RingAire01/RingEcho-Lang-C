#ifndef RE0_NATIVE_TARGET_H
#define RE0_NATIVE_TARGET_H
#include <stdbool.h>

/* Target description for the native (direct machine-code) backend. This layer
 * is the single source of truth for architecture, operating system, calling
 * convention and object format. Code generation and object serialization read
 * from a descriptor instead of hardcoding host or x86-64 assumptions.
 *
 * All descriptors are immutable static data; callers hold borrowed pointers for
 * the lifetime of the process. No global mutable state. */

typedef enum {
    RE0_ARCH_X86_64, RE0_ARCH_X86, RE0_ARCH_AARCH64, RE0_ARCH_ARM
} Re0NativeArch;

typedef enum {
    RE0_OS_LINUX, RE0_OS_MACOS, RE0_OS_WINDOWS
} Re0NativeOs;

/* Calling convention. The architecture does not fully determine the ABI: the
 * same instruction set uses different argument/return registers on Windows. */
typedef enum {
    RE0_ABI_SYSV64,   /* x86-64 System V (Linux ELF, macOS Mach-O) */
    RE0_ABI_SYSV32,   /* i386 System V (Linux ELF) */
    RE0_ABI_AAPCS64,  /* AArch64 Procedure Call Standard (Linux, macOS) */
    RE0_ABI_AAPCS32,  /* ARM 32-bit EABI (Linux) */
    RE0_ABI_MS64,     /* Microsoft x64 */
    RE0_ABI_MS32,     /* Microsoft x86 */
    RE0_ABI_MSARM64   /* Microsoft ARM64 */
} Re0NativeAbi;

typedef enum {
    RE0_OBJ_ELF64, RE0_OBJ_ELF32, RE0_OBJ_MACHO64, RE0_OBJ_COFF
} Re0NativeObject;

typedef struct Re0NativeTarget {
    const char      *triple;         /* canonical GNU/LLVM target triple */
    Re0NativeArch    arch;
    Re0NativeOs      os;
    Re0NativeAbi     abi;
    Re0NativeObject  object;
    unsigned         pointer_size;   /* bytes */
    unsigned         pointer_align;  /* bytes, power of two */
    unsigned         int128_align;   /* bytes, power of two */
    unsigned         stack_align;    /* bytes, required at a call boundary */
    unsigned         word_size;      /* stack value slot size (>= pointer) */
} Re0NativeTarget;

/* Returns NULL for an unknown or empty triple. */
const Re0NativeTarget *re0_native_target_find(const char *triple);
/* Best match for the compiling host, or NULL if the host is unsupported. */
const Re0NativeTarget *re0_native_target_host(void);
/* Whether this build can encode the given target (arch implemented). */
bool re0_native_target_supported(const Re0NativeTarget *target);

#endif
