#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_CHUNK (64 * 1024)
#define ARENA_ALIGN 16

static size_t align_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static ArenaChunk *new_chunk(size_t cap) {
    ArenaChunk *c = (ArenaChunk *)malloc(sizeof(ArenaChunk) + cap);
    if (!c) {
        fprintf(stderr, "qsc: arena out of memory\n");
        abort();
    }
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

void arena_init(Arena *a) {
    a->head = NULL;
    a->default_chunk_size = ARENA_DEFAULT_CHUNK;
}

void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *n = c->next;
        free(c);
        c = n;
    }
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t size) {
    size = align_up(size ? size : 1, ARENA_ALIGN);

    if (a->head) {
        size_t aligned_used = align_up(a->head->used, ARENA_ALIGN);
        if (aligned_used + size <= a->head->cap) {
            void *p = a->head->data + aligned_used;
            a->head->used = aligned_used + size;
            memset(p, 0, size);
            return p;
        }
    }

    size_t cap = a->default_chunk_size;
    if (size > cap) cap = size;

    ArenaChunk *c = new_chunk(cap);
    c->next = a->head;
    a->head = c;
    void *p = c->data;
    c->used = size;
    memset(p, 0, size);
    return p;
}

char *arena_strndup(Arena *a, const char *src, size_t len) {
    char *p = (char *)arena_alloc(a, len + 1);
    if (len) memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

char *arena_strdup(Arena *a, const char *src) {
    return arena_strndup(a, src, strlen(src));
}
