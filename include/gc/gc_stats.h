#ifndef RE0_GC_STATS_H
#define RE0_GC_STATS_H

#include <stddef.h>
#include <stdint.h>

/* ── GC 统计信息 ──
 * 记录引擎运行期间的累计数据，供 verbose 输出和外部查询使用。
 */
typedef struct {
    /* 当前快照 */
    int    alive_count;       /* 存活对象数 */
    size_t alive_bytes;       /* 存活对象总字节 */

    /* 累计统计 */
    int    total_alloc;       /* 累计分配次数 */
    size_t total_alloc_bytes; /* 累计分配字节 */
    int    total_freed;       /* 累计释放次数 */
    size_t total_freed_bytes; /* 累计释放字节 */
    int    collect_count;     /* 累计回收轮次 */
    uint64_t total_pause_ns;  /* 累计 STW 暂停时间（纳秒） */

    /* 上一次回收的详情 */
    int    last_freed_count;
    size_t last_freed_bytes;
    uint64_t last_pause_ns;
} Re0GcStats;

void re0_gc_stats_reset(Re0GcStats *s);

/* 分配后调用：增加存活计数 */
void re0_gc_stats_on_alloc(Re0GcStats *s, size_t bytes);

/* 释放后调用：减少存活计数 */
void re0_gc_stats_on_free(Re0GcStats *s, size_t bytes);

/* 回收轮次结束调用：记录暂停时间和释放量 */
void re0_gc_stats_on_collect(Re0GcStats *s, int freed_count,
                              size_t freed_bytes, uint64_t pause_ns);

#endif
