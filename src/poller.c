#include "poller.h"

#ifdef __linux__

#include <stddef.h>
#include <sys/epoll.h>

int poller_create(void) {
    return epoll_create1(EPOLL_CLOEXEC);
}

static int ctl(int poller, int op, int fd, unsigned int mask) {
    struct epoll_event ev = { .events = mask, .data.fd = fd };
    return epoll_ctl(poller, op, fd, &ev);
}

int poller_add(int poller, int fd) {
    return ctl(poller, EPOLL_CTL_ADD, fd, EPOLLIN);
}

int poller_del(int poller, int fd, int write_enabled) {
    (void)write_enabled;
    return epoll_ctl(poller, EPOLL_CTL_DEL, fd, NULL);
}

int poller_set_write(int poller, int fd, int enable) {
    return ctl(poller, EPOLL_CTL_MOD, fd, enable ? EPOLLIN | EPOLLOUT : EPOLLIN);
}

int poller_wait(int poller, pollerEvent *events, int max, int timeout_ms) {
    struct epoll_event raw[max];
    int n = epoll_wait(poller, raw, max, timeout_ms);
    for (int i = 0; i < n; i++) {
        events[i].fd = raw[i].data.fd;
        /* HUP/ERR surface through read(), which then closes the client. */
        events[i].readable = (raw[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        events[i].writable = (raw[i].events & EPOLLOUT) != 0;
    }
    return n;
}

#else

#include <stdint.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

int poller_create(void) {
    return kqueue();
}

static int change(int poller, int fd, int16_t filter, uint16_t flags) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, flags, 0, 0, NULL);
    return kevent(poller, &ev, 1, NULL, 0, NULL);
}

int poller_add(int poller, int fd) {
    return change(poller, fd, EVFILT_READ, EV_ADD | EV_ENABLE);
}

int poller_del(int poller, int fd, int write_enabled) {
    if (write_enabled) change(poller, fd, EVFILT_WRITE, EV_DELETE);
    return change(poller, fd, EVFILT_READ, EV_DELETE);
}

int poller_set_write(int poller, int fd, int enable) {
    return change(poller, fd, EVFILT_WRITE, enable ? EV_ADD | EV_ENABLE : EV_DELETE);
}

int poller_wait(int poller, pollerEvent *events, int max, int timeout_ms) {
    struct kevent raw[max];
    struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
    int n = kevent(poller, NULL, 0, raw, max, &ts);
    for (int i = 0; i < n; i++) {
        events[i].fd = (int)raw[i].ident;
        events[i].readable = raw[i].filter == EVFILT_READ;
        events[i].writable = raw[i].filter == EVFILT_WRITE;
    }
    return n;
}

#endif
