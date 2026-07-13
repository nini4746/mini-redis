#include "server.h"
#include "resp.h"
#include "command.h"

#include <arpa/inet.h>
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
    server.clients = realloc(server.clients, (size_t)cap * sizeof(client *));
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

static int setup_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 512) < 0) {
        perror("listen"); close(fd); return -1;
    }
    if (set_nonblock(fd) < 0) { perror("fcntl"); close(fd); return -1; }
    return fd;
}

int main(void) {
    /* Ignore SIGPIPE so writes to dead sockets return EPIPE instead. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    server.db = db_create();

    server.listen_fd = setup_listener();
    if (server.listen_fd < 0) return 1;

    server.kq = kqueue();
    if (server.kq < 0) { perror("kqueue"); return 1; }

    kq_change(server.listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE);

    fprintf(stderr, "mini-redis listening on port %d\n", SERVER_PORT);

    struct kevent events[256];
    for (;;) {
        int n = kevent(server.kq, NULL, 0, events, 256, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("kevent");
            break;
        }
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
