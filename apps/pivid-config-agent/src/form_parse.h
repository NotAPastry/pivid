#ifndef AGENT_FORM_PARSE_H
#define AGENT_FORM_PARSE_H
#include "settings.h"

/* Apply a urlencoded form body onto `s` (caller pre-loads current values). Only
 * recognised schema keys are applied (via settings_set_field_str); unknown keys
 * and read-only keys are ignored. Returns the number of recognised keys seen. */
int form_parse_apply(const char *body, Settings *s);

/* URL-decode `src` into `dst` (size `cap`): %XX -> byte, '+' -> space. Returns
 * the decoded length, or -1 if it would overflow. */
int form_url_decode(const char *src, int srclen, char *dst, int cap);
#endif
