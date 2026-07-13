#ifndef DICT_H
#define DICT_H

#include <stddef.h>

typedef struct dictEntry {
    char *key;
    size_t klen;
    void *val;
    struct dictEntry *next;
} dictEntry;

typedef void (*dictValFree)(void *val);

typedef struct dict {
    dictEntry **table;
    size_t size;   /* number of buckets (power of 2) */
    size_t mask;   /* size - 1 */
    size_t used;   /* number of entries */
    dictValFree valfree; /* may be NULL */
} dict;

dict *dict_create(dictValFree valfree);
void dict_free(dict *d);

/* Insert or replace. Returns 1 if the key was new, 0 if it replaced an
 * existing value (the old value is freed via valfree). Takes ownership
 * of val; copies the key. */
int dict_set(dict *d, const void *key, size_t klen, void *val);

/* Returns the stored value, or NULL if absent. */
void *dict_get(dict *d, const void *key, size_t klen);

/* Returns the entry, or NULL if absent (lets callers read key/val). */
dictEntry *dict_find(dict *d, const void *key, size_t klen);

/* Removes a key. Returns 1 if it existed (value freed), 0 otherwise. */
int dict_del(dict *d, const void *key, size_t klen);

static inline size_t dict_size(const dict *d) { return d->used; }

/* Pick a random entry (uniform-ish), or NULL if empty. */
dictEntry *dict_random(dict *d);

/* Iteration. */
typedef struct dictIterator {
    dict *d;
    size_t idx;
    dictEntry *entry;
    dictEntry *next; /* allows deleting current during iteration */
} dictIterator;

void dict_iter_init(dictIterator *it, dict *d);
dictEntry *dict_iter_next(dictIterator *it);

#endif
