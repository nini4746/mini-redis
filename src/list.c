#include "list.h"
#include "zmalloc.h"

#include <stdlib.h>

mlist *list_new(void) {
    mlist *l = zcalloc(1, sizeof(*l));
    return l;
}

void list_free(mlist *l) {
    if (!l) return;
    listNode *n = l->head;
    while (n) {
        listNode *next = n->next;
        zfree(n->value);
        zfree(n);
        n = next;
    }
    zfree(l);
}

void list_push_head(mlist *l, const void *p, size_t len) {
    listNode *n = zmalloc(sizeof(*n));
    n->value = sstr_new(p, len);
    n->prev = NULL;
    n->next = l->head;
    if (l->head) l->head->prev = n;
    else l->tail = n;
    l->head = n;
    l->len++;
}

void list_push_tail(mlist *l, const void *p, size_t len) {
    listNode *n = zmalloc(sizeof(*n));
    n->value = sstr_new(p, len);
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail) l->tail->next = n;
    else l->head = n;
    l->tail = n;
    l->len++;
}

sstr *list_pop_head(mlist *l) {
    if (!l->head) return NULL;
    listNode *n = l->head;
    l->head = n->next;
    if (l->head) l->head->prev = NULL;
    else l->tail = NULL;
    l->len--;
    sstr *v = n->value;
    zfree(n);
    return v;
}

sstr *list_pop_tail(mlist *l) {
    if (!l->tail) return NULL;
    listNode *n = l->tail;
    l->tail = n->prev;
    if (l->tail) l->tail->next = NULL;
    else l->head = NULL;
    l->len--;
    sstr *v = n->value;
    zfree(n);
    return v;
}

size_t list_bytes(const mlist *l) {
    size_t sz = sizeof(mlist);
    for (listNode *n = l->head; n; n = n->next) {
        sz += sizeof(listNode) + sizeof(sstr) + n->value->len;
    }
    return sz;
}
