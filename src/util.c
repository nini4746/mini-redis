#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

long long mstime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int parse_memory(const char *s, long long *out) {
    char *end = NULL;
    double val = strtod(s, &end);
    if (end == s || val < 0) return -1;

    long long mult = 1;
    if (*end != '\0') {
        /* Case-insensitive unit suffix. */
        char unit[8];
        size_t k = 0;
        for (const char *p = end; *p && k < sizeof(unit) - 1; p++)
            unit[k++] = (char)tolower((unsigned char)*p);
        unit[k] = '\0';
        if      (strcmp(unit, "b") == 0)  mult = 1;
        else if (strcmp(unit, "k") == 0)  mult = 1000LL;
        else if (strcmp(unit, "kb") == 0) mult = 1024LL;
        else if (strcmp(unit, "m") == 0)  mult = 1000LL * 1000;
        else if (strcmp(unit, "mb") == 0) mult = 1024LL * 1024;
        else if (strcmp(unit, "g") == 0)  mult = 1000LL * 1000 * 1000;
        else if (strcmp(unit, "gb") == 0) mult = 1024LL * 1024 * 1024;
        else return -1;
    }
    *out = (long long)(val * (double)mult);
    return 0;
}
