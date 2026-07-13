#include "db.h"

#include <stdlib.h>

static void free_robj(void *v) { obj_free((robj *)v); }

redisDb *db_create(void) {
    redisDb *db = calloc(1, sizeof(*db));
    db->dict = dict_create(free_robj);
    db->expires = dict_create(NULL); /* values are inline ms, not owned */
    return db;
}

robj *db_lookup(redisDb *db, const void *key, size_t klen) {
    /* Expiry handling is wired in during plan 04; for now a plain lookup. */
    return dict_get(db->dict, key, klen);
}

robj *db_lookup_write(redisDb *db, const void *key, size_t klen) {
    return dict_get(db->dict, key, klen);
}

void db_set(redisDb *db, const void *key, size_t klen, robj *o) {
    dict_set(db->dict, key, klen, o);
    dict_del(db->expires, key, klen); /* SET clears TTL */
}

int db_delete(redisDb *db, const void *key, size_t klen) {
    dict_del(db->expires, key, klen);
    return dict_del(db->dict, key, klen);
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
