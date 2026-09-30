#include "base/target.h"
#include <stdint.h>
#include <string.h>

#define CC_BIT(cc) (1u << (cc))

/* int128 alignment follows the platform psABI: 16 on x86-64, i386 and
 * AArch64; 8 on ARM EABI. max_object_size is the signed pointer range. */
const Re0TargetLayout re0_target_x86_64_sysv = {
    "x86_64", 8, 8, 16, 16, (size_t)PTRDIFF_MAX, false,
    RE0_CC_SYSV64, CC_BIT(RE0_CC_SYSV64) | CC_BIT(RE0_CC_WIN64)
};
const Re0TargetLayout re0_target_x86_64_win64 = {
    "x86_64-windows", 8, 8, 16, 16, (size_t)PTRDIFF_MAX, false,
    RE0_CC_WIN64, CC_BIT(RE0_CC_SYSV64) | CC_BIT(RE0_CC_WIN64)
};
const Re0TargetLayout re0_target_x86_sysv = {
    "x86", 4, 4, 16, 16, (size_t)0x7FFFFFFF, false,
    RE0_CC_SYSV32, CC_BIT(RE0_CC_SYSV32)
};
const Re0TargetLayout re0_target_x86_win32 = {
    "x86-windows", 4, 4, 16, 16, (size_t)0x7FFFFFFF, false,
    RE0_CC_MS32, CC_BIT(RE0_CC_MS32)
};
const Re0TargetLayout re0_target_aarch64_lp64 = {
    "aarch64", 8, 8, 16, 16, (size_t)PTRDIFF_MAX, false,
    RE0_CC_AAPCS64, CC_BIT(RE0_CC_AAPCS64)
};
const Re0TargetLayout re0_target_aarch64_win64 = {
    "aarch64-windows", 8, 8, 16, 16, (size_t)PTRDIFF_MAX, false,
    RE0_CC_MSARM64, CC_BIT(RE0_CC_MSARM64)
};
const Re0TargetLayout re0_target_arm_aapcs = {
    "arm", 4, 4, 8, 8, (size_t)0x7FFFFFFF, false,
    RE0_CC_AAPCS32, CC_BIT(RE0_CC_AAPCS32)
};

const Re0TargetLayout *re0_target_layout_host(void) {
#if defined(__aarch64__)
    return &re0_target_aarch64_lp64;
#elif defined(__arm__)
    return &re0_target_arm_aapcs;
#elif defined(__x86_64__) || defined(_M_X64)
    return &re0_target_x86_64_sysv;
#elif defined(__i386__) || defined(_M_IX86)
    return &re0_target_x86_sysv;
#else
    return &re0_target_x86_64_sysv;
#endif
}

const Re0TargetLayout *re0_target_layout_find(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "x86_64") == 0) return &re0_target_x86_64_sysv;
    if (strcmp(name, "x86_64-windows") == 0) return &re0_target_x86_64_win64;
    if (strcmp(name, "x86") == 0 || strcmp(name, "i386") == 0 || strcmp(name, "i686") == 0)
        return &re0_target_x86_sysv;
    if (strcmp(name, "x86-windows") == 0) return &re0_target_x86_win32;
    if (strcmp(name, "aarch64") == 0 || strcmp(name, "arm64") == 0) return &re0_target_aarch64_lp64;
    if (strcmp(name, "aarch64-windows") == 0) return &re0_target_aarch64_win64;
    if (strcmp(name, "arm") == 0 || strcmp(name, "armv7") == 0) return &re0_target_arm_aapcs;
    return NULL;
}
