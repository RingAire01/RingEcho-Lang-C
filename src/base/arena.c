#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

Re0Arena *re0_arena_new(void) {
    Re0Arena *a = (Re0Arena*)malloc(sizeof(Re0Arena));
    if (!a) return NULL;
    Re0ArenaBlock *b = (Re0ArenaBlock*)malloc(sizeof(Re0ArenaBlock));
    if (!b) { free(a); return NULL; }
    b->mem = (char*)malloc(RE0_ARENA_BLOCK_SIZE);
    if (!b->mem) { free(b); free(a); return NULL; }
    b->cap = RE0_ARENA_BLOCK_SIZE;
    b->used = 0;
    b->next = NULL;
    a->head = b;
    a->cur = b;
    return a;
}

static Re0ArenaBlock *re0_arena_new_block(size_t min_sz) {
    size_t sz = min_sz > RE0_ARENA_BLOCK_SIZE ? min_sz : RE0_ARENA_BLOCK_SIZE;
    Re0ArenaBlock *b = (Re0ArenaBlock*)malloc(sizeof(Re0ArenaBlock));
    if (!b) return NULL;
    b->mem = (char*)malloc(sz);
    if (!b->mem) { free(b); return NULL; }
    b->cap = sz;
    b->used = 0;
    b->next = NULL;
    return b;
}

void *re0_arena_alloc(Re0Arena *a, size_t sz) {
    if (!a || !sz) return NULL;
    if (sz > SIZE_MAX - 7) return NULL;
    size_t aligned = (sz + 7) & ~7;
    if (aligned > a->cur->cap - a->cur->used) {
        Re0ArenaBlock *nb = re0_arena_new_block(aligned);
        if (!nb) return NULL;
        a->cur->next = nb;
        a->cur = nb;
    }
    void *ptr = a->cur->mem + a->cur->used;
    a->cur->used += aligned;
    return ptr;
}

void *re0_arena_alloc_zero(Re0Arena *a, size_t sz) {
    void *p = re0_arena_alloc(a, sz);
    if (p) memset(p, 0, sz);
    return p;
}

char *re0_arena_strdup(Re0Arena *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char*)re0_arena_alloc(a, n);
    if (d) memcpy(d, s, n);
    return d;
}

void re0_arena_free(Re0Arena *a) {
    if (!a) return;
    Re0ArenaBlock *b = a->head;
    while (b) {
        Re0ArenaBlock *next = b->next;
        free(b->mem);
        free(b);
        b = next;
    }
    free(a);
}
