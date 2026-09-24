#include "analysis/layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);
static unsigned calls, failure;
static bool fail_now(void) { return ++calls == failure; }
void *__wrap_realloc(void *p, size_t n) { return fail_now() ? NULL : __real_realloc(p, n); }
void *__wrap_calloc(size_t n, size_t size) { return fail_now() ? NULL : __real_calloc(n, size); }
char *__wrap_strdup(const char *s) { return fail_now() ? NULL : __real_strdup(s); }

static bool check(unsigned fail_at, unsigned *count) {
    Re0Type small = {.kind = RE0_TYPE_U8}, wide = {.kind = RE0_TYPE_U64};
    Re0Type array = {.kind = RE0_TYPE_ARRAY, .array = {.inner = &wide, .size = 3}};
    Re0Type *members[] = {&small, &array};
    Re0Type tuple = {.kind = RE0_TYPE_TUPLE, .tuple = {.elems = members, .count = 2}};
    Re0LayoutManager manager;
    if (!re0_layout_init(&manager, &re0_target_x86_64_sysv, NULL, NULL, NULL)) return false;
    calls = 0; failure = fail_at;
    Re0Layout *result = re0_layout_type(&manager, &tuple);
    unsigned total = calls;
    failure = 0;
    bool expected_success = !fail_at || fail_at > total;
    bool passed = expected_success ? result && result->size == 32 && !manager.failed : !result && manager.failed;
    re0_layout_destroy(&manager);
    if (count) *count = total;
    return passed;
}

int main(void) {
    unsigned count;
    if (!check(0, &count)) return 1;
    for (unsigned i = 1; i <= count; i++) {
        if (!check(i, NULL)) { fprintf(stderr, "layout failure point %u did not propagate\n", i); return 1; }
    }
    printf("layout allocation faults passed (%u points)\n", count);
    return 0;
}
