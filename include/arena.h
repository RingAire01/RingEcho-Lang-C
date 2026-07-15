#ifndef RE0_ARENA_H
#define RE0_ARENA_H
#include <stddef.h>
#define RE0_ARENA_BLOCK_SIZE (64 * 1024)
typedef struct Re0ArenaBlock { char *mem; size_t cap; size_t used; struct Re0ArenaBlock *next; } Re0ArenaBlock;
typedef struct { Re0ArenaBlock *head; Re0ArenaBlock *cur; } Re0Arena;
Re0Arena *re0_arena_new(void);
void *re0_arena_alloc(Re0Arena *a, size_t sz);
void *re0_arena_alloc_zero(Re0Arena *a, size_t sz);
char *re0_arena_strdup(Re0Arena *a, const char *s);
void  re0_arena_free(Re0Arena *a);
#endif
