/*
 * arena.h — bump allocator for compiler lifetime.
 *
 * Allocations live until arena_free(). No individual free.
 * Backing memory grows as a linked list of chunks (default 64 KiB each).
 */

#ifndef QSC_ARENA_H
#define QSC_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t cap;
    size_t used;
    /* memory follows: data[cap] */
    char data[];
} ArenaChunk;

typedef struct {
    ArenaChunk *head;
    size_t default_chunk_size;
} Arena;

void arena_init(Arena *a);
void arena_free(Arena *a);

/* Aligned allocation. Returned pointer is zero-initialized. */
void *arena_alloc(Arena *a, size_t size);

/* Copy len bytes into the arena, append a NUL terminator, return new pointer. */
char *arena_strndup(Arena *a, const char *src, size_t len);
char *arena_strdup(Arena *a, const char *src);

#endif /* QSC_ARENA_H */
