#include "command.h"
#include "zmalloc.h"
#include "resp.h"
#include "server.h"
#include "object.h"
#include "list.h"
#include "dict.h"
#include "util.h"
#include "rdb.h"

#include <errno.h>
#include <stdint.h>
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
    int deny_oom;   /* command grows memory -> subject to eviction/OOM check */
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
        zfree(old);
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

/* ---- collections: shared helpers ---- */

/* Look up key for a write. If it exists with a different type, reply
 * WRONGTYPE and set *err. Returns the object (or NULL if absent). */
static robj *lookup_write_typed(client *c, arg *key, obj_type want, int *err) {
    *err = 0;
    robj *o = db_lookup_write(server.db, key->data, key->len);
    if (o && o->type != want) {
        reply_error(c,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        *err = 1;
        return NULL;
    }
    return o;
}

static robj *lookup_read_typed(client *c, arg *key, obj_type want, int *err) {
    *err = 0;
    robj *o = db_lookup(server.db, key->data, key->len);
    if (o && o->type != want) {
        reply_error(c,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        *err = 1;
        return NULL;
    }
    return o;
}

/* ---- list ---- */

static void push_cmd(client *c, int at_head) {
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_LIST, &err);
    if (err) return;
    if (!o) {
        o = obj_new(OBJ_LIST, list_new());
        db_set(server.db, c->argv[1].data, c->argv[1].len, o);
    }
    mlist *l = o->ptr;
    for (int i = 2; i < c->argc; i++) {
        if (at_head) list_push_head(l, c->argv[i].data, c->argv[i].len);
        else         list_push_tail(l, c->argv[i].data, c->argv[i].len);
    }
    reply_integer(c, (long long)list_len(l));
}

static void lpush_cmd(client *c) { push_cmd(c, 1); }
static void rpush_cmd(client *c) { push_cmd(c, 0); }

static void pop_cmd(client *c, int from_head) {
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_LIST, &err);
    if (err) return;
    if (!o) { reply_null(c); return; }
    mlist *l = o->ptr;
    sstr *v = from_head ? list_pop_head(l) : list_pop_tail(l);
    if (!v) { reply_null(c); return; }
    reply_bulk(c, v->data, v->len);
    zfree(v);
    if (list_len(l) == 0)
        db_delete(server.db, c->argv[1].data, c->argv[1].len);
}

static void lpop_cmd(client *c) { pop_cmd(c, 1); }
static void rpop_cmd(client *c) { pop_cmd(c, 0); }

static void lrange_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_LIST, &err);
    if (err) return;
    if (!o) { reply_array_header(c, 0); return; }
    mlist *l = o->ptr;
    long long len = (long long)list_len(l);

    char *e1, *e2;
    long long start = strtoll(c->argv[2].data, &e1, 10);
    long long stop  = strtoll(c->argv[3].data, &e2, 10);
    if (*e1 || *e2) {
        reply_error(c, "ERR value is not an integer or out of range");
        return;
    }
    if (start < 0) start = len + start;
    if (stop < 0)  stop = len + stop;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop || start >= len) { reply_array_header(c, 0); return; }

    reply_array_header(c, stop - start + 1);
    long long idx = 0;
    for (listNode *n = l->head; n; n = n->next, idx++) {
        if (idx < start) continue;
        if (idx > stop) break;
        reply_bulk(c, n->value->data, n->value->len);
    }
}

static void llen_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_LIST, &err);
    if (err) return;
    reply_integer(c, o ? (long long)list_len(o->ptr) : 0);
}

/* ---- hash ---- */

static void hset_cmd(client *c) {
    if ((c->argc % 2) != 0) { /* HSET key f v f v ... => even argc */
        reply_error(c, "ERR wrong number of arguments for 'HSET' command");
        return;
    }
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_HASH, &err);
    if (err) return;
    if (!o) {
        o = obj_new(OBJ_HASH, dict_create(zfree));
        db_set(server.db, c->argv[1].data, c->argv[1].len, o);
    }
    dict *h = o->ptr;
    long long added = 0;
    for (int i = 2; i + 1 < c->argc; i += 2) {
        sstr *v = sstr_new(c->argv[i+1].data, c->argv[i+1].len);
        added += dict_set(h, c->argv[i].data, c->argv[i].len, v);
    }
    reply_integer(c, added);
}

static void hget_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_HASH, &err);
    if (err) return;
    if (!o) { reply_null(c); return; }
    sstr *v = dict_get(o->ptr, c->argv[2].data, c->argv[2].len);
    if (!v) { reply_null(c); return; }
    reply_bulk(c, v->data, v->len);
}

static void hdel_cmd(client *c) {
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_HASH, &err);
    if (err) return;
    if (!o) { reply_integer(c, 0); return; }
    dict *h = o->ptr;
    long long removed = 0;
    for (int i = 2; i < c->argc; i++) {
        removed += dict_del(h, c->argv[i].data, c->argv[i].len);
    }
    if (dict_size(h) == 0)
        db_delete(server.db, c->argv[1].data, c->argv[1].len);
    reply_integer(c, removed);
}

static void hgetall_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_HASH, &err);
    if (err) return;
    if (!o) { reply_array_header(c, 0); return; }
    dict *h = o->ptr;
    reply_array_header(c, (long long)dict_size(h) * 2);
    dictIterator it;
    dict_iter_init(&it, h);
    dictEntry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        reply_bulk(c, e->key, e->klen);
        sstr *v = e->val;
        reply_bulk(c, v->data, v->len);
    }
}

/* ---- set ---- */

static void sadd_cmd(client *c) {
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_SET, &err);
    if (err) return;
    if (!o) {
        o = obj_new(OBJ_SET, dict_create(NULL));
        db_set(server.db, c->argv[1].data, c->argv[1].len, o);
    }
    dict *s = o->ptr;
    long long added = 0;
    for (int i = 2; i < c->argc; i++) {
        if (!dict_find(s, c->argv[i].data, c->argv[i].len)) {
            dict_set(s, c->argv[i].data, c->argv[i].len, NULL);
            added++;
        }
    }
    reply_integer(c, added);
}

static void srem_cmd(client *c) {
    int err;
    robj *o = lookup_write_typed(c, &c->argv[1], OBJ_SET, &err);
    if (err) return;
    if (!o) { reply_integer(c, 0); return; }
    dict *s = o->ptr;
    long long removed = 0;
    for (int i = 2; i < c->argc; i++) {
        removed += dict_del(s, c->argv[i].data, c->argv[i].len);
    }
    if (dict_size(s) == 0)
        db_delete(server.db, c->argv[1].data, c->argv[1].len);
    reply_integer(c, removed);
}

static void sismember_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_SET, &err);
    if (err) return;
    if (!o) { reply_integer(c, 0); return; }
    reply_integer(c, dict_find(o->ptr, c->argv[2].data, c->argv[2].len) ? 1 : 0);
}

static void smembers_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_SET, &err);
    if (err) return;
    if (!o) { reply_array_header(c, 0); return; }
    dict *s = o->ptr;
    reply_array_header(c, (long long)dict_size(s));
    dictIterator it;
    dict_iter_init(&it, s);
    dictEntry *e;
    while ((e = dict_iter_next(&it)) != NULL)
        reply_bulk(c, e->key, e->klen);
}

static void scard_cmd(client *c) {
    int err;
    robj *o = lookup_read_typed(c, &c->argv[1], OBJ_SET, &err);
    if (err) return;
    reply_integer(c, o ? (long long)dict_size(o->ptr) : 0);
}

/* ---- TTL ---- */

static void expire_cmd(client *c) {
    char *end;
    long long secs = strtoll(c->argv[2].data, &end, 10);
    if (*end) { reply_error(c, "ERR value is not an integer or out of range"); return; }

    robj *o = db_lookup(server.db, c->argv[1].data, c->argv[1].len);
    if (!o) { reply_integer(c, 0); return; }

    if (secs <= 0) {
        /* Non-positive expire deletes the key immediately. */
        db_delete(server.db, c->argv[1].data, c->argv[1].len);
        reply_integer(c, 1);
        return;
    }
    db_set_expire(server.db, c->argv[1].data, c->argv[1].len,
                  mstime() + secs * 1000);
    reply_integer(c, 1);
}

static void ttl_cmd(client *c) {
    robj *o = db_lookup(server.db, c->argv[1].data, c->argv[1].len);
    if (!o) { reply_integer(c, -2); return; }
    long long when = db_get_expire(server.db, c->argv[1].data, c->argv[1].len);
    if (when < 0) { reply_integer(c, -1); return; }
    long long ms = when - mstime();
    if (ms < 0) ms = 0;
    reply_integer(c, (ms + 999) / 1000); /* round up to whole seconds */
}

static void persist_cmd(client *c) {
    robj *o = db_lookup(server.db, c->argv[1].data, c->argv[1].len);
    if (!o) { reply_integer(c, 0); return; }
    reply_integer(c, db_remove_expire(server.db, c->argv[1].data, c->argv[1].len));
}

/* ---- CONFIG (maxmemory only) ---- */

static void config_cmd(client *c) {
    /* CONFIG GET <param> | CONFIG SET <param> <value> */
    if (strncasecmp(c->argv[1].data, "GET", c->argv[1].len) == 0 && c->argc == 3) {
        if (strncasecmp(c->argv[2].data, "maxmemory", c->argv[2].len) == 0) {
            char val[32];
            int n = snprintf(val, sizeof(val), "%lld", server.maxmemory);
            reply_array_header(c, 2);
            reply_bulk_cstr(c, "maxmemory");
            reply_bulk(c, val, (size_t)n);
        } else {
            /* Unknown parameter: empty result (keeps redis-benchmark happy). */
            reply_array_header(c, 0);
        }
        return;
    }
    if (strncasecmp(c->argv[1].data, "SET", c->argv[1].len) == 0 && c->argc == 4) {
        if (strncasecmp(c->argv[2].data, "maxmemory", c->argv[2].len) == 0) {
            long long bytes;
            if (parse_memory(c->argv[3].data, &bytes) != 0) {
                reply_error(c, "ERR Invalid argument for CONFIG SET 'maxmemory'");
                return;
            }
            server.maxmemory = bytes;
        }
        /* Silently accept other params so tooling isn't blocked. */
        reply_simple(c, "OK");
        return;
    }
    reply_error(c, "ERR Unknown CONFIG subcommand or wrong number of arguments");
}

/* ---- persistence ---- */

static void save_cmd(client *c) {
    if (rdb_save(server.db, server.rdb_filename) == 0)
        reply_simple(c, "OK");
    else
        reply_error(c, "ERR error saving DB on disk");
}

static void bgsave_cmd(client *c) {
    int rc = server_start_bgsave();
    if (rc == 0)
        reply_simple(c, "Background saving started");
    else if (rc == 1)
        reply_error(c, "ERR Background save already in progress");
    else
        reply_error(c, "ERR Can't save in background: fork failed");
}

/* ---- table ---- */

static const command command_table[] = {
    {"PING", ping_cmd, -1, 0},
    {"ECHO", echo_cmd, 2, 0},
    {"GET", get_cmd, 2, 0},
    {"SET", set_cmd, 3, 1},
    {"DEL", del_cmd, -2, 0},
    {"EXISTS", exists_cmd, -2, 0},
    {"INCR", incr_cmd, 2, 1},
    {"DBSIZE", dbsize_cmd, 1, 0},
    {"FLUSHALL", flushall_cmd, 1, 0},
    {"LPUSH", lpush_cmd, -3, 1},
    {"RPUSH", rpush_cmd, -3, 1},
    {"LPOP", lpop_cmd, 2, 0},
    {"RPOP", rpop_cmd, 2, 0},
    {"LRANGE", lrange_cmd, 4, 0},
    {"LLEN", llen_cmd, 2, 0},
    {"HSET", hset_cmd, -4, 1},
    {"HGET", hget_cmd, 3, 0},
    {"HDEL", hdel_cmd, -3, 0},
    {"HGETALL", hgetall_cmd, 2, 0},
    {"SADD", sadd_cmd, -3, 1},
    {"SREM", srem_cmd, -3, 0},
    {"SISMEMBER", sismember_cmd, 3, 0},
    {"SMEMBERS", smembers_cmd, 2, 0},
    {"SCARD", scard_cmd, 2, 0},
    {"EXPIRE", expire_cmd, 3, 0},
    {"TTL", ttl_cmd, 2, 0},
    {"PERSIST", persist_cmd, 2, 0},
    {"CONFIG", config_cmd, -2, 0},
    {"SAVE", save_cmd, 1, 0},
    {"BGSAVE", bgsave_cmd, -1, 0},
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

/* Rough estimate of extra bytes this command wants to add. */
static long long incoming_bytes(client *c) {
    long long s = 0;
    for (int i = 2; i < c->argc; i++) s += (long long)c->argv[i].len + 64;
    return s;
}

/* Approximate allkeys-LRU: evict keys (sampling 5, dropping the least
 * recently accessed) until used memory drops to target. Returns 1 if the
 * target was reached, 0 if the keyspace ran empty first. */
static int evict_until(long long target) {
    redisDb *db = server.db;
    while ((long long)zmalloc_used() > target) {
        dictEntry *best = NULL;
        long long best_lru = 0;
        for (int i = 0; i < 5; i++) {
            dictEntry *e = dict_random(db->dict);
            if (!e) break;
            robj *o = e->val;
            if (!best || o->lru < best_lru) { best = e; best_lru = o->lru; }
        }
        if (!best) return 0;
        size_t kl = best->klen;
        char *k = zmalloc(kl ? kl : 1);
        memcpy(k, best->key, kl);
        db_delete(db, k, kl);
        zfree(k);
    }
    return 1;
}

/* Enforce maxmemory before a memory-growing write. Returns 0 to proceed,
 * -1 if the write must be denied with OOM (already replied). */
static int check_oom(client *c, const command *cmd) {
    if (!cmd->deny_oom || server.maxmemory <= 0) return 0; /* eviction off */
    long long incoming = incoming_bytes(c);
    evict_until(server.maxmemory - incoming);
    if ((long long)zmalloc_used() + incoming > server.maxmemory) {
        reply_error(c,
            "OOM command not allowed when used memory > 'maxmemory'");
        return -1;
    }
    return 0;
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
    if (check_oom(c, cmd) != 0) return;
    cmd->handler(c);
}
