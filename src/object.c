#include "object.h"

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
    default:
        /* Collection destructors are added in plan 03. */
        break;
    }
    free(o);
}

size_t obj_size(const robj *o) {
    size_t sz = sizeof(robj) + 48; /* object + entry overhead estimate */
    if (o->type == OBJ_STRING) {
        sstr *s = o->ptr;
        sz += sizeof(sstr) + s->len;
    }
    return sz;
}
