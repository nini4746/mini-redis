#include "dict.h"
#include "zmalloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DICT_INITIAL_SIZE 16

static uint64_t fnv1a(const void *key, size_t klen) {
    const unsigned char *p = key;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < klen; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int key_eq(const dictEntry *e, const void *key, size_t klen) {
    return e->klen == klen && memcmp(e->key, key, klen) == 0;
}

dict *dict_create(dictValFree valfree) {
    dict *d = zcalloc(1, sizeof(*d));
    d->size = DICT_INITIAL_SIZE;
    d->mask = d->size - 1;
    d->table = zcalloc(d->size, sizeof(dictEntry *));
    d->valfree = valfree;
    return d;
}

static void dict_rehash(dict *d, size_t newsize) {
    dictEntry **nt = zcalloc(newsize, sizeof(dictEntry *));
    size_t nmask = newsize - 1;
    for (size_t i = 0; i < d->size; i++) {
        dictEntry *e = d->table[i];
        while (e) {
            dictEntry *next = e->next;
            size_t idx = fnv1a(e->key, e->klen) & nmask;
            e->next = nt[idx];
            nt[idx] = e;
            e = next;
        }
    }
    zfree(d->table);
    d->table = nt;
    d->size = newsize;
    d->mask = nmask;
}

static void maybe_grow(dict *d) {
    if (d->used >= d->size) dict_rehash(d, d->size * 2);
}

dictEntry *dict_find(dict *d, const void *key, size_t klen) {
    size_t idx = fnv1a(key, klen) & d->mask;
    for (dictEntry *e = d->table[idx]; e; e = e->next) {
        if (key_eq(e, key, klen)) return e;
    }
    return NULL;
}

void *dict_get(dict *d, const void *key, size_t klen) {
    dictEntry *e = dict_find(d, key, klen);
    return e ? e->val : NULL;
}

int dict_set(dict *d, const void *key, size_t klen, void *val) {
    size_t idx = fnv1a(key, klen) & d->mask;
    for (dictEntry *e = d->table[idx]; e; e = e->next) {
        if (key_eq(e, key, klen)) {
            if (d->valfree && e->val != val) d->valfree(e->val);
            e->val = val;
            return 0;
        }
    }
    dictEntry *e = zmalloc(sizeof(*e));
    e->key = zmalloc(klen ? klen : 1);
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->val = val;
    e->next = d->table[idx];
    d->table[idx] = e;
    d->used++;
    maybe_grow(d);
    return 1;
}

int dict_del(dict *d, const void *key, size_t klen) {
    size_t idx = fnv1a(key, klen) & d->mask;
    dictEntry *prev = NULL;
    for (dictEntry *e = d->table[idx]; e; prev = e, e = e->next) {
        if (key_eq(e, key, klen)) {
            if (prev) prev->next = e->next;
            else d->table[idx] = e->next;
            if (d->valfree) d->valfree(e->val);
            zfree(e->key);
            zfree(e);
            d->used--;
            return 1;
        }
    }
    return 0;
}

void dict_free(dict *d) {
    if (!d) return;
    for (size_t i = 0; i < d->size; i++) {
        dictEntry *e = d->table[i];
        while (e) {
            dictEntry *next = e->next;
            if (d->valfree) d->valfree(e->val);
            zfree(e->key);
            zfree(e);
            e = next;
        }
    }
    zfree(d->table);
    zfree(d);
}

dictEntry *dict_random(dict *d) {
    if (d->used == 0) return NULL;
    /* Start at a random bucket, scan forward to the next non-empty one,
     * then pick a random node within that bucket's chain. */
    size_t start = (size_t)random() & d->mask;
    for (size_t i = 0; i < d->size; i++) {
        size_t idx = (start + i) & d->mask;
        dictEntry *e = d->table[idx];
        if (!e) continue;
        int chainlen = 0;
        for (dictEntry *t = e; t; t = t->next) chainlen++;
        int pick = (int)((size_t)random() % (size_t)chainlen);
        while (pick-- > 0) e = e->next;
        return e;
    }
    return NULL;
}

void dict_iter_init(dictIterator *it, dict *d) {
    it->d = d;
    it->idx = 0;
    it->entry = NULL;
    it->next = NULL;
}

dictEntry *dict_iter_next(dictIterator *it) {
    dict *d = it->d;
    for (;;) {
        if (it->next) {
            it->entry = it->next;
            it->next = it->entry->next;
            return it->entry;
        }
        if (it->idx >= d->size) return NULL;
        dictEntry *e = d->table[it->idx++];
        if (e) {
            it->entry = e;
            it->next = e->next;
            return e;
        }
    }
}
