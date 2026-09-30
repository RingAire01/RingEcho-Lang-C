#include "backend/native_target.h"
#include <string.h>

/* Immutable target registry. Keep entries sorted by arch then OS for stable
 * iteration; lookup is by exact triple. */
static const Re0NativeTarget targets[] = {
    { "x86_64-unknown-linux-gnu", RE0_ARCH_X86_64, RE0_OS_LINUX, RE0_ABI_SYSV64,
      RE0_OBJ_ELF64, 8, 8, 16, 16, 8 },
    { "i686-unknown-linux-gnu", RE0_ARCH_X86, RE0_OS_LINUX, RE0_ABI_SYSV32,
      RE0_OBJ_ELF32, 4, 4, 16, 16, 8 },
    { "aarch64-unknown-linux-gnu", RE0_ARCH_AARCH64, RE0_OS_LINUX, RE0_ABI_AAPCS64,
      RE0_OBJ_ELF64, 8, 8, 16, 16, 8 },
    { "armv7-unknown-linux-gnueabihf", RE0_ARCH_ARM, RE0_OS_LINUX, RE0_ABI_AAPCS32,
      RE0_OBJ_ELF32, 4, 4, 8, 8, 8 },
    { "x86_64-apple-darwin", RE0_ARCH_X86_64, RE0_OS_MACOS, RE0_ABI_SYSV64,
      RE0_OBJ_MACHO64, 8, 8, 16, 16, 8 },
    { "aarch64-apple-darwin", RE0_ARCH_AARCH64, RE0_OS_MACOS, RE0_ABI_AAPCS64,
      RE0_OBJ_MACHO64, 8, 8, 16, 16, 8 },
    { "x86_64-pc-windows-msvc", RE0_ARCH_X86_64, RE0_OS_WINDOWS, RE0_ABI_MS64,
      RE0_OBJ_COFF, 8, 8, 16, 16, 8 },
    { "i686-pc-windows-msvc", RE0_ARCH_X86, RE0_OS_WINDOWS, RE0_ABI_MS32,
      RE0_OBJ_COFF, 4, 4, 16, 16, 8 },
    { "aarch64-pc-windows-msvc", RE0_ARCH_AARCH64, RE0_OS_WINDOWS, RE0_ABI_MSARM64,
      RE0_OBJ_COFF, 8, 8, 16, 16, 8 },
};

const Re0NativeTarget *re0_native_target_find(const char *triple) {
    if (!triple || !*triple) return NULL;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (strcmp(targets[i].triple, triple) == 0) return &targets[i];
    return NULL;
}

const Re0NativeTarget *re0_native_target_host(void) {
#if defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    return re0_native_target_find("aarch64-apple-darwin");
  #elif defined(__x86_64__)
    return re0_native_target_find("x86_64-apple-darwin");
  #endif
#elif defined(_WIN32)
  #if defined(__aarch64__)
    return re0_native_target_find("aarch64-pc-windows-msvc");
  #elif defined(_M_X64) || defined(__x86_64__)
    return re0_native_target_find("x86_64-pc-windows-msvc");
  #elif defined(_M_IX86) || defined(__i386__)
    return re0_native_target_find("i686-pc-windows-msvc");
  #endif
#elif defined(__linux__)
  #if defined(__aarch64__)
    return re0_native_target_find("aarch64-unknown-linux-gnu");
  #elif defined(__arm__)
    return re0_native_target_find("armv7-unknown-linux-gnueabihf");
  #elif defined(__x86_64__)
    return re0_native_target_find("x86_64-unknown-linux-gnu");
  #elif defined(__i386__)
    return re0_native_target_find("i686-unknown-linux-gnu");
  #endif
#endif
    return NULL;
}

bool re0_native_target_supported(const Re0NativeTarget *target) {
    if (!target) return false;
    /* Only targets with a complete encoder and object writer are accepted.
     * As new architectures land this list grows. */
    switch (target->object) {
        case RE0_OBJ_ELF64:
            return target->arch == RE0_ARCH_X86_64 || target->arch == RE0_ARCH_AARCH64;
        case RE0_OBJ_ELF32:
            return target->arch == RE0_ARCH_X86 || target->arch == RE0_ARCH_ARM;
        default:
            return false;
    }
}
