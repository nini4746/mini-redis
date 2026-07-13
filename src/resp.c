#include "resp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESP_MAX_MULTIBULK  1048576   /* max number of args per command */
#define RESP_MAX_BULK_LEN   (512L * 1024 * 1024) /* max single bulk length */

/* Find "\r\n" starting at buf[from..len). Returns index of '\r' or -1. */
static long find_crlf(const char *buf, size_t len, size_t from) {
    for (size_t i = from; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return (long)i;
    }
    return -1;
}

/* Parse a base-10 (optionally negative) integer from buf[from..end).
 * Returns 0 on success (result in *out), -1 on malformed input. */
static int parse_int(const char *buf, size_t from, size_t end, long long *out) {
    if (from >= end) return -1;
    int neg = 0;
    size_t i = from;
    if (buf[i] == '-') { neg = 1; i++; }
    else if (buf[i] == '+') { i++; }
    if (i >= end) return -1;
    long long v = 0;
    for (; i < end; i++) {
        char ch = buf[i];
        if (ch < '0' || ch > '9') return -1;
        v = v * 10 + (ch - '0');
        if (v < 0) return -1; /* overflow guard */
    }
    *out = neg ? -v : v;
    return 0;
}

static void argv_ensure(client *c, int n) {
    if (n <= c->argv_cap) return;
    int cap = c->argv_cap ? c->argv_cap : 4;
    while (cap < n) cap *= 2;
    c->argv = realloc(c->argv, (size_t)cap * sizeof(arg));
    c->argv_cap = cap;
}

void resp_free_argv(client *c) {
    for (int i = 0; i < c->argc; i++) {
        free(c->argv[i].data);
        c->argv[i].data = NULL;
    }
    c->argc = 0;
}

int resp_parse(client *c) {
    const char *buf = c->rbuf;
    size_t len = c->rlen;
    if (len == 0) return 0;

    if (buf[0] != '*') return -1; /* only multibulk accepted */

    /* Parse *<count>\r\n */
    long crlf = find_crlf(buf, len, 1);
    if (crlf < 0) {
        /* No line terminator yet. If the header line already looks
         * unreasonably long, treat as protocol error, else wait. */
        if (len > 64) return -1;
        return 0;
    }
    long long count;
    if (parse_int(buf, 1, (size_t)crlf, &count) != 0) return -1;
    if (count <= 0 || count > RESP_MAX_MULTIBULK) {
        /* *0 and negative are not sent by redis-cli/benchmark. */
        if (count == 0) {
            /* Empty command: consume it and report nothing to run. */
            size_t consumed = (size_t)crlf + 2;
            memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
            c->rlen -= consumed;
            c->argc = 0;
            return 1; /* dispatcher will ignore argc==0 */
        }
        return -1;
    }

    size_t pos = (size_t)crlf + 2;
    argv_ensure(c, (int)count);

    /* Temporary holding of parsed args; only commit once the full
     * command has been assembled so incomplete input leaves rbuf intact. */
    arg *tmp = malloc((size_t)count * sizeof(arg));
    if (!tmp) return -1;
    long long parsed = 0;
    int rc = 0; /* 0=need more, 1=done, -1=error */

    for (long long i = 0; i < count; i++) {
        if (pos >= len) { rc = 0; goto cleanup; }
        if (buf[pos] != '$') { rc = -1; goto cleanup; }
        long c2 = find_crlf(buf, len, pos + 1);
        if (c2 < 0) {
            if (len - pos > 64) { rc = -1; goto cleanup; }
            rc = 0; goto cleanup;
        }
        long long blen;
        if (parse_int(buf, pos + 1, (size_t)c2, &blen) != 0) { rc = -1; goto cleanup; }
        if (blen < 0 || blen > RESP_MAX_BULK_LEN) { rc = -1; goto cleanup; }
        size_t data_start = (size_t)c2 + 2;
        size_t data_end = data_start + (size_t)blen;
        if (data_end + 2 > len) { rc = 0; goto cleanup; } /* need data + \r\n */
        if (buf[data_end] != '\r' || buf[data_end + 1] != '\n') { rc = -1; goto cleanup; }

        tmp[i].data = malloc((size_t)blen + 1);
        if (!tmp[i].data) { rc = -1; goto cleanup; }
        memcpy(tmp[i].data, buf + data_start, (size_t)blen);
        tmp[i].data[blen] = '\0';
        tmp[i].len = (size_t)blen;
        parsed++;
        pos = data_end + 2;
    }
    rc = 1;

cleanup:
    if (rc == 1) {
        /* Commit: move parsed args into client, consume from rbuf. */
        for (long long i = 0; i < count; i++) c->argv[i] = tmp[i];
        c->argc = (int)count;
        free(tmp);
        memmove(c->rbuf, c->rbuf + pos, c->rlen - pos);
        c->rlen -= pos;
        return 1;
    }
    /* Not committed: free anything we allocated. */
    for (long long i = 0; i < parsed; i++) free(tmp[i].data);
    free(tmp);
    return rc;
}

/* ---- reply helpers ---- */

void reply_simple(client *c, const char *s) {
    reply_raw(c, "+", 1);
    reply_raw(c, s, strlen(s));
    reply_raw(c, "\r\n", 2);
}

void reply_error(client *c, const char *s) {
    reply_raw(c, "-", 1);
    reply_raw(c, s, strlen(s));
    reply_raw(c, "\r\n", 2);
}

void reply_integer(client *c, long long n) {
    char buf[32];
    int k = snprintf(buf, sizeof(buf), ":%lld\r\n", n);
    reply_raw(c, buf, (size_t)k);
}

void reply_bulk(client *c, const void *p, size_t len) {
    char hdr[32];
    int k = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    reply_raw(c, hdr, (size_t)k);
    reply_raw(c, p, len);
    reply_raw(c, "\r\n", 2);
}

void reply_bulk_cstr(client *c, const char *s) {
    reply_bulk(c, s, strlen(s));
}

void reply_null(client *c) {
    reply_raw(c, "$-1\r\n", 5);
}

void reply_array_header(client *c, long long n) {
    char hdr[32];
    int k = snprintf(hdr, sizeof(hdr), "*%lld\r\n", n);
    reply_raw(c, hdr, (size_t)k);
}
