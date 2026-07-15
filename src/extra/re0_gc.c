#include "re0_gc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

Re0GcPool *re0_gc_new(int threshold) {
    Re0GcPool *gc = (Re0GcPool *)malloc(sizeof(Re0GcPool));
    if (!gc) return NULL;
    gc->head = NULL;
    gc->roots = NULL;
    gc->root_count = 0;
    gc->root_cap = 0;
    gc->total_count = 0;
    gc->threshold = threshold > 0 ? threshold : 1024;
    return gc;
}

Re0GcNode *re0_gc_alloc(Re0GcPool *gc, size_t sz, Re0PtrKind kind, void (*dtor)(void *)) {
    if (!gc || !sz) return NULL;
    Re0GcNode *n = (Re0GcNode *)malloc(sizeof(Re0GcNode));
    if (!n) return NULL;
    n->ptr = malloc(sz);
    if (!n->ptr) { free(n); return NULL; }
    n->size = sz;
    n->ref_count = (kind == RE0_PTR_OWNED || kind == RE0_PTR_BORROWED) ? 1 : 0;
    n->kind = kind;
    n->marked = false;
    n->dtor = dtor;
    n->next = gc->head;
    gc->head = n;
    gc->total_count++;
    return n;
}

Re0GcNode *re0_gc_alloc_zero(Re0GcPool *gc, size_t sz, Re0PtrKind kind, void (*dtor)(void *)) {
    Re0GcNode *n = re0_gc_alloc(gc, sz, kind, dtor);
    if (n && n->ptr) memset(n->ptr, 0, sz);
    return n;
}

char *re0_gc_strdup(Re0GcPool *gc, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    Re0GcNode *n = re0_gc_alloc(gc, len, RE0_PTR_OWNED, NULL);
    if (!n) return NULL;
    memcpy(n->ptr, s, len);
    return (char *)n->ptr;
}

void re0_gc_retain(Re0GcNode *n) {
    if (n) n->ref_count++;
}

void re0_gc_release(Re0GcNode *n) {
    if (!n) return;
    n->ref_count--;
    if (n->ref_count <= 0 && n->kind != RE0_PTR_BORROWED) {
        if (n->dtor) n->dtor(n->ptr);
        free(n->ptr);
        n->ptr = NULL;
    }
}

void re0_gc_add_root(Re0GcPool *gc, void **root) {
    if (!gc || !root) return;
    if (gc->root_count >= gc->root_cap) {
        int nc = gc->root_cap ? gc->root_cap * 2 : 16;
        gc->roots = (Re0GcNode **)realloc(gc->roots, sizeof(Re0GcNode *) * nc);
        gc->root_cap = nc;
    }
    gc->roots[gc->root_count++] = (Re0GcNode *)(*root);
}

void re0_gc_remove_root(Re0GcPool *gc, void **root) {
    if (!gc || !root) return;
    for (int i = 0; i < gc->root_count; i++) {
        if (gc->roots[i] == (Re0GcNode *)(*root)) {
            gc->roots[i] = gc->roots[--gc->root_count];
            return;
        }
    }
}

static void gc_mark(Re0GcNode *n) {
    if (!n || n->marked) return;
    n->marked = true;
}

static void gc_sweep(Re0GcPool *gc) {
    Re0GcNode **pp = &gc->head;
    while (*pp) {
        Re0GcNode *n = *pp;
        if (!n->marked && n->ref_count <= 0 && n->kind != RE0_PTR_BORROWED) {
            *pp = n->next;
            if (n->dtor) n->dtor(n->ptr);
            free(n->ptr);
            free(n);
            gc->total_count--;
        } else {
            n->marked = false;
            pp = &n->next;
        }
    }
}

void re0_gc_collect(Re0GcPool *gc) {
    if (!gc) return;
    for (int i = 0; i < gc->root_count; i++)
        gc_mark(gc->roots[i]);
    gc_sweep(gc);
}

void re0_gc_stats(Re0GcPool *gc, int *alive, int *bytes) {
    if (!gc) return;
    int a = 0, b = 0;
    Re0GcNode *n = gc->head;
    while (n) { a++; b += (int)n->size; n = n->next; }
    if (alive) *alive = a;
    if (bytes) *bytes = b;
}

void re0_gc_destroy(Re0GcPool *gc) {
    if (!gc) return;
    Re0GcNode *n = gc->head;
    while (n) {
        Re0GcNode *next = n->next;
        if (n->dtor) n->dtor(n->ptr);
        free(n->ptr);
        free(n);
        n = next;
    }
    free(gc->roots);
    free(gc);
}
