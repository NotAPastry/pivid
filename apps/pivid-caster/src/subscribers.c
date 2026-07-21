/* apps/pivid-caster/src/subscribers.c -- see subscribers.h. */
#include "subscribers.h"
#include <arpa/inet.h>   /* htonl, INADDR_LOOPBACK */
#include <string.h>

void subs_init(SubTable *t) {
    memset(t, 0, sizeof *t);
}

/* An entry is a client INSTANCE: (ip, control-socket source port). */
static Subscriber *find(SubTable *t, uint32_t ip, uint16_t ctrl_port) {
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++)
        if (t->e[i].used && t->e[i].ip == ip && t->e[i].ctrl_port == ctrl_port)
            return &t->e[i];
    return NULL;
}

int subs_upsert(SubTable *t, uint32_t ip, uint16_t ctrl_port,
                uint16_t video_port, uint16_t telem_port, int64_t now_ms) {
    Subscriber *s = find(t, ip, ctrl_port);
    if (!s) {
        for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++)
            if (!t->e[i].used) { s = &t->e[i]; break; }
        if (!s) return -1;   /* full, instance is new -> reject */
        s->used = true;
        s->ip = ip;
    }
    s->ctrl_port   = ctrl_port;
    s->video_port  = video_port;
    s->telem_port  = telem_port;
    s->last_seen_ms = now_ms;
    return 0;
}

void subs_remove(SubTable *t, uint32_t ip, uint16_t ctrl_port) {
    Subscriber *s = find(t, ip, ctrl_port);
    if (s) memset(s, 0, sizeof *s);   /* clears `used` */
}

int subs_age_out(SubTable *t, int64_t now_ms, int64_t ttl_ms) {
    int removed = 0;
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++) {
        if (t->e[i].used && now_ms - t->e[i].last_seen_ms > ttl_ms) {
            memset(&t->e[i], 0, sizeof t->e[i]);
            removed++;
        }
    }
    return removed;
}

int subs_count(const SubTable *t) {
    int n = 0;
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++) if (t->e[i].used) n++;
    return n;
}

bool subs_src_allowed(uint32_t src_ip, bool forward_enabled) {
    if (forward_enabled) return true;
    return src_ip == htonl(INADDR_LOOPBACK);
}
