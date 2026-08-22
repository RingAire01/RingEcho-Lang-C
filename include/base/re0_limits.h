#ifndef RE0_LIMITS_H
#define RE0_LIMITS_H

/* ── C backend codegen capacities ── */
#define RE0_MAX_VAR_TYPES         256
#define RE0_MAX_FN_RETS           128
#define RE0_MAX_LAMBDAS            64
#define RE0_MAX_GENERIC_STRUCTS    32
#define RE0_MAX_INSTANTIATED      256
#define RE0_MAX_GENERIC_FNS        64

/* ── reo ISA backend codegen capacities ── */
#define RE0_MAX_REO_VARS          128
#define RE0_MAX_REO_STRUCTS        32

/* ── lint thresholds ── */
#define RE0_MAX_LINT_VARS         256
#define RE0_MAX_FUNC_LINES         50
#define RE0_MAX_NESTING             5

/* ── recursion depth limits ── */
#define RE0_MAX_PARSE_DEPTH       512
#define RE0_MAX_SEMA_DEPTH        256
#define RE0_MAX_JSON_DEPTH        256

/* ── file/message size limits ── */
#define RE0_MAX_SOURCE_BYTES    (16 * 1024 * 1024)
#define RE0_MAX_LSP_MESSAGE     (16 * 1024 * 1024)
#define RE0_MAX_JSON_STRING     (1 * 1024 * 1024)

#endif
