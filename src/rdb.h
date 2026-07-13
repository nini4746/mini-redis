#ifndef RDB_H
#define RDB_H

#include "db.h"

#define RDB_OK      0
#define RDB_ERR    -1  /* corrupt/truncated file: caller must refuse to run */
#define RDB_NOFILE  1  /* file absent: start with an empty DB */

/* Serialize the whole keyspace to path (atomic via temp file + rename). */
int rdb_save(redisDb *db, const char *path);

/* Load path into db. Returns RDB_OK, RDB_NOFILE, or RDB_ERR. */
int rdb_load(redisDb *db, const char *path);

#endif
