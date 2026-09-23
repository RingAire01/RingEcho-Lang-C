#ifndef RE0_GC_CONFIG_H
#define RE0_GC_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

/* ── GC mode: controls "when" collection runs ── */
typedef enum {
    RE0_GC_MODE_NONE = 0,    /* no GC: heap objects freed manually only, engine never auto-collects */
    RE0_GC_MODE_AUTO,        /* system GC: auto mark-sweep when allocations exceed the threshold */
    RE0_GC_MODE_MANUAL,      /* developer GC: only triggered explicitly via gc_collect() */
} Re0GcMode;

/* ── GC algorithm: controls "how" collection runs ── */
typedef enum {
    RE0_GC_ALGO_TRACING = 0,   /* pure mark-sweep, walk the object graph from roots */
    RE0_GC_ALGO_ARC_CYCLE,     /* reference counting + cycle detection */
    RE0_GC_ALGO_HYBRID,        /* hybrid: OWNED released instantly, GC objects traced */
} Re0GcAlgo;

/* ── pointer semantics kinds ── */
typedef enum {
    RE0_PTR_KIND_NULLABLE = 0,  /* nullable: may point to NULL */
    RE0_PTR_KIND_NONNULL,       /* non-null: guaranteed non-NULL */
    RE0_PTR_KIND_OWNED,         /* ownership: holder is responsible for freeing */
    RE0_PTR_KIND_BORROWED,      /* borrow: does not own, never frees */
    RE0_PTR_KIND_WEAK,          /* weak reference: does not prevent collection */
} Re0PtrKind;

/* ── tri-color marking bits ── */
enum {
    RE0_GC_COLOR_WHITE = 0,  /* not visited (collectable) */
    RE0_GC_COLOR_GRAY  = 1,  /* enqueued, children not yet walked */
    RE0_GC_COLOR_BLACK = 2,  /* marked live, children walked */
};

/* ── default parameters ── */
#define RE0_GC_DEFAULT_THRESHOLD  1024
#define RE0_GC_DEFAULT_FACTOR     2.0f   /* next threshold = survivors * factor */
#define RE0_GC_ROOT_CAP_INIT      16
#define RE0_GC_MARK_STACK_INIT    64

/* ── runtime configuration ── */
typedef struct {
    Re0GcMode mode;
    Re0GcAlgo algo;
    int       threshold;    /* AUTO trigger threshold (allocation count) */
    float     gc_factor;    /* post-collection threshold growth factor */
    bool      verbose;      /* print GC events to stderr */
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
