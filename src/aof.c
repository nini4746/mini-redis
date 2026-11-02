#include "aof.h"
#include "command.h"
#include "resp.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static long long last_fsync;
static int dirty;

static void write_all(const char *p, size_t len) {
    while (len > 0) {
        ssize_t n = write(server.aof_fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            /* Acknowledging writes we can't persist would hide data loss. */
            perror("FATAL: AOF write failed");
            exit(1);
        }
        p += n;
        len -= (size_t)n;
    }
    dirty = 1;
}

static void write_header(const char *prefix, size_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%s%zu\r\n", prefix, value);
    write_all(buf, (size_t)n);
}

void aof_append(int argc, const arg *argv) {
    if (server.aof_fd < 0) return;
    write_header("*", (size_t)argc);
    for (int i = 0; i < argc; i++) {
        write_header("$", argv[i].len);
        write_all(argv[i].data, argv[i].len);
        write_all("\r\n", 2);
    }
}

void aof_append_cstr(int argc, const char **argv, const size_t *lens) {
    if (server.aof_fd < 0) return;
    write_header("*", (size_t)argc);
    for (int i = 0; i < argc; i++) {
        write_header("$", lens[i]);
        write_all(argv[i], lens[i]);
        write_all("\r\n", 2);
    }
}

void aof_fsync_if_due(long long now_ms) {
    if (server.aof_fd < 0 || !dirty || now_ms - last_fsync < 1000) return;
    if (fsync(server.aof_fd) < 0) perror("AOF fsync failed");
    dirty = 0;
    last_fsync = now_ms;
}

int aof_open(const char *path) {
    server.aof_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (server.aof_fd < 0) { perror("open AOF"); return AOF_ERR; }
    return AOF_OK;
}

int aof_load(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? AOF_MISSING : AOF_ERR;

    client *c = client_new(-1);
    int result = AOF_OK;
    off_t total = 0;
    int eof = 0;
    server.aof_loading = 1;
    while (result == AOF_OK) {
        int rc = 0;
        if (c->rlen > 0) {
            if (c->rbuf[0] != '*') { result = AOF_ERR; break; }
            rc = resp_parse(c);
        }
        if (rc < 0) { result = AOF_ERR; break; }
        if (rc == 1) {
            command_dispatch(c);
            int rejected = c->wlen > 4 && memcmp(c->wbuf, "-ERR", 4) == 0;
            resp_free_argv(c);
            c->wlen = 0;
            c->wsent = 0;
            if (rejected) result = AOF_ERR; /* the log only holds commands we accepted */
            continue;
        }
        if (eof) break;
        client_reserve_read(c, 64 * 1024);
        ssize_t n = read(fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = AOF_ERR; break; }
        if (n == 0) eof = 1;
        c->rlen += (size_t)n;
        total += n;
    }
    server.aof_loading = 0;
    close(fd);

    if (result == AOF_OK && c->rlen > 0) {
        fprintf(stderr, "WARNING: cutting %zu bytes of a partial last command from %s\n",
                c->rlen, path);
        if (truncate(path, total - (off_t)c->rlen) < 0) result = AOF_ERR;
    }
    client_free(c);
    return result;
}
