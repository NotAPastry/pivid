/* apps/pivid-player/src/mdns_parse.h -- pure mDNS response parser.
 *
 * Layer A of the player's dependency-free mDNS auto-discovery. This file
 * is PURE: no sockets, no threads, no SDL/ffmpeg -- just bytes in, facts out, so
 * it is unit-testable on the host against captured Avahi packets. The socket +
 * thread wrapper lives in mdns_browse.{c,h}.
 *
 * Extracts exactly what the player needs to connect: the caster's IPv4 (A record)
 * and the control port to SUBSCRIBE to (SRV record). TXT/AAAA are ignored. */
#ifndef MDNS_PARSE_H
#define MDNS_PARSE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Accumulates facts across one or more response packets. POD, no heap, so a
 * caller can zero-init it and feed packets until mdns_parse_response returns
 * true. Names are NUL-terminated C strings. */
typedef struct {
    char     host[256];     /* SRV target, e.g. "pivid-rx.local" ("" => unknown) */
    uint16_t port;          /* SRV port   (0 => unknown)                          */
    char     a_owner[256];  /* owner NAME of the most recent A record seen        */
    char     ipv4[16];      /* dotted-quad of that A record ("" => none seen yet) */
    int      have_srv;      /* host+port populated                                */
    int      have_a;        /* a_owner set AND host known AND a_owner==host        */
} MdnsAcc;

/* Merge any SRV/A facts for `service` (e.g. "_pivid._udp") from ONE mDNS response
 * packet (pkt, len) into *acc. Returns true once acc->have_srv && acc->have_a
 * (a complete IPv4 + port is available). Total and bounds-checked: a malformed or
 * truncated packet leaves *acc in its current state and never reads past pkt+len.
 * `service` filters which SRV to accept (its owner instance must contain it);
 * pass NULL to accept any SRV. */
bool mdns_parse_response(const uint8_t *pkt, size_t len,
                         const char *service, MdnsAcc *acc);

#endif
