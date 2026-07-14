#include "server.h"
#include "zmalloc.h"
#include "resp.h"
#include "command.h"
#include "dict.h"
#include "util.h"
#include "rdb.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

serverState server;

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void clients_ensure(int fd) {
    if (fd < server.clients_cap) return;
    int cap = server.clients_cap ? server.clients_cap : 64;
    while (cap <= fd) cap *= 2;
    server.clients = zrealloc(server.clients, (size_t)cap * sizeof(client *));
    for (int i = server.clients_cap; i < cap; i++) server.clients[i] = NULL;
    server.clients_cap = cap;
}

static void kq_change(int fd, int16_t filter, uint16_t flags) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, flags, 0, 0, NULL);
    kevent(server.kq, &ev, 1, NULL, 0, NULL);
}

void el_set_write(client *c, int enable) {
    if (enable && !c->write_blocked) {
        kq_change(c->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE);
        c->write_blocked = 1;
    } else if (!enable && c->write_blocked) {
        kq_change(c->fd, EVFILT_WRITE, EV_DELETE);
        c->write_blocked = 0;
    }
}

static void close_client(client *c) {
    int fd = c->fd;
    kq_change(fd, EVFILT_READ, EV_DELETE);
    if (c->write_blocked) kq_change(fd, EVFILT_WRITE, EV_DELETE);
    server.clients[fd] = NULL;
    client_free(c); /* also closes fd */
}

/* Flush queued write buffer. Returns 0 ok, -1 fatal error (client dead). */
static int flush_write(client *c) {
    while (c->wsent < c->wlen) {
        ssize_t n = write(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent);
        if (n > 0) {
            c->wsent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            el_set_write(c, 1); /* backpressure: finish later */
            return 0;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    /* Fully drained. */
    c->wsent = 0;
    c->wlen = 0;
    if (c->wcap > 65536) { /* release oversized reply buffer */
        zfree(c->wbuf);
        c->wbuf = NULL;
        c->wcap = 0;
    }
    el_set_write(c, 0);
    if (c->flags & CLIENT_CLOSE_AFTER_WRITE) return -1;
    return 0;
}

/* Parse & run everything currently buffered. */
static void process_input(client *c) {
    for (;;) {
        int rc = resp_parse(c);
        if (rc == 0) break; /* need more bytes */
        if (rc < 0) {
            reply_error(c, "ERR Protocol error");
            c->flags |= CLIENT_CLOSE_AFTER_WRITE;
            break;
        }
        command_dispatch(c);
        resp_free_argv(c);
    }
}

static void read_from_client(client *c) {
    for (;;) {
        client_reserve_read(c, 16 * 1024);
        ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n > 0) {
            c->rlen += (size_t)n;
            /* If the client filled the whole reservation, loop to drain
             * the socket; otherwise stop (avoids a spurious extra read). */
            if ((size_t)n < 16 * 1024) break;
        } else if (n == 0) {
            close_client(c); /* EOF */
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            close_client(c); /* RST or other error */
            return;
        }
    }
    process_input(c);
    /* Release an oversized read buffer once fully consumed, so a single
     * large request doesn't pin memory (and skew maxmemory) forever. */
    if (c->rlen == 0 && c->rcap > 65536) {
        zfree(c->rbuf);
        c->rbuf = NULL;
        c->rcap = 0;
    }
    if (flush_write(c) < 0) close_client(c);
}

static void accept_clients(void) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(server.listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (set_nonblock(fd) < 0) { close(fd); continue; }
        clients_ensure(fd);
        client *c = client_new(fd);
        if (!c) { close(fd); continue; }
        server.clients[fd] = c;
        kq_change(fd, EVFILT_READ, EV_ADD | EV_ENABLE);
    }
}

/* Active expiry: sample random keys from the expire table and drop the
 * ones already past their deadline. If a sampled batch was heavily
 * expired, keep going (bounded by a time budget) so bursts of expirations
 * are reclaimed without a full scan. */
static void active_expire_cycle(void) {
    redisDb *db = server.db;
    long long start = mstime();
    int loops = 0;
    do {
        if (dict_size(db->expires) == 0) return;
        int sampled = 0, expired = 0;
        long long now = mstime();
        for (int i = 0; i < 20; i++) {
            dictEntry *e = dict_random(db->expires);
            if (!e) break;
            sampled++;
            long long when = (long long)(intptr_t)e->val;
            if (now >= when) {
                /* Copy the key: db_delete frees the expire entry (and its
                 * key copy) before touching the keyspace dict. */
                size_t kl = e->klen;
                char *k = zmalloc(kl ? kl : 1);
                memcpy(k, e->key, kl);
                db_delete(db, k, kl);
                zfree(k);
                expired++;
            }
        }
        if (sampled == 0) return;
        /* Stop once expirations fall below ~25% of the sample. */
        if (expired * 4 <= sampled) return;
        if (mstime() - start > 25) return; /* time cap: don't stall the loop */
    } while (++loops < 16);
}

int server_start_bgsave(void) {
    if (server.rdb_child_pid != -1) return 1; /* already running */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: drop all inherited sockets so it can never answer a client,
         * then dump the fork-time snapshot (COW) and exit. */
        close(server.listen_fd);
        close(server.kq);
        for (int fd = 0; fd < server.clients_cap; fd++)
            if (server.clients[fd]) close(fd);
        int ok = (rdb_save(server.db, server.rdb_filename) == 0);
        _exit(ok ? 0 : 1);
    }
    server.rdb_child_pid = pid;
    return 0;
}

/* Reap a finished BGSAVE child (non-blocking) so no zombie lingers. */
static void reap_bgsave(void) {
    if (server.rdb_child_pid == -1) return;
    int status;
    pid_t p = waitpid(server.rdb_child_pid, &status, WNOHANG);
    if (p == server.rdb_child_pid) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            fprintf(stderr, "Background saving terminated with success\n");
        else
            fprintf(stderr, "Background saving error\n");
        server.rdb_child_pid = -1;
    }
}

static void server_cron(void) {
    reap_bgsave();
    long long now = mstime();
    if (now - server.last_cron < 100) return; /* ~10Hz */
    server.last_cron = now;
    active_expire_cycle();
}

static int setup_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(server.port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 512) < 0) {
        perror("listen"); close(fd); return -1;
    }
    if (set_nonblock(fd) < 0) { perror("fcntl"); close(fd); return -1; }
    return fd;
}

/* Parse CLI args. Supports `--port N` / `-p N`; returns 0 on success,
 * -1 on a malformed/out-of-range value so main can exit non-zero. */
static int parse_args(int argc, char **argv) {
    server.port = SERVER_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a port number\n", argv[i]);
                return -1;
            }
            char *end;
            long p = strtol(argv[++i], &end, 10);
            if (*end != '\0' || p < 1 || p > 65535) {
                fprintf(stderr, "error: invalid port '%s' (1-65535)\n", argv[i]);
                return -1;
            }
            server.port = (int)p;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (parse_args(argc, argv) < 0) return 1;

    /* Ignore SIGPIPE so writes to dead sockets return EPIPE instead. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    server.db = db_create();
    server.rdb_child_pid = -1;
    server.rdb_filename = "dump.mrdb";

    /* Load a snapshot if present. A corrupt file is fatal (we refuse to
     * start rather than silently mask data loss with an empty DB). */
    int lr = rdb_load(server.db, server.rdb_filename);
    if (lr == RDB_ERR) {
        fprintf(stderr,
                "FATAL: RDB file '%s' is corrupt or truncated; refusing to start\n",
                server.rdb_filename);
        return 1;
    }
    if (lr == RDB_OK)
        fprintf(stderr, "Loaded %zu keys from %s\n",
                db_size(server.db), server.rdb_filename);

    server.listen_fd = setup_listener();
    if (server.listen_fd < 0) return 1;

    server.kq = kqueue();
    if (server.kq < 0) { perror("kqueue"); return 1; }

    kq_change(server.listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE);

    fprintf(stderr, "mini-redis listening on port %d\n", server.port);

    server.last_cron = mstime();

    struct kevent events[256];
    struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms tick */
    for (;;) {
        int n = kevent(server.kq, NULL, 0, events, 256, &ts);
        if (n < 0) {
            if (errno == EINTR) { server_cron(); continue; }
            perror("kevent");
            break;
        }
        server_cron();
        for (int i = 0; i < n; i++) {
            int fd = (int)events[i].ident;
            if (fd == server.listen_fd) {
                accept_clients();
                continue;
            }
            client *c = (fd < server.clients_cap) ? server.clients[fd] : NULL;
            if (!c) continue;
            if (events[i].filter == EVFILT_READ) {
                read_from_client(c);
            } else if (events[i].filter == EVFILT_WRITE) {
                if (flush_write(c) < 0) close_client(c);
            }
        }
    }
    return 0;
}
