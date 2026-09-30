#ifndef RE0_TARGET_H
#define RE0_TARGET_H
#include <stddef.h>
#include <stdbool.h>
#include "base/abi.h"

/* Shared target layout descriptor. This is the single source of truth for the
 * machine model used by the type system, semantic analysis and the layout
 * manager: pointer width/alignment, the alignment of 128-bit integers, the
 * stack alignment at a public interface, the maximum object size, endianness,
 * and the target's default/supported C calling conventions. Backend-specific
 * facts (object format) live in the backend target tables; they are
 * intentionally not duplicated here.
 *
 * All instances are immutable static data; callers hold borrowed pointers. */
typedef struct {
    const char *name;
    unsigned    pointer_size;   /* bytes */
    unsigned    pointer_align;  /* bytes, power of two */
    unsigned    int128_align;   /* bytes, power of two */
    unsigned    stack_align;    /* bytes at a call boundary */
    size_t      max_object_size;
    bool        big_endian;
    Re0CallingConvention default_cc; /* target-default C convention */
    unsigned    supported_cc;        /* bitmask of (1u << Re0CallingConvention) */
} Re0TargetLayout;

extern const Re0TargetLayout re0_target_x86_64_sysv;
extern const Re0TargetLayout re0_target_x86_64_win64;
extern const Re0TargetLayout re0_target_x86_sysv;
extern const Re0TargetLayout re0_target_x86_win32;
extern const Re0TargetLayout re0_target_aarch64_lp64;
extern const Re0TargetLayout re0_target_aarch64_win64;
extern const Re0TargetLayout re0_target_arm_aapcs;

/* Layout of the machine this compiler runs on. Never NULL. */
const Re0TargetLayout *re0_target_layout_host(void);
/* Look up by name ("x86_64", "x86_64-windows", "aarch64", "arm", ...). */
const Re0TargetLayout *re0_target_layout_find(const char *name);

#endif
