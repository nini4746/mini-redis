#include "command.h"
#include "resp.h"
#include "server.h"
#include "object.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

/* Reply WRONGTYPE and return 1 if o exists and is not of the wanted type. */
static int wrong_type(client *c, robj *o, obj_type want) {
    if (o && o->type != want) {
        reply_error(c,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        return 1;
    }
    return 0;
}

typedef void (*cmd_handler)(client *c);

typedef struct command {
    const char *name;
    cmd_handler handler;
    int arity;      /* exact arg count, or negative for "at least -arity" */
} command;

/* ---- handlers ---- */

static void ping_cmd(client *c) {
    if (c->argc == 1) {
        reply_simple(c, "PONG");
    } else {
        reply_bulk(c, c->argv[1].data, c->argv[1].len);
    }
}

static void echo_cmd(client *c) {
    reply_bulk(c, c->argv[1].data, c->argv[1].len);
}

/* ---- string / keyspace ---- */

static void get_cmd(client *c) {
    robj *o = db_lookup(server.db, c->argv[1].data, c->argv[1].len);
    if (!o) { reply_null(c); return; }
    if (wrong_type(c, o, OBJ_STRING)) return;
    sstr *s = o->ptr;
    reply_bulk(c, s->data, s->len);
}

static void set_cmd(client *c) {
    robj *o = obj_string(c->argv[2].data, c->argv[2].len);
    db_set(server.db, c->argv[1].data, c->argv[1].len, o);
    reply_simple(c, "OK");
}

static void del_cmd(client *c) {
    long long removed = 0;
    for (int i = 1; i < c->argc; i++) {
        if (db_delete(server.db, c->argv[i].data, c->argv[i].len)) removed++;
    }
    reply_integer(c, removed);
}

static void exists_cmd(client *c) {
    long long count = 0;
    for (int i = 1; i < c->argc; i++) {
        if (db_lookup(server.db, c->argv[i].data, c->argv[i].len)) count++;
    }
    reply_integer(c, count);
}

static void incr_cmd(client *c) {
    robj *o = db_lookup_write(server.db, c->argv[1].data, c->argv[1].len);
    if (o && wrong_type(c, o, OBJ_STRING)) return;

    long long val = 0;
    if (o) {
        sstr *s = o->ptr;
        /* Parse the whole string strictly as a base-10 integer. */
        char *end = NULL;
        errno = 0;
        if (s->len == 0 || s->len > 20) {
            reply_error(c, "ERR value is not an integer or out of range");
            return;
        }
        val = strtoll(s->data, &end, 10);
        if (errno != 0 || end != s->data + s->len) {
            reply_error(c, "ERR value is not an integer or out of range");
            return;
        }
    }
    val += 1;
    if (o) {
        /* Replace the value in place so any TTL on the key is preserved. */
        char b[32];
        int n = snprintf(b, sizeof(b), "%lld", val);
        sstr *old = o->ptr;
        o->ptr = sstr_new(b, (size_t)n);
        free(old);
    } else {
        db_set(server.db, c->argv[1].data, c->argv[1].len, obj_string_ll(val));
    }
    reply_integer(c, val);
}

static void dbsize_cmd(client *c) {
    reply_integer(c, (long long)db_size(server.db));
}

static void flushall_cmd(client *c) {
    db_flush(server.db);
    reply_simple(c, "OK");
}

/* ---- table ---- */

static const command command_table[] = {
    {"PING", ping_cmd, -1},
    {"ECHO", echo_cmd, 2},
    {"GET", get_cmd, 2},
    {"SET", set_cmd, 3},
    {"DEL", del_cmd, -2},
    {"EXISTS", exists_cmd, -2},
    {"INCR", incr_cmd, 2},
    {"DBSIZE", dbsize_cmd, 1},
    {"FLUSHALL", flushall_cmd, 1},
};

static const command *lookup_command(const char *name, size_t len) {
    size_t n = sizeof(command_table) / sizeof(command_table[0]);
    for (size_t i = 0; i < n; i++) {
        if (strlen(command_table[i].name) == len &&
            strncasecmp(command_table[i].name, name, len) == 0) {
            return &command_table[i];
        }
    }
    return NULL;
}

static int arity_ok(const command *cmd, int argc) {
    if (cmd->arity >= 0) return argc == cmd->arity;
    return argc >= -cmd->arity;
}

void command_dispatch(client *c) {
    if (c->argc == 0) return; /* empty multibulk: ignore */

    const command *cmd = lookup_command(c->argv[0].data, c->argv[0].len);
    if (!cmd) {
        char buf[256];
        snprintf(buf, sizeof(buf), "ERR unknown command '%.*s'",
                 (int)c->argv[0].len, c->argv[0].data);
        reply_error(c, buf);
        return;
    }
    if (!arity_ok(cmd, c->argc)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "ERR wrong number of arguments for '%s' command", cmd->name);
        reply_error(c, buf);
        return;
    }
    cmd->handler(c);
}
