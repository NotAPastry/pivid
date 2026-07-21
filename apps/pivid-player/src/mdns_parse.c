/* apps/pivid-player/src/mdns_parse.c -- see mdns_parse.h.
 *
 * A small, total DNS-response reader. It walks the resource records of one mDNS
 * packet and harvests the two facts the player needs: the SRV (control port +
 * target host) and the A (IPv4 of that host). Everything is bounds-checked
 * against pkt+len; a malformed or truncated packet stops the walk and returns
 * the facts gathered so far. No casts of the wire buffer to wider integer types
 * (aarch64/Win alignment + strict-aliasing safe): 16-bit fields are assembled
 * byte-by-byte. */
#include "mdns_parse.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* DNS RR TYPEs / CLASS we care about. */
#define T_A    1
#define T_PTR  12
#define T_TXT  16
#define T_SRV  33
#define C_IN   1

/* Big-endian 16-bit read, alignment- and endian-safe (no uint8_t*->uint16_t*). */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* Case-insensitive "does haystack contain needle". Used to match an SRV's owner
 * instance (e.g. "PiVid._pivid._udp.local") against the service ("_pivid._udp"). */
static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (hay[i + j] && j < nl &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nl) return 1;
    }
    return 0;
}

/* Decode a (possibly compressed) DNS name at `off` into `out` (dotted, NUL-term).
 * Sets *next_off to the offset just past the name AS IT APPEARS AT `off` (after
 * the first compression pointer's 2 bytes, or after the terminating 0x00). Every
 * compression pointer must reference an offset strictly less than the pointer
 * byte's own position (RFC 1035: a prior occurrence) so offsets decrease
 * monotonically and loops are impossible; a hop cap backstops it. Returns 0 on
 * success, -1 on any malformation / overrun. */
static int decode_name(const uint8_t *pkt, size_t len, size_t off,
                       char *out, size_t outcap, size_t *next_off) {
    size_t pos = off, outlen = 0, hops = 0, next = 0;
    int jumped = 0;
    for (;;) {
        if (pos >= len) return -1;
        uint8_t b = pkt[pos];
        if ((b & 0xC0) == 0xC0) {                       /* compression pointer */
            if (pos + 1 >= len) return -1;
            size_t ptr = ((size_t)(b & 0x3F) << 8) | pkt[pos + 1];
            if (!jumped) { next = pos + 2; jumped = 1; }
            if (ptr >= pos) return -1;                  /* must point strictly back */
            if (++hops > 128) return -1;
            pos = ptr;
        } else if ((b & 0xC0) == 0x00) {                /* normal label */
            size_t l = b;
            if (l == 0) {                               /* end of name */
                if (!jumped) next = pos + 1;
                break;
            }
            pos++;
            if (pos + l > len) return -1;
            if (outlen) {                               /* '.' between labels */
                if (outlen + 1 >= outcap) return -1;
                out[outlen++] = '.';
            }
            if (outlen + l >= outcap) return -1;        /* leave room for NUL */
            memcpy(out + outlen, pkt + pos, l);
            outlen += l;
            pos += l;
        } else {
            return -1;                                  /* 0x40/0x80 reserved */
        }
    }
    out[outlen] = '\0';
    *next_off = next;
    return 0;
}

bool mdns_parse_response(const uint8_t *pkt, size_t len,
                         const char *service, MdnsAcc *acc) {
    if (len < 12) return acc->have_srv && acc->have_a;

    uint16_t qd = rd16(pkt + 4);
    size_t   rr = (size_t)rd16(pkt + 6) + rd16(pkt + 8) + rd16(pkt + 10);  /* AN+NS+AR */
    size_t   pos = 12;
    char     name[256];

    /* Skip the question section: name + QTYPE(2) + QCLASS(2). */
    for (uint16_t i = 0; i < qd; i++) {
        size_t next;
        if (decode_name(pkt, len, pos, name, sizeof name, &next) != 0) goto done;
        pos = next;
        if (pos + 4 > len) goto done;
        pos += 4;
    }

    for (size_t i = 0; i < rr; i++) {
        size_t next;
        if (decode_name(pkt, len, pos, name, sizeof name, &next) != 0) goto done;
        pos = next;
        if (pos + 10 > len) goto done;
        uint16_t type  = rd16(pkt + pos);
        uint16_t cls   = (uint16_t)(rd16(pkt + pos + 2) & 0x7FFF);   /* mask cache-flush */
        uint16_t rdlen = rd16(pkt + pos + 8);                       /* skip 4-byte TTL */
        size_t   rdata = pos + 10;
        if (rdata + rdlen > len) goto done;

        if (cls == C_IN && type == T_SRV) {
            /* owner `name` is the instance; accept only our service. */
            if ((!service || ci_contains(name, service)) && rdlen >= 7) {
                char target[256]; size_t tn;
                if (decode_name(pkt, len, rdata + 6, target, sizeof target, &tn) == 0) {
                    acc->port = rd16(pkt + rdata + 4);              /* prio,weight,PORT */
                    snprintf(acc->host, sizeof acc->host, "%s", target);
                    acc->have_srv = 1;
                    if (acc->a_owner[0] && ci_equal(acc->a_owner, acc->host))
                        acc->have_a = 1;
                }
            }
        } else if (cls == C_IN && type == T_A && rdlen == 4) {
            const uint8_t *ip = pkt + rdata;
            snprintf(acc->ipv4, sizeof acc->ipv4, "%u.%u.%u.%u",
                     ip[0], ip[1], ip[2], ip[3]);
            snprintf(acc->a_owner, sizeof acc->a_owner, "%s", name);
            if (acc->have_srv && ci_equal(acc->a_owner, acc->host))
                acc->have_a = 1;
        }
        (void)type; (void)T_PTR; (void)T_TXT;
        pos = rdata + rdlen;
    }

done:
    return acc->have_srv && acc->have_a;
}
