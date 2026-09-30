#include "base/abi.h"
#include <string.h>

const char *re0_linkage_name(Re0Linkage linkage) {
    switch (linkage) {
        case RE0_LINKAGE_REO: return "reo";
        case RE0_LINKAGE_C: return "c";
        default: return "?";
    }
}

const char *re0_calling_convention_name(Re0CallingConvention cc) {
    switch (cc) {
        case RE0_CC_DEFAULT: return "default";
        case RE0_CC_SYSV64: return "sysv64";
        case RE0_CC_WIN64: return "win64";
        case RE0_CC_AAPCS64: return "aapcs64";
        case RE0_CC_AAPCS32: return "aapcs32";
        case RE0_CC_SYSV32: return "sysv32";
        case RE0_CC_MS32: return "ms32";
        case RE0_CC_MSARM64: return "msarm64";
        default: return "?";
    }
}

bool re0_abi_parse(const char *text, Re0Linkage *linkage, Re0CallingConvention *cc) {
    if (!text || !*text) return false;
    if (strcmp(text, "C") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_DEFAULT; return true; }
    if (strcmp(text, "sysv64") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_SYSV64; return true; }
    if (strcmp(text, "win64") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_WIN64; return true; }
    if (strcmp(text, "aapcs64") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_AAPCS64; return true; }
    if (strcmp(text, "aapcs32") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_AAPCS32; return true; }
    if (strcmp(text, "sysv32") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_SYSV32; return true; }
    if (strcmp(text, "ms32") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_MS32; return true; }
    if (strcmp(text, "msarm64") == 0) { *linkage = RE0_LINKAGE_C; *cc = RE0_CC_MSARM64; return true; }
    return false;
}
