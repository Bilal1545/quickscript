/*
 * map.h — insertion-ordered string→pointer hash map.
 *
 * Keys must outlive the map (caller-owned, typically arena-allocated).
 * Iteration order matches insertion order. This is required because codegen
 * emits identifiers in the order they were first seen, and we want
 * bit-for-bit reproducibility against the JS compiler.
 *
 * Used for: builtin lookup tables, knownFunctions set, globals set,
 * per-module renameMap, exports map.
 *
 * Backed by a separate insertion-ordered array and an open-addressed
 * bucket index. Removal is supported via tombstones (not used yet).
 */

#ifndef QSC_MAP_H
#define QSC_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *key;
    void *value;
} MapEntry;

typedef struct {
    /* Insertion-ordered entries. NULL key indicates a removed slot. */
    MapEntry *entries;
    size_t entries_len;
    size_t entries_cap;

    /* Index into entries[] for each bucket. -1 = empty, -2 = tombstone. */
    int32_t *buckets;
    size_t buckets_cap;
} Map;

void map_init(Map *m);
void map_free(Map *m);

/* Returns true if key existed (value replaced); false if newly inserted. */
bool map_put(Map *m, const char *key, void *value);

/* Returns NULL if not found. Use map_has() to distinguish "missing" from "value is NULL". */
void *map_get(const Map *m, const char *key);
bool map_has(const Map *m, const char *key);

/* Returns true if a value existed and was removed. */
bool map_remove(Map *m, const char *key);

size_t map_len(const Map *m);

/* Iterate in insertion order. *idx starts at 0; returns false at end.
 * Skips tombstoned entries. Output pointers are stable until the next mutation. */
bool map_iter(const Map *m, size_t *idx, const char **out_key, void **out_value);

#endif /* QSC_MAP_H */
