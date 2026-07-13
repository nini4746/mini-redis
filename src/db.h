#ifndef DB_H
#define DB_H

#include "dict.h"
#include "object.h"

typedef struct redisDb {
    dict *dict;     /* key -> robj* */
    dict *expires;  /* key -> absolute expire time in ms (stored inline) */
} redisDb;

redisDb *db_create(void);

/* Look up a key on the read path: expired keys are lazily deleted here
 * (returns NULL for them) and the object's LRU clock is refreshed. */
robj *db_lookup(redisDb *db, const void *key, size_t klen);

/* Look up a key on the write path (same expiry check, refreshes LRU). */
robj *db_lookup_write(redisDb *db, const void *key, size_t klen);

/* Insert or overwrite key -> o (takes ownership of o). Clears any TTL. */
void db_set(redisDb *db, const void *key, size_t klen, robj *o);

/* Delete key (and its TTL). Returns 1 if it existed. */
int db_delete(redisDb *db, const void *key, size_t klen);

/* Expire table operations. */
void db_set_expire(redisDb *db, const void *key, size_t klen, long long when_ms);
long long db_get_expire(redisDb *db, const void *key, size_t klen); /* -1 if none */
int db_remove_expire(redisDb *db, const void *key, size_t klen);

size_t db_size(redisDb *db);
void db_flush(redisDb *db);

#endif
