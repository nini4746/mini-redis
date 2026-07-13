#ifndef ZMALLOC_H
#define ZMALLOC_H

#include <stddef.h>

/* Allocation wrappers that keep a running total of live bytes, used as an
 * approximate memory footprint for maxmemory / LRU eviction. Every alloc in
 * the server goes through these so the counter stays balanced. */
void *zmalloc(size_t size);
void *zcalloc(size_t nmemb, size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);

/* Approximate live bytes currently allocated. */
size_t zmalloc_used(void);

#endif
