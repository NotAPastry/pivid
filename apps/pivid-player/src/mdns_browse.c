/* apps/pivid-player/src/mdns_browse.c -- see mdns_browse.h. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE   /* unhide BSD multicast macros (IP_MULTICAST_TTL) */
#endif
#include "mdns_browse.h"
#include "mdns_parse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_MCAST_ADDR "224.0.0.251"
#define MDNS_MCAST_PORT 5353
#define REQUERY_MS      1000      /* re-send the query about once a second */
#define POLL_MS         250       /* recv slice -> stop responds within 250 ms */

/* Build a QU PTR query for "<service>.local". Returns the byte count, or 0 if it
 * would not fit. Labels are length-prefixed; no compression in our own query. */
static size_t build_query(uint8_t *buf, size_t cap, const char *service) {
    size_t n = 0;
    if (cap < 12) return 0;
    memset(buf, 0, 12);
    buf[5] = 1;                                  /* QDCOUNT = 1 (id/flags = 0) */
    n = 12;

    /* QNAME = labels of `service` then "local" then root. */
    char qname[80];
    snprintf(qname, sizeof qname, "%s.local", service);
    const char *s = qname;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || n + 1 + l > cap) return 0;
        buf[n++] = (uint8_t)l;
        memcpy(buf + n, s, l); n += l;
        s += l; if (dot) s++;
    }
    if (n + 5 > cap) return 0;
    buf[n++] = 0;                                /* root label */
    buf[n++] = 0; buf[n++] = 12;                 /* QTYPE  = PTR (12)          */
    buf[n++] = 0x80; buf[n++] = 0x01;            /* QCLASS = IN | QU (unicast) */
    return n;
}

static void send_query(int fd, const uint8_t *q, size_t qn) {
    struct sockaddr_in mc;
    memset(&mc, 0, sizeof mc);
    mc.sin_family = AF_INET;
    mc.sin_port = htons(MDNS_MCAST_PORT);
    inet_pton(AF_INET, MDNS_MCAST_ADDR, &mc.sin_addr);
    sendto(fd, q, qn, 0, (const struct sockaddr *)&mc, sizeof mc);
}

static void *run(void *arg) {
    MdnsBrowser *b = (MdnsBrowser *)arg;

    uint8_t q[128];
    size_t qn = build_query(q, sizeof q, b->service);
    if (qn == 0) return NULL;                    /* service name too long */

    MdnsAcc acc;
    memset(&acc, 0, sizeof acc);

    int since_query = REQUERY_MS;                /* force an immediate first query */
    while (!atomic_load(&b->stop)) {
        if (since_query >= REQUERY_MS) { send_query(b->fd, q, qn); since_query = 0; }

        struct pollfd p = { .fd = b->fd, .events = POLLIN };
        int rc = poll(&p, 1, POLL_MS);
        since_query += POLL_MS;
        if (rc <= 0 || !(p.revents & POLLIN)) continue;

        uint8_t pkt[2048];
        ssize_t got = recv(b->fd, pkt, sizeof pkt, 0);
        if (got <= 0) continue;

        if (mdns_parse_response(pkt, (size_t)got, b->service, &acc)) {
            snprintf(b->ip, sizeof b->ip, "%s", acc.ipv4);
            b->port = acc.port;
            atomic_store(&b->resolved, true);     /* publish: ip/port written first */
            break;                                /* one-shot: found it, stop browsing */
        }
    }
    return NULL;
}

int mdns_browser_start(MdnsBrowser *b, const char *service) {
    memset(b, 0, sizeof *b);
    snprintf(b->service, sizeof b->service, "%s", service);
    atomic_store(&b->stop, false);
    atomic_store(&b->resolved, false);
    b->fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("mdns socket"); return -1; }

    /* Explicit ephemeral bind BEFORE any send: deterministic source port for the
     * unicast replies (and the precondition for a future self-datagram wakeup). */
    struct sockaddr_in any;
    memset(&any, 0, sizeof any);
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&any, sizeof any) < 0) {
        perror("mdns bind"); close(fd); return -1;
    }
    unsigned char ttl = 1;                        /* LAN-local multicast */
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);

    b->fd = fd;
    if (pthread_create(&b->thread, NULL, run, b) != 0) {
        close(fd); b->fd = -1; return -1;
    }
    return 0;
}

bool mdns_browser_result(MdnsBrowser *b, char ip[16], uint16_t *port) {
    if (!atomic_load(&b->resolved)) return false;
    snprintf(ip, 16, "%s", b->ip);
    *port = b->port;
    return true;
}

void mdns_browser_stop(MdnsBrowser *b) {
    atomic_store(&b->stop, true);
    pthread_join(b->thread, NULL);                /* wakes within POLL_MS */
    if (b->fd >= 0) { close(b->fd); b->fd = -1; }
}
