#include "rdb.h"
#include "object.h"
#include "list.h"
#include "dict.h"
#include "util.h"
#include "zmalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RDB_MAGIC   "MRDB"
#define RDB_VERSION 1
#define OP_RECORD   0x01
#define OP_EOF      0xFF
#define RDB_MAX_LEN (512ULL * 1024 * 1024) /* sanity cap on any length field */

/* A small streaming context that keeps a rolling checksum of everything
 * read/written, so corruption is caught structurally and by digest. */
typedef struct rdbctx {
    FILE *f;
    uint64_t chk;
} rdbctx;

static void chk_update(rdbctx *c, const void *p, size_t n) {
    const unsigned char *b = p;
    uint64_t h = c->chk;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    c->chk = h;
}

/* ---- write side ---- */

static int wr(rdbctx *c, const void *buf, size_t n) {
    if (fwrite(buf, 1, n, c->f) != n) return -1;
    chk_update(c, buf, n);
    return 0;
}
static int wr_u8(rdbctx *c, uint8_t v) { return wr(c, &v, 1); }
static int wr_u32(rdbctx *c, uint32_t v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return wr(c, b, 4);
}
static int wr_u64(rdbctx *c, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    return wr(c, b, 8);
}
static int wr_str(rdbctx *c, const void *p, size_t len) {
    if (len > RDB_MAX_LEN) return -1;
    if (wr_u32(c, (uint32_t)len)) return -1;
    return wr(c, p, len);
}

static int save_object(rdbctx *c, robj *o) {
    switch (o->type) {
    case OBJ_STRING: {
        sstr *s = o->ptr;
        return wr_str(c, s->data, s->len);
    }
    case OBJ_LIST: {
        mlist *l = o->ptr;
        if (wr_u32(c, (uint32_t)list_len(l))) return -1;
        for (listNode *n = l->head; n; n = n->next)
            if (wr_str(c, n->value->data, n->value->len)) return -1;
        return 0;
    }
    case OBJ_SET: {
        dict *d = o->ptr;
        if (wr_u32(c, (uint32_t)dict_size(d))) return -1;
        dictIterator it; dict_iter_init(&it, d);
        dictEntry *e;
        while ((e = dict_iter_next(&it)))
            if (wr_str(c, e->key, e->klen)) return -1;
        return 0;
    }
    case OBJ_HASH: {
        dict *d = o->ptr;
        if (wr_u32(c, (uint32_t)dict_size(d))) return -1;
        dictIterator it; dict_iter_init(&it, d);
        dictEntry *e;
        while ((e = dict_iter_next(&it))) {
            if (wr_str(c, e->key, e->klen)) return -1;
            sstr *v = e->val;
            if (wr_str(c, v->data, v->len)) return -1;
        }
        return 0;
    }
    }
    return -1;
}

int rdb_save(redisDb *db, const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%d", path, (int)getpid());

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    rdbctx c = { f, 1469598103934665603ULL };

    long long now = mstime();
    int rc = 0;

    if (wr(&c, RDB_MAGIC, 4) || wr_u8(&c, RDB_VERSION)) { rc = -1; goto done; }

    dictIterator it; dict_iter_init(&it, db->dict);
    dictEntry *e;
    while ((e = dict_iter_next(&it))) {
        long long when = db_get_expire(db, e->key, e->klen);
        if (when >= 0 && now >= when) continue; /* already expired: skip */

        robj *o = e->val;
        if (wr_u8(&c, OP_RECORD)) { rc = -1; goto done; }
        if (when >= 0) {
            if (wr_u8(&c, 1) || wr_u64(&c, (uint64_t)when)) { rc = -1; goto done; }
        } else {
            if (wr_u8(&c, 0)) { rc = -1; goto done; }
        }
        if (wr_u8(&c, (uint8_t)o->type)) { rc = -1; goto done; }
        if (wr_str(&c, e->key, e->klen)) { rc = -1; goto done; }
        if (save_object(&c, o)) { rc = -1; goto done; }
    }
    if (wr_u8(&c, OP_EOF)) { rc = -1; goto done; }
    /* Footer: rolling checksum, written raw (not folded into itself). */
    {
        unsigned char b[8];
        for (int i = 0; i < 8; i++) b[i] = (unsigned char)(c.chk >> (8 * i));
        if (fwrite(b, 1, 8, f) != 8) { rc = -1; goto done; }
    }

done:
    if (fflush(f) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) { unlink(tmp); return -1; }
    /* Atomic replace: a crash mid-write leaves the previous file intact. */
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ---- read side ---- */

static int rd(rdbctx *c, void *buf, size_t n) {
    if (fread(buf, 1, n, c->f) != n) return -1;
    chk_update(c, buf, n);
    return 0;
}
static int rd_u8(rdbctx *c, uint8_t *out) { return rd(c, out, 1); }
static int rd_u32(rdbctx *c, uint32_t *out) {
    unsigned char b[4];
    if (rd(c, b, 4)) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}
static int rd_u64(rdbctx *c, uint64_t *out) {
    unsigned char b[8];
    if (rd(c, b, 8)) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
    *out = v;
    return 0;
}

/* Read a length-prefixed blob. Caller owns *out (zmalloc'd). */
static int rd_str(rdbctx *c, char **out, size_t *outlen) {
    uint32_t len;
    if (rd_u32(c, &len)) return -1;
    if (len > RDB_MAX_LEN) return -1;
    char *buf = zmalloc((size_t)len + 1);
    if (rd(c, buf, len)) { zfree(buf); return -1; }
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return 0;
}

static int load_object(rdbctx *c, uint8_t type, robj **out) {
    switch (type) {
    case OBJ_STRING: {
        char *d; size_t dl;
        if (rd_str(c, &d, &dl)) return -1;
        *out = obj_string(d, dl);
        zfree(d);
        return 0;
    }
    case OBJ_LIST: {
        uint32_t n;
        if (rd_u32(c, &n)) return -1;
        if (n > RDB_MAX_LEN) return -1;
        robj *o = obj_new(OBJ_LIST, list_new());
        mlist *l = o->ptr;
        for (uint32_t i = 0; i < n; i++) {
            char *d; size_t dl;
            if (rd_str(c, &d, &dl)) { obj_free(o); return -1; }
            list_push_tail(l, d, dl);
            zfree(d);
        }
        *out = o;
        return 0;
    }
    case OBJ_SET: {
        uint32_t n;
        if (rd_u32(c, &n)) return -1;
        if (n > RDB_MAX_LEN) return -1;
        robj *o = obj_new(OBJ_SET, dict_create(NULL));
        dict *d = o->ptr;
        for (uint32_t i = 0; i < n; i++) {
            char *m; size_t ml;
            if (rd_str(c, &m, &ml)) { obj_free(o); return -1; }
            dict_set(d, m, ml, NULL);
            zfree(m);
        }
        *out = o;
        return 0;
    }
    case OBJ_HASH: {
        uint32_t n;
        if (rd_u32(c, &n)) return -1;
        if (n > RDB_MAX_LEN) return -1;
        robj *o = obj_new(OBJ_HASH, dict_create(zfree));
        dict *d = o->ptr;
        for (uint32_t i = 0; i < n; i++) {
            char *fld; size_t fl; char *val; size_t vl;
            if (rd_str(c, &fld, &fl)) { obj_free(o); return -1; }
            if (rd_str(c, &val, &vl)) { zfree(fld); obj_free(o); return -1; }
            dict_set(d, fld, fl, sstr_new(val, vl));
            zfree(fld); zfree(val);
        }
        *out = o;
        return 0;
    }
    }
    return -1;
}

int rdb_load(redisDb *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return RDB_NOFILE;

    rdbctx c = { f, 1469598103934665603ULL };
    long long now = mstime();
    int rc = RDB_ERR;

    char magic[4];
    uint8_t ver;
    if (rd(&c, magic, 4) || memcmp(magic, RDB_MAGIC, 4) != 0) goto out;
    if (rd_u8(&c, &ver) || ver != RDB_VERSION) goto out;

    for (;;) {
        uint8_t op;
        if (rd_u8(&c, &op)) goto out;
        if (op == OP_EOF) break;
        if (op != OP_RECORD) goto out;

        uint8_t has_expire;
        uint64_t when = 0;
        if (rd_u8(&c, &has_expire)) goto out;
        if (has_expire > 1) goto out; /* must be 0 or 1 */
        if (has_expire) { if (rd_u64(&c, &when)) goto out; }

        uint8_t type;
        if (rd_u8(&c, &type)) goto out;
        if (type > OBJ_SET) goto out;

        char *key; size_t klen;
        if (rd_str(&c, &key, &klen)) goto out;

        robj *o = NULL;
        if (load_object(&c, type, &o)) { zfree(key); goto out; }

        /* Drop keys already expired at load time (data isn't restored). */
        if (has_expire && now >= (long long)when) {
            obj_free(o);
            zfree(key);
            continue;
        }
        db_set(db, key, klen, o);
        if (has_expire) db_set_expire(db, key, klen, (long long)when);
        zfree(key);
    }

    /* Verify footer checksum (stored raw, so read it without folding it in). */
    {
        unsigned char b[8];
        if (fread(b, 1, 8, f) != 8) goto out;
        uint64_t stored = 0;
        for (int i = 0; i < 8; i++) stored |= (uint64_t)b[i] << (8 * i);
        if (stored != c.chk) goto out;
    }
    rc = RDB_OK;

out:
    fclose(f);
    if (rc != RDB_OK) db_flush(db); /* don't leave a half-loaded DB behind */
    return rc;
}
