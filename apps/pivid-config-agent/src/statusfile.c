#define _POSIX_C_SOURCE 200809L
#include "statusfile.h"
#include <stdio.h>
#include <string.h>

size_t statusfile_parse(const char *text, StatusMap m[], size_t cap) {
    size_t n = 0;
    const char *line = text;
    while (line && *line && n < cap) {
        const char *eol = strchr(line, '\n');
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
        const char *eq = memchr(line, '=', linelen);
        if (eq) {
            size_t klen = (size_t)(eq - line);
            size_t vlen = linelen - klen - 1;
            if (klen < sizeof(m[n].key) && vlen < sizeof(m[n].val)) {
                memcpy(m[n].key, line, klen); m[n].key[klen] = '\0';
                memcpy(m[n].val, eq + 1, vlen); m[n].val[vlen] = '\0';
                m[n].n = n; n++;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    return n;
}

const char *statusfile_get(const StatusMap m[], size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(m[i].key, key) == 0) return m[i].val;
    return NULL;
}

int statusfile_read(const char *path, char *buf, size_t cap) {
    if (cap == 0) return -1;   /* else cap-1 wraps to SIZE_MAX below */
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t got = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[got] = '\0';
    return 0;
}
