#include "conn.h"
#include "zmalloc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

client *client_new(int fd) {
    client *c = zcalloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    return c;
}

void client_free(client *c) {
    if (!c) return;
    zfree(c->rbuf);
    zfree(c->wbuf);
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) zfree(c->argv[i].data);
        zfree(c->argv);
    }
    if (c->fd >= 0) close(c->fd);
    zfree(c);
}

void client_reserve_read(client *c, size_t extra) {
    size_t need = c->rlen + extra;
    if (need <= c->rcap) return;
    size_t cap = c->rcap ? c->rcap : 4096;
    while (cap < need) cap *= 2;
    c->rbuf = zrealloc(c->rbuf, cap);
    c->rcap = cap;
}

void reply_raw(client *c, const void *data, size_t len) {
    if (len == 0) return;
    /* If the buffer has been fully drained, reset offsets first. */
    if (c->wsent == c->wlen) {
        c->wsent = 0;
        c->wlen = 0;
    }
    size_t need = c->wlen + len;
    if (need > c->wcap) {
        size_t cap = c->wcap ? c->wcap : 4096;
        while (cap < need) cap *= 2;
        c->wbuf = zrealloc(c->wbuf, cap);
        c->wcap = cap;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
}
