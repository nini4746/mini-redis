#include "db.h"
#include "zmalloc.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>

static void free_robj(void *v) { obj_free((robj *)v); }

redisDb *db_create(void) {
    redisDb *db = zcalloc(1, sizeof(*db));
    db->dict = dict_create(free_robj);
    db->expires = dict_create(NULL); /* values are inline ms, not owned */
    return db;
}

/* Return 1 if the key currently carries an expire in the past. */
static int key_is_expired(redisDb *db, const void *key, size_t klen) {
    if (dict_size(db->expires) == 0) return 0;
    dictEntry *e = dict_find(db->expires, key, klen);
    if (!e) return 0;
    long long when = (long long)(intptr_t)e->val;
    return mstime() >= when;
}

/* Shared lookup: lazily deletes an expired key (returning NULL) and
 * refreshes the object's LRU clock on a hit. This single chokepoint is
 * why no read/write path can ever observe an expired value. */
static robj *lookup(redisDb *db, const void *key, size_t klen) {
    robj *o = dict_get(db->dict, key, klen);
    if (!o) return NULL;
    if (key_is_expired(db, key, klen)) {
        db_delete(db, key, klen);
        return NULL;
    }
    o->lru = mstime();
    return o;
}

robj *db_lookup(redisDb *db, const void *key, size_t klen) {
    return lookup(db, key, klen);
}

robj *db_lookup_write(redisDb *db, const void *key, size_t klen) {
    return lookup(db, key, klen);
}

void db_set(redisDb *db, const void *key, size_t klen, robj *o) {
    dict_set(db->dict, key, klen, o);
    dict_del(db->expires, key, klen); /* SET clears TTL */
}

int db_delete(redisDb *db, const void *key, size_t klen) {
    dict_del(db->expires, key, klen);
    return dict_del(db->dict, key, klen);
}

void db_set_expire(redisDb *db, const void *key, size_t klen, long long when_ms) {
    dict_set(db->expires, key, klen, (void *)(intptr_t)when_ms);
}

/* Returns absolute expire time in ms, or -1 if the key has no expire. */
long long db_get_expire(redisDb *db, const void *key, size_t klen) {
    dictEntry *e = dict_find(db->expires, key, klen);
    if (!e) return -1;
    return (long long)(intptr_t)e->val;
}

int db_remove_expire(redisDb *db, const void *key, size_t klen) {
    return dict_del(db->expires, key, klen);
}

size_t db_size(redisDb *db) {
    return dict_size(db->dict);
}

void db_flush(redisDb *db) {
    dict_free(db->dict);
    dict_free(db->expires);
    db->dict = dict_create(free_robj);
    db->expires = dict_create(NULL);
}
