#include "gc/gc_object.h"
#include <stdlib.h>
#include <string.h>

Re0GcObject *re0_gc_object_alloc(size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (size == 0) return NULL;

    void *payload = malloc(size);
    if (!payload) return NULL;

    Re0GcObject *obj = (Re0GcObject *)malloc(sizeof(Re0GcObject));
    if (!obj) { free(payload); return NULL; }

    obj->ptr       = payload;
    obj->size      = size;
    obj->kind      = kind;
    obj->ref_count = (kind == RE0_PTR_KIND_OWNED || kind == RE0_PTR_KIND_BORROWED) ? 1 : 0;
    obj->color     = RE0_GC_COLOR_WHITE;
    obj->trace     = trace;
    obj->dtor      = dtor;
    obj->prev      = NULL;
    obj->next      = NULL;
    return obj;
}

Re0GcObject *re0_gc_object_alloc_zero(size_t size, Re0PtrKind kind,
                                       Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    Re0GcObject *obj = re0_gc_object_alloc(size, kind, trace, dtor);
    if (obj && obj->ptr) memset(obj->ptr, 0, size);
    return obj;
}

Re0GcObject *re0_gc_object_wrap(void *ptr, size_t size, Re0PtrKind kind,
                                  Re0GcTraceFn trace, Re0GcDtorFn dtor)
{
    if (!ptr || size == 0) return NULL;

    Re0GcObject *obj = (Re0GcObject *)malloc(sizeof(Re0GcObject));
    if (!obj) return NULL;

    obj->ptr       = ptr;
    obj->size      = size;
    obj->kind      = kind;
    obj->ref_count = (kind == RE0_PTR_KIND_OWNED || kind == RE0_PTR_KIND_BORROWED) ? 1 : 0;
    obj->color     = RE0_GC_COLOR_WHITE;
    obj->trace     = trace;
    obj->dtor      = dtor;
    obj->prev      = NULL;
    obj->next      = NULL;
    return obj;
}

void re0_gc_object_free_payload(Re0GcObject *obj)
{
    if (!obj) return;
    if (obj->dtor && obj->ptr) obj->dtor(obj->ptr, obj->size);
    free(obj->ptr);
    obj->ptr = NULL;
}

void re0_gc_object_destroy(Re0GcObject *obj)
{
    if (!obj) return;
    re0_gc_object_free_payload(obj);
    free(obj);
}

void re0_gc_object_retain(Re0GcObject *obj)
{
    if (!obj) return;
    obj->ref_count++;
}

void re0_gc_object_release(Re0GcObject *obj)
{
    if (!obj) return;
    if (obj->ref_count > 0) obj->ref_count--;
}
