#ifndef RE0_ABI_H
#define RE0_ABI_H
#include <stdbool.h>

/* Backend-independent ABI concepts.
 *
 * Linkage determines symbol naming / who resolves the symbol; the calling
 * convention determines argument/return registers, stack layout and
 * alignment. They are modelled separately and must never be encoded as a
 * backend-only string. */

typedef enum {
    RE0_LINKAGE_REO,  /* RingEcho-defined symbol (default) */
    RE0_LINKAGE_C     /* external C linkage: unmangled, target-default C ABI */
} Re0Linkage;

typedef enum {
    RE0_CC_DEFAULT,   /* target-default convention for the linkage */
    RE0_CC_SYSV64,    /* x86-64 System V */
    RE0_CC_WIN64,     /* Microsoft x64 */
    RE0_CC_AAPCS64,   /* AArch64 */
    RE0_CC_AAPCS32,   /* ARM 32-bit EABI */
    RE0_CC_SYSV32,    /* i386 System V */
    RE0_CC_MS32,      /* Microsoft x86 (cdecl) */
    RE0_CC_MSARM64    /* Microsoft ARM64 */
} Re0CallingConvention;

const char *re0_linkage_name(Re0Linkage linkage);
const char *re0_calling_convention_name(Re0CallingConvention cc);

/* Parse an extern ABI string. Returns false for an unknown spelling. "C"
 * selects C linkage with the target-default convention. */
bool re0_abi_parse(const char *text, Re0Linkage *linkage, Re0CallingConvention *cc);

#endif
