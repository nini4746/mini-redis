#ifndef POLLER_H
#define POLLER_H

/* Readiness polling over the platform's native API: epoll on Linux,
 * kqueue on macOS/BSD. Level-triggered on both. */

typedef struct pollerEvent {
    int fd;
    int readable;
    int writable;
} pollerEvent;

int poller_create(void);
int poller_add(int poller, int fd);                     /* watch for reads */
int poller_del(int poller, int fd, int write_enabled);
int poller_set_write(int poller, int fd, int enable);
int poller_wait(int poller, pollerEvent *events, int max, int timeout_ms);

#endif
