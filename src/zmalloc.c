#include "zmalloc.h"

#ifdef __linux__
#include <malloc.h>
#define malloc_size malloc_usable_size
#else
#include <malloc/malloc.h> /* malloc_size on macOS */
#endif
#include <stdlib.h>

static size_t used_memory = 0;

void *zmalloc(size_t size) {
    void *p = malloc(size);
    if (p) used_memory += malloc_size(p);
    return p;
}

void *zcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (p) used_memory += malloc_size(p);
    return p;
}

void *zrealloc(void *ptr, size_t size) {
    size_t old = ptr ? malloc_size(ptr) : 0;
    void *np = realloc(ptr, size);
    if (np) {
        used_memory -= old;
        used_memory += malloc_size(np);
    }
    return np;
}

void zfree(void *ptr) {
    if (!ptr) return;
    used_memory -= malloc_size(ptr);
    free(ptr);
}

size_t zmalloc_used(void) {
    return used_memory;
}
