#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_INITIAL_BUCKETS 16
#define MAP_INITIAL_ENTRIES 8
#define MAP_LOAD_NUM 7
#define MAP_LOAD_DEN 10  /* resize when len > 70% of buckets */

#define BUCKET_EMPTY ((int32_t)-1)
#define BUCKET_TOMB  ((int32_t)-2)

static uint32_t hash_string(const char *s) {
    /* FNV-1a 32-bit */
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

static void grow_entries(Map *m, size_t need) {
    if (m->entries_cap >= need) return;
    size_t cap = m->entries_cap ? m->entries_cap : MAP_INITIAL_ENTRIES;
    while (cap < need) cap *= 2;
    MapEntry *p = (MapEntry *)realloc(m->entries, cap * sizeof(MapEntry));
    if (!p) {
        fprintf(stderr, "qsc: map out of memory\n");
        abort();
    }
    m->entries = p;
    m->entries_cap = cap;
}

static void rebuild_buckets(Map *m, size_t new_cap) {
    int32_t *b = (int32_t *)malloc(new_cap * sizeof(int32_t));
    if (!b) {
        fprintf(stderr, "qsc: map out of memory\n");
        abort();
    }
    for (size_t i = 0; i < new_cap; ++i) b[i] = BUCKET_EMPTY;
    /* Reinsert each live entry into the new bucket array. */
    for (size_t i = 0; i < m->entries_len; ++i) {
        if (!m->entries[i].key) continue; /* tombstone */
        uint32_t h = hash_string(m->entries[i].key);
        size_t idx = h & (new_cap - 1);
        while (b[idx] != BUCKET_EMPTY) idx = (idx + 1) & (new_cap - 1);
        b[idx] = (int32_t)i;
    }
    free(m->buckets);
    m->buckets = b;
    m->buckets_cap = new_cap;
}

static void ensure_buckets(Map *m) {
    if (!m->buckets) {
        rebuild_buckets(m, MAP_INITIAL_BUCKETS);
        return;
    }
    /* live entries = entries_len minus tombstones; rough check uses entries_len. */
    if (m->entries_len * MAP_LOAD_DEN > m->buckets_cap * MAP_LOAD_NUM) {
        rebuild_buckets(m, m->buckets_cap * 2);
    }
}

/* Returns the bucket index where `key` lives or where it would be inserted.
 * Sets *found to true if the key is present. */
static size_t probe(const Map *m, const char *key, bool *found) {
    uint32_t h = hash_string(key);
    size_t idx = h & (m->buckets_cap - 1);
    size_t first_tomb = (size_t)-1;
    while (1) {
        int32_t slot = m->buckets[idx];
        if (slot == BUCKET_EMPTY) {
            *found = false;
            return first_tomb != (size_t)-1 ? first_tomb : idx;
        }
        if (slot == BUCKET_TOMB) {
            if (first_tomb == (size_t)-1) first_tomb = idx;
        } else {
            const char *k = m->entries[slot].key;
            if (k && strcmp(k, key) == 0) {
                *found = true;
                return idx;
            }
        }
        idx = (idx + 1) & (m->buckets_cap - 1);
    }
}

void map_init(Map *m) {
    m->entries = NULL;
    m->entries_len = m->entries_cap = 0;
    m->buckets = NULL;
    m->buckets_cap = 0;
}

void map_free(Map *m) {
    free(m->entries);
    free(m->buckets);
    map_init(m);
}

bool map_put(Map *m, const char *key, void *value) {
    ensure_buckets(m);
    bool found = false;
    size_t bidx = probe(m, key, &found);
    if (found) {
        int32_t slot = m->buckets[bidx];
        m->entries[slot].value = value;
        return true;
    }
    grow_entries(m, m->entries_len + 1);
    int32_t eidx = (int32_t)m->entries_len++;
    m->entries[eidx].key = key;
    m->entries[eidx].value = value;
    m->buckets[bidx] = eidx;
    /* Resizing on subsequent insert if the load factor was just exceeded. */
    if (m->entries_len * MAP_LOAD_DEN > m->buckets_cap * MAP_LOAD_NUM) {
        rebuild_buckets(m, m->buckets_cap * 2);
    }
    return false;
}

void *map_get(const Map *m, const char *key) {
    if (!m->buckets) return NULL;
    bool found = false;
    size_t bidx = probe(m, key, &found);
    if (!found) return NULL;
    return m->entries[m->buckets[bidx]].value;
}

bool map_has(const Map *m, const char *key) {
    if (!m->buckets) return false;
    bool found = false;
    (void)probe(m, key, &found);
    return found;
}

bool map_remove(Map *m, const char *key) {
    if (!m->buckets) return false;
    bool found = false;
    size_t bidx = probe(m, key, &found);
    if (!found) return false;
    int32_t slot = m->buckets[bidx];
    m->entries[slot].key = NULL;     /* mark entry as tombstone */
    m->entries[slot].value = NULL;
    m->buckets[bidx] = BUCKET_TOMB;
    return true;
}

size_t map_len(const Map *m) {
    /* This counts live entries by scanning; insertion-order map_len is
     * rarely called in hot paths, so this is fine. */
    size_t n = 0;
    for (size_t i = 0; i < m->entries_len; ++i) {
        if (m->entries[i].key) n++;
    }
    return n;
}

bool map_iter(const Map *m, size_t *idx, const char **out_key, void **out_value) {
    while (*idx < m->entries_len) {
        const MapEntry *e = &m->entries[*idx];
        (*idx)++;
        if (e->key) {
            if (out_key) *out_key = e->key;
            if (out_value) *out_value = e->value;
            return true;
        }
    }
    return false;
}
