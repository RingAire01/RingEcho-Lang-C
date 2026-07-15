#ifndef RE0_GC_CONFIG_H
#define RE0_GC_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

/* ── GC 模式：控制「何时」触发回收 ── */
typedef enum {
    RE0_GC_MODE_NONE = 0,    /* 无 GC：堆对象仅手动 free，引擎不自动回收 */
    RE0_GC_MODE_AUTO,        /* 系统 GC：分配量超阈值时自动 mark-sweep */
    RE0_GC_MODE_MANUAL,      /* 开发者 GC：仅 gc_collect() API 显式触发 */
} Re0GcMode;

/* ── GC 算法：控制「如何」回收 ── */
typedef enum {
    RE0_GC_ALGO_TRACING = 0,   /* 纯 mark-sweep，从 roots 遍历对象图 */
    RE0_GC_ALGO_ARC_CYCLE,     /* 引用计数 + 循环引用检测 */
    RE0_GC_ALGO_HYBRID,        /* 混合：OWNED 即时释放，GC 对象走 tracing */
} Re0GcAlgo;

/* ── 指针语义类别 ── */
typedef enum {
    RE0_PTR_KIND_NULLABLE = 0,  /* 可空：允许指向 NULL */
    RE0_PTR_KIND_NONNULL,       /* 非空：保证非 NULL */
    RE0_PTR_KIND_OWNED,         /* 所有权：持有者负责释放 */
    RE0_PTR_KIND_BORROWED,      /* 借用：不拥有，不释放 */
    RE0_PTR_KIND_WEAK,          /* 弱引用：不阻止回收 */
} Re0PtrKind;

/* ── 三色标记位 ── */
enum {
    RE0_GC_COLOR_WHITE = 0,  /* 未访问（可回收） */
    RE0_GC_COLOR_GRAY  = 1,  /* 已入队待遍历子节点 */
    RE0_GC_COLOR_BLACK = 2,  /* 已标记存活，子节点已遍历 */
};

/* ── 默认参数 ── */
#define RE0_GC_DEFAULT_THRESHOLD  1024
#define RE0_GC_DEFAULT_FACTOR     2.0f   /* 下次阈值 = 存活量 * factor */
#define RE0_GC_ROOT_CAP_INIT      16
#define RE0_GC_MARK_STACK_INIT    64

/* ── 运行时配置 ── */
typedef struct {
    Re0GcMode mode;
    Re0GcAlgo algo;
    int       threshold;    /* AUTO 触发阈值（分配计数） */
    float     gc_factor;    /* 回收后阈值放大因子 */
    bool      verbose;      /* 是否打印 GC 事件到 stderr */
} Re0GcConfig;

static inline Re0GcConfig re0_gc_config_default(void) {
    Re0GcConfig c;
    c.mode      = RE0_GC_MODE_NONE;
    c.algo      = RE0_GC_ALGO_TRACING;
    c.threshold = RE0_GC_DEFAULT_THRESHOLD;
    c.gc_factor = RE0_GC_DEFAULT_FACTOR;
    c.verbose   = false;
    return c;
}

static inline Re0GcMode re0_gc_mode_from_str(const char *s) {
    if (!s) return RE0_GC_MODE_NONE;
    if (s[0] == 'a' && s[1] == 'u' && s[2] == 't' && s[3] == 'o' && s[4] == '\0')
        return RE0_GC_MODE_AUTO;
    if (s[0] == 'm' && s[1] == 'a' && s[2] == 'n' && s[3] == 'u' &&
        s[4] == 'a' && s[5] == 'l' && s[6] == '\0')
        return RE0_GC_MODE_MANUAL;
    return RE0_GC_MODE_NONE;
}

static inline const char *re0_gc_mode_name(Re0GcMode m) {
    switch (m) {
        case RE0_GC_MODE_NONE:   return "none";
        case RE0_GC_MODE_AUTO:   return "auto";
        case RE0_GC_MODE_MANUAL: return "manual";
    }
    return "unknown";
}

static inline const char *re0_gc_algo_name(Re0GcAlgo a) {
    switch (a) {
        case RE0_GC_ALGO_TRACING:   return "tracing";
        case RE0_GC_ALGO_ARC_CYCLE: return "arc-cycle";
        case RE0_GC_ALGO_HYBRID:    return "hybrid";
    }
    return "unknown";
}

#endif
