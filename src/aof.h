#ifndef AOF_H
#define AOF_H

#include "conn.h"

/* Append-only file: every state change is logged as a RESP command and
 * replayed at startup. Fsynced once a second, so a process crash loses
 * nothing and a power loss loses at most about a second of writes. */

#define AOF_OK 0
#define AOF_MISSING 1
#define AOF_ERR -1

/* Replay the file. A partial last command (crash during write) is cut off
 * with a warning; anything else malformed returns AOF_ERR. */
int aof_load(const char *path);

int aof_open(const char *path);
void aof_append(int argc, const arg *argv);
void aof_append_cstr(int argc, const char **argv, const size_t *lens);
void aof_fsync_if_due(long long now_ms);

#endif
