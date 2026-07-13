#include "object.h"
#include "dict.h"
#include "list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sstr *sstr_new(const void *p, size_t len) {
    sstr *s = malloc(sizeof(sstr) + len + 1);
    s->len = len;
    if (len && p) memcpy(s->data, p, len);
    s->data[len] = '\0';
    return s;
}

robj *obj_new(obj_type type, void *ptr) {
    robj *o = malloc(sizeof(*o));
    o->type = type;
    o->ptr = ptr;
    o->lru = 0;
    return o;
}

robj *obj_string(const void *p, size_t len) {
    return obj_new(OBJ_STRING, sstr_new(p, len));
}

robj *obj_string_ll(long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    return obj_string(buf, (size_t)n);
}

void obj_free(robj *o) {
    if (!o) return;
    switch (o->type) {
    case OBJ_STRING:
        free(o->ptr);
        break;
    case OBJ_LIST:
        list_free(o->ptr);
        break;
    case OBJ_HASH: /* dict created with free() as value destructor */
    case OBJ_SET:  /* dict created with NULL value destructor        */
        dict_free(o->ptr);
        break;
    }
    free(o);
}

size_t obj_size(const robj *o) {
    size_t sz = sizeof(robj) + 48; /* object + entry overhead estimate */
    switch (o->type) {
    case OBJ_STRING: {
        sstr *s = o->ptr;
        sz += sizeof(sstr) + s->len;
        break;
    }
    case OBJ_LIST:
        sz += list_bytes(o->ptr);
        break;
    case OBJ_HASH:
    case OBJ_SET: {
        dict *d = o->ptr;
        dictIterator it;
        dict_iter_init(&it, d);
        dictEntry *e;
        while ((e = dict_iter_next(&it)) != NULL) {
            sz += 48 + e->klen;
            if (o->type == OBJ_HASH && e->val)
                sz += ((sstr *)e->val)->len;
        }
        break;
    }
    }
    return sz;
}
