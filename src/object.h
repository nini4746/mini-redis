#ifndef OBJECT_H
#define OBJECT_H

#include <stddef.h>

typedef enum {
    OBJ_STRING = 0,
    OBJ_LIST   = 1,
    OBJ_HASH   = 2,
    OBJ_SET    = 3
} obj_type;

/* Binary-safe string value (length carried; NUL-terminated for convenience). */
typedef struct sstr {
    size_t len;
    char data[]; /* flexible array member */
} sstr;

sstr *sstr_new(const void *p, size_t len);

/* Redis value object. */
typedef struct robj {
    obj_type type;
    void *ptr;       /* OBJ_STRING: sstr*; OBJ_LIST: mlist*; HASH/SET: dict* */
    long long lru;   /* last access time in ms (approximate LRU, plan 04) */
} robj;

robj *obj_new(obj_type type, void *ptr);
robj *obj_string(const void *p, size_t len);
robj *obj_string_ll(long long v);
void obj_free(robj *o);

/* Approximate heap footprint of an object, for the memory accounting used
 * by eviction (plan 04/05). */
size_t obj_size(const robj *o);

#endif
