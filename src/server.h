#ifndef SERVER_H
#define SERVER_H

#include "conn.h"
#include "db.h"

#include <sys/types.h>

#define SERVER_PORT 6380

typedef struct serverState {
    int poller;
    int port;           /* listen port; defaults to SERVER_PORT, overridable via --port */
    int listen_fd;
    client **clients;   /* indexed by fd */
    int clients_cap;
    redisDb *db;
    long long maxmemory;   /* 0 = unlimited (eviction disabled) */
    long long last_cron;   /* ms timestamp of last periodic tick */
    pid_t rdb_child_pid;   /* -1 when no BGSAVE in flight */
    const char *rdb_filename;
    int aof_enabled;
    int aof_fd;            /* -1 when AOF is off */
    int aof_loading;       /* replaying: don't log commands again */
    const char *aof_filename;
} serverState;

extern serverState server;

/* Enable/disable interest in writability for a client's fd. */
void el_set_write(client *c, int enable);

/* Fork a background save. Returns 0 started, -1 fork failed,
 * 1 a BGSAVE is already in progress. */
int server_start_bgsave(void);

#endif
