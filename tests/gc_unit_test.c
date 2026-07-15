/*
 * gc_unit_test.c — GC 引擎模块化单元测试
 *
 * 验证：
 *   1. NONE 模式：不自动回收，手动 free 生效
 *   2. AUTO 模式：超阈值自动 mark-sweep
 *   3. MANUAL 模式：仅 collect() 触发回收
 *   4. 对象图遍历：父→子链正确标记
 *   5. 事件监听器：COLLECT_START/DONE 触发
 *   6. 引用计数 retain/release
 *   7. 统计数据正确性
 */

#include "re0_gc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ── 测试用对象类型 ── */
typedef struct TestNode {
    int   value;
    Re0GcObject *child;   /* 指向子对象（GC 跟踪） */
} TestNode;

/* trace 回调：遍历子对象 */
static void test_node_trace(Re0GcObject *self, Re0GcTraceVisitor visit, void *ctx)
{
    (void)self;
    /* self->ptr 是 TestNode*，但我们需要从 TestNode 获取 child。
     * 注意：trace 回调签名接收 self 对象，我们通过 self->ptr 访问用户数据。 */
    TestNode *tn = (TestNode *)re0_gc_object_data(self);
    if (tn && tn->child) visit(tn->child, ctx);
}

static Re0GcObject *make_node(Re0GcEngine *eng, int value)
{
    Re0GcObject *obj = re0_gc_engine_alloc_zero(
        eng, sizeof(TestNode), RE0_PTR_KIND_OWNED,
        test_node_trace, NULL);
    if (!obj) return NULL;
    TestNode *tn = (TestNode *)obj->ptr;
    tn->value = value;
    tn->child = NULL;
    return obj;
}

/* ── 事件计数器 ── */
typedef struct {
    int alloc_events;
    int collect_start_events;
    int collect_done_events;
    int threshold_events;
} EventCounter;

static void event_handler(const Re0GcEvent *ev, void *ctx)
{
    EventCounter *ec = (EventCounter *)ctx;
    switch (ev->kind) {
        case RE0_GC_EV_ALLOC:            ec->alloc_events++;        break;
        case RE0_GC_EV_COLLECT_START:    ec->collect_start_events++; break;
        case RE0_GC_EV_COLLECT_DONE:     ec->collect_done_events++;  break;
        case RE0_GC_EV_THRESHOLD_REACHED: ec->threshold_events++;    break;
        default: break;
    }
}

/* ════════════════════════════════════════════════════
 *  TEST 1: NONE 模式 — 不自动回收
 * ════════════════════════════════════════════════════ */
static void test_none_mode(void)
{
    printf("[test] NONE mode: no auto-collect... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_NONE;
    cfg.threshold = 4;   /* 低阈值，验证 NONE 不触发 */
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    EventCounter ec = {0};
    re0_gc_engine_on_event(eng, event_handler, &ec);

    /* 分配 8 个对象（超过阈值 4） */
    for (int i = 0; i < 8; i++) {
        Re0GcObject *obj = make_node(eng, i);
        assert(obj);
    }

    /* NONE 模式不应自动回收 */
    assert(ec.collect_start_events == 0);
    assert(ec.threshold_events == 0);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 8);

    /* 手动 collect 只回收悬挂对象（ref_count<=0） */
    re0_gc_engine_collect(eng);
    re0_gc_engine_stats(eng, &stats);
    /* NONE 模式下 OWNED 对象 ref_count=1，不会被 dangling collect 回收 */
    assert(stats.collect_count == 1);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 2: AUTO 模式 — 超阈值自动回收
 * ════════════════════════════════════════════════════ */
static void test_auto_mode(void)
{
    printf("[test] AUTO mode: threshold-triggered collect... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_AUTO;
    cfg.algo = RE0_GC_ALGO_TRACING;
    cfg.threshold = 5;
    cfg.gc_factor = 2.0f;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    EventCounter ec = {0};
    re0_gc_engine_on_event(eng, event_handler, &ec);

    /* 创建 root 对象 */
    Re0GcObject *root = make_node(eng, 100);
    re0_gc_engine_add_root(eng, root);

    /* 分配超出阈值：不可达对象应被自动回收 */
    for (int i = 0; i < 10; i++) {
        Re0GcObject *obj = make_node(eng, i);
        assert(obj);
    }

    /* AUTO 模式应触发过回收（threshold=5，分配了 11 个） */
    assert(ec.threshold_events > 0);
    assert(ec.collect_start_events > 0);
    assert(ec.collect_done_events > 0);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    /* root 应存活，不可达对象应被回收 */
    assert(stats.alive_count >= 1);

    /* root 的值应未被破坏 */
    TestNode *root_data = (TestNode *)root->ptr;
    assert(root_data->value == 100);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 3: MANUAL 模式 — 仅 collect() 触发
 * ════════════════════════════════════════════════════ */
static void test_manual_mode(void)
{
    printf("[test] MANUAL mode: api-triggered collect... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_TRACING;
    cfg.threshold = 3;  /* 低阈值，但 MANUAL 不自动触发 */
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    EventCounter ec = {0};
    re0_gc_engine_on_event(eng, event_handler, &ec);

    Re0GcObject *root = make_node(eng, 1);
    re0_gc_engine_add_root(eng, root);

    /* 分配超出阈值，但 MANUAL 不应自动回收 */
    for (int i = 0; i < 8; i++) {
        make_node(eng, i);
    }

    assert(ec.threshold_events == 0);
    assert(ec.collect_start_events == 0);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 9);  /* root + 8 */

    /* 手动触发回收 */
    re0_gc_engine_collect(eng);
    assert(ec.collect_start_events == 1);
    assert(ec.collect_done_events == 1);

    re0_gc_engine_stats(eng, &stats);
    /* 8 个不可达对象应被回收，仅 root 存活 */
    assert(stats.alive_count == 1);
    assert(stats.last_freed_count == 8);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 4: 对象图遍历 — 链式引用正确标记
 * ════════════════════════════════════════════════════ */
static void test_object_graph_traversal(void)
{
    printf("[test] object graph: chain traversal... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_TRACING;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    /* 创建链：root → A → B → C */
    Re0GcObject *root = make_node(eng, 0);
    Re0GcObject *a    = make_node(eng, 1);
    Re0GcObject *b    = make_node(eng, 2);
    Re0GcObject *c    = make_node(eng, 3);
    Re0GcObject *orphan = make_node(eng, 99);  /* 不可达 */
    (void)orphan;

    /* 建立引用关系 */
    ((TestNode *)root->ptr)->child  = a;
    ((TestNode *)a->ptr)->child     = b;
    ((TestNode *)b->ptr)->child     = c;

    re0_gc_engine_add_root(eng, root);

    /* 回收：orphan 应被回收，链 root→A→B→C 应全部存活 */
    re0_gc_engine_collect(eng);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.last_freed_count == 1);  /* 仅 orphan */
    assert(stats.alive_count == 4);       /* root, A, B, C */

    /* 验证链完整性 */
    assert(((TestNode *)root->ptr)->value == 0);
    assert(((TestNode *)a->ptr)->value == 1);
    assert(((TestNode *)b->ptr)->value == 2);
    assert(((TestNode *)c->ptr)->value == 3);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 5: 手动 free + retain/release
 * ════════════════════════════════════════════════════ */
static void test_manual_free_and_refcount(void)
{
    printf("[test] manual free + retain/release... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_NONE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    Re0GcObject *obj = make_node(eng, 42);
    assert(obj);
    assert(obj->ref_count == 1);  /* OWNED 初始 ref_count=1 */

    re0_gc_engine_retain(eng, obj);
    assert(obj->ref_count == 2);

    re0_gc_engine_release(eng, obj);
    assert(obj->ref_count == 1);

    /* 手动 free（NONE 模式） */
    re0_gc_engine_free(eng, obj);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);
    assert(stats.total_freed == 1);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 6: strdup
 * ════════════════════════════════════════════════════ */
static void test_strdup(void)
{
    printf("[test] strdup via GC engine... ");

    Re0GcConfig cfg = re0_gc_config_default();
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    char *s = re0_gc_engine_strdup(eng, "hello gc");
    assert(s);
    assert(strcmp(s, "hello gc") == 0);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 1);
    assert(stats.alive_bytes == strlen("hello gc") + 1);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 7: 运行时模式切换
 * ════════════════════════════════════════════════════ */
static void test_mode_switch(void)
{
    printf("[test] runtime mode switch... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_NONE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    /* NONE 模式下分配不触发回收 */
    for (int i = 0; i < 10; i++) make_node(eng, i);
    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.collect_count == 0);

    /* 切换到 MANUAL 并回收 */
    re0_gc_engine_set_mode(eng, RE0_GC_MODE_MANUAL);
    re0_gc_engine_collect(eng);
    re0_gc_engine_stats(eng, &stats);
    assert(stats.collect_count == 1);
    /* 无 root，全部回收 */
    assert(stats.alive_count == 0);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 8: ARC 即时释放（不等 collect）
 * ════════════════════════════════════════════════════ */
static void test_arc_immediate_release(void)
{
    printf("[test] ARC: immediate release on ref_count==0... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_ARC_CYCLE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    Re0GcObject *obj = make_node(eng, 42);
    re0_gc_engine_retain(eng, obj);   /* ref_count = 2 */

    Re0GcStats stats;
    re0_gc_engine_release(eng, obj);  /* ref_count = 1 */
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 1);   /* 未释放 */

    re0_gc_engine_release(eng, obj);  /* ref_count = 0 → 即时释放 */
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);   /* 已释放 */
    assert(stats.collect_count == 0); /* 未触发 collect */

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 9: ARC 级联释放链 A→B→C
 * ════════════════════════════════════════════════════ */
static void test_arc_cascade_chain(void)
{
    printf("[test] ARC: cascade release chain A->B->C... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_ARC_CYCLE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    /* A→B→C，每个对象 OWNED ref_count=1，parent 拥有 child */
    Re0GcObject *a = make_node(eng, 1);
    Re0GcObject *b = make_node(eng, 2);
    Re0GcObject *c = make_node(eng, 3);
    ((TestNode *)a->ptr)->child = b;
    ((TestNode *)b->ptr)->child = c;

    /* release A → 级联释放 A、B、C */
    re0_gc_engine_release(eng, a);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);
    assert(stats.total_freed == 3);
    assert(stats.collect_count == 0);  /* 纯即时释放，无 collect */

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 10: ARC 循环引用检测
 * ════════════════════════════════════════════════════ */
static void test_arc_cycle_collection(void)
{
    printf("[test] ARC: cycle reference detection... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_ARC_CYCLE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    /* 创建 A↔B 循环引用 */
    Re0GcObject *a = make_node(eng, 1);
    Re0GcObject *b = make_node(eng, 2);
    ((TestNode *)a->ptr)->child = b;
    re0_gc_engine_retain(eng, b);   /* B ref_count = 2 */
    ((TestNode *)b->ptr)->child = a;
    re0_gc_engine_retain(eng, a);   /* A ref_count = 2 */

    /* release A：A ref_count 2→1（B 还引用 A），不释放 */
    re0_gc_engine_release(eng, a);
    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 2);  /* A、B 都在（循环引用） */

    /* collect：tracing 检测到不可达 → 回收循环垃圾 */
    re0_gc_engine_collect(eng);
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);
    assert(stats.last_freed_count == 2);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 11: ARC 深度链防栈溢出（迭代式验证）
 * ════════════════════════════════════════════════════ */
static void test_arc_deep_chain(void)
{
    printf("[test] ARC: deep chain (10k nodes, iterative)... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_ARC_CYCLE;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    const int DEPTH = 10000;
    Re0GcObject *first = make_node(eng, 0);
    Re0GcObject *prev = first;
    for (int i = 1; i < DEPTH; i++) {
        Re0GcObject *node = make_node(eng, i);
        ((TestNode *)prev->ptr)->child = node;
        prev = node;
    }

    /* release first → 迭代式级联释放全部 10000 个对象 */
    re0_gc_engine_release(eng, first);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);
    assert(stats.total_freed == DEPTH);

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

/* ════════════════════════════════════════════════════
 *  TEST 12: HYBRID — OWNED 即时释放 + 非 OWNED 走 tracing
 * ════════════════════════════════════════════════════ */
static void test_hybrid_mode(void)
{
    printf("[test] HYBRID: OWNED instant + non-OWNED tracing... ");

    Re0GcConfig cfg = re0_gc_config_default();
    cfg.mode = RE0_GC_MODE_MANUAL;
    cfg.algo = RE0_GC_ALGO_HYBRID;
    Re0GcEngine *eng = re0_gc_engine_new(cfg);
    assert(eng);

    /* OWNED 对象：release 即时释放 */
    Re0GcObject *owned_obj = make_node(eng, 1);  /* OWNED ref_count=1 */
    re0_gc_engine_release(eng, owned_obj);

    Re0GcStats stats;
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);  /* OWNED 已即时释放 */

    /* 非 OWNED（NULLABLE）对象：release 不即时释放 */
    Re0GcObject *nullable_obj = re0_gc_engine_alloc(
        eng, sizeof(TestNode), RE0_PTR_KIND_NULLABLE,
        test_node_trace, NULL);
    assert(nullable_obj);

    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 1);  /* NULLABLE 仍在 */

    /* collect 后回收 */
    re0_gc_engine_collect(eng);
    re0_gc_engine_stats(eng, &stats);
    assert(stats.alive_count == 0);  /* 被 tracing 回收 */

    re0_gc_engine_destroy(eng);
    printf("PASS\n");
}

int main(void)
{
    printf("=== GC Engine Unit Tests ===\n\n");
    test_none_mode();
    test_auto_mode();
    test_manual_mode();
    test_object_graph_traversal();
    test_manual_free_and_refcount();
    test_strdup();
    test_mode_switch();
    test_arc_immediate_release();
    test_arc_cascade_chain();
    test_arc_cycle_collection();
    test_arc_deep_chain();
    test_hybrid_mode();
    printf("\n=== All tests passed ===\n");
    return 0;
}
