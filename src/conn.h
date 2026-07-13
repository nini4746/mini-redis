#ifndef CONN_H
#define CONN_H

#include <stddef.h>

/* A single command argument (binary-safe: length carried explicitly). */
typedef struct arg {
    char *data;
    size_t len;
} arg;

/* Client flags. */
#define CLIENT_CLOSE_AFTER_WRITE 1

/* Per-client connection state. */
typedef struct client {
    int fd;

    /* Read buffer: unparsed inbound bytes. */
    char *rbuf;
    size_t rlen;   /* bytes currently held */
    size_t rcap;   /* capacity */

    /* Write buffer: outbound bytes not yet fully written. */
    char *wbuf;
    size_t wlen;   /* total bytes queued */
    size_t wsent;  /* bytes already written */
    size_t wcap;

    int write_blocked; /* EVFILT_WRITE currently registered */
    int flags;

    /* Parsed argv for the command currently being dispatched. */
    arg *argv;
    int argc;
    int argv_cap;
} client;

client *client_new(int fd);
void client_free(client *c);

/* Append raw bytes to the write buffer (grows as needed). */
void reply_raw(client *c, const void *data, size_t len);

/* Grow read buffer to hold at least extra more bytes past rlen. */
void client_reserve_read(client *c, size_t extra);

#endif
