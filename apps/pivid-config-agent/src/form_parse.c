#define _POSIX_C_SOURCE 200809L
#include "form_parse.h"
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int form_url_decode(const char *src, int srclen, char *dst, int cap) {
    int di = 0;
    for (int si = 0; si < srclen && src[si]; si++) {
        if (di >= cap - 1) return -1;
        char c = src[si];
        if (c == '+') { dst[di++] = ' '; }
        else if (c == '%' && si + 2 < srclen) {
            int hi = hexval(src[si + 1]), lo = hexval(src[si + 2]);
            if (hi < 0 || lo < 0) { dst[di++] = c; }
            else { dst[di++] = (char)((hi << 4) | lo); si += 2; }
        } else { dst[di++] = c; }
    }
    dst[di] = '\0';
    return di;
}

int form_parse_apply(const char *body, Settings *s) {
    int seen = 0;
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        int pairlen = amp ? (int)(amp - p) : (int)strlen(p);
        const char *eq = memchr(p, '=', pairlen);
        if (eq) {
            char key[64], rawval[256], val[256];
            int klen = (int)(eq - p);
            int vlen = pairlen - klen - 1;
            if (klen > 0 && klen < (int)sizeof(key) && vlen >= 0 && vlen < (int)sizeof(rawval)) {
                memcpy(key, p, klen); key[klen] = '\0';
                memcpy(rawval, eq + 1, vlen); rawval[vlen] = '\0';
                if (form_url_decode(rawval, vlen, val, sizeof val) >= 0) {
                    /* settings_set_field_str returns 0 for a recognised,
                     * settable key; -1 for unknown OR read-only -> not counted. */
                    if (settings_set_field_str(s, key, val) == 0) seen++;
                }
            }
        }
        if (!amp) break;
        p = amp + 1;
    }
    return seen;
}
