/* apps/pivid-caster/src/mdns.h -- thin libavahi-client wrapper to advertise
 * the caster's _pivid._udp service when "Forward to PC" is ON. Daemon-only
 * (not linked into host tests). All functions are no-fail best-effort: an
 * avahi failure logs and leaves mDNS simply unadvertised -- local loopback
 * forwarding never depends on it. */
#ifndef CASTER_MDNS_H
#define CASTER_MDNS_H
#include <stdbool.h>
#include <stdint.h>

typedef struct CasterMdns CasterMdns;   /* opaque */

/* Create the avahi client + threaded poll (does NOT publish yet). Returns a
 * handle, or NULL on failure (caller continues without mDNS). */
CasterMdns *mdns_create(void);

/* Publish the _pivid._udp service on `ctrl_port` with the given human-readable
 * stream name. `width`/`height`/`fps` are advertised as TXT records so a player
 * can size its window from discovery alone (spec lists name/codec/WxH/fps/ctrl;
 * name is the service instance, ctrl is the SRV port, the rest are TXT).
 * Idempotent: if the service is already published this is a no-op -- v1 always
 * uses the same name + port + dims so there is nothing to refresh. (Live changes
 * would need an avahi_entry_group_reset() first; a follow-on.) */
void mdns_publish(CasterMdns *m, const char *stream_name, uint16_t ctrl_port,
                  int width, int height, int fps);

/* Withdraw the service if published (idempotent). */
void mdns_withdraw(CasterMdns *m);

/* Tear down the client + poll thread. */
void mdns_destroy(CasterMdns *m);

#endif
