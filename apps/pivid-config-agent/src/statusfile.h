#ifndef AGENT_STATUSFILE_H
#define AGENT_STATUSFILE_H
#include <stddef.h>

#define STATUSFILE_MAX_KV 64
typedef struct { char key[48]; char val[48]; size_t n; } StatusMap;

/* Parse "key=value\n" lines from `text` into `m` (capacity STATUSFILE_MAX_KV;
 * extra lines ignored). Returns the count parsed. */
size_t statusfile_parse(const char *text, StatusMap m[], size_t cap);
/* Lookup; returns the value string or NULL. */
const char *statusfile_get(const StatusMap m[], size_t n, const char *key);
/* Read a whole file into buf (NUL-terminated). Returns 0 / -1. */
int statusfile_read(const char *path, char *buf, size_t cap);
#endif
