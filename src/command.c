#include "command.h"
#include "resp.h"

#include <stdio.h>
#include <strings.h>
#include <string.h>

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

/* ---- table ---- */

static const command command_table[] = {
    {"PING", ping_cmd, -1},
    {"ECHO", echo_cmd, 2},
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
