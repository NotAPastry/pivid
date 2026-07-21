/* apps/pivid-player/src/mdns_browse.h -- Layer B of the player's dependency-free
 * mDNS auto-discovery: a background browser thread that finds the caster.
 *
 * Sends a QU (unicast-response) PTR query for "<service>.local" to 224.0.0.251:5353
 * from an ephemeral port (so it never contends for the system-owned 5353), feeds
 * each reply to the pure parser in mdns_parse.{c,h}, and on the first complete
 * SRV+A publishes {ipv4, port}. The caller (main.c) polls mdns_browser_result()
 * each render frame and, once resolved, starts the control client.
 *
 * NOTE: the socket/thread layer follows the rest of the player's POSIX idiom
 * (int fd, <sys/socket.h>, pthread) -- identical to control_client / telem_recv /
 * video_decode. Making the player build on Windows is a whole-player socket-layer
 * effort, out of scope here; the PARSER (mdns_parse.c) is already portable. */
#ifndef MDNS_BROWSE_H
#define MDNS_BROWSE_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char         service[64];   /* e.g. "_pivid._udp"                          */
    int          fd;            /* UDP socket, -1 when not open                */
    atomic_bool  stop;          /* set by stop(), polled by the thread         */
    atomic_bool  resolved;      /* set true once ip/port are valid             */
    pthread_t    thread;
    char         ip[16];        /* result: caster IPv4 (valid once resolved)   */
    uint16_t     port;          /* result: caster control port                 */
} MdnsBrowser;

/* Start browsing for `service` (e.g. "_pivid._udp"). Spawns a background thread.
 * Returns 0 on success, -1 on socket/thread setup failure. */
int  mdns_browser_start(MdnsBrowser *b, const char *service);

/* If a caster has been resolved, copy its IPv4 into ip[16] and port into *port
 * and return true; otherwise return false (nothing written). Lock-free. */
bool mdns_browser_result(MdnsBrowser *b, char ip[16], uint16_t *port);

/* Signal the thread to stop, join it, and close the socket. */
void mdns_browser_stop(MdnsBrowser *b);

#endif
