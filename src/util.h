#ifndef UTIL_H
#define UTIL_H

/* Current wall-clock time in milliseconds. */
long long mstime(void);

/* Parse a Redis-style memory size ("1024", "1mb", "512kb", "1gb").
 * Returns 0 on success (result in *out), -1 on malformed input. */
int parse_memory(const char *s, long long *out);

#endif
