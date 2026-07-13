#ifndef LIST_H
#define LIST_H

#include <stddef.h>
#include "object.h" /* sstr */

typedef struct listNode {
    sstr *value;
    struct listNode *prev;
    struct listNode *next;
} listNode;

typedef struct mlist {
    listNode *head;
    listNode *tail;
    size_t len;
} mlist;

mlist *list_new(void);
void list_free(mlist *l);

void list_push_head(mlist *l, const void *p, size_t len);
void list_push_tail(mlist *l, const void *p, size_t len);

/* Pop returns the detached sstr (caller owns / must free), or NULL if empty. */
sstr *list_pop_head(mlist *l);
sstr *list_pop_tail(mlist *l);

static inline size_t list_len(const mlist *l) { return l->len; }

/* Approximate heap footprint, for memory accounting. */
size_t list_bytes(const mlist *l);

#endif
