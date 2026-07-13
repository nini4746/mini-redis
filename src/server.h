#ifndef SERVER_H
#define SERVER_H

#include "conn.h"
#include "db.h"

#define SERVER_PORT 6380

typedef struct serverState {
    int kq;
    int listen_fd;
    client **clients;   /* indexed by fd */
    int clients_cap;
    redisDb *db;
    long long maxmemory;   /* 0 = unlimited (eviction disabled) */
    long long last_cron;   /* ms timestamp of last periodic tick */
} serverState;

extern serverState server;

/* Enable/disable interest in writability for a client's fd. */
void el_set_write(client *c, int enable);

#endif
