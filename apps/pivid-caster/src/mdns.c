/* apps/pivid-caster/src/mdns.c -- see mdns.h.
 *
 * Standard libavahi-client entry-group registration with a threaded poll, so
 * the daemon's single poll() loop is never blocked by avahi. Best-effort: every
 * failure path logs and degrades to "not advertised". HW-verified. */
#include "mdns.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#include <stdio.h>
#include <string.h>

struct CasterMdns {
    AvahiThreadedPoll *poll;
    AvahiClient       *client;
    AvahiEntryGroup   *group;
    char               name[64];   /* stream/service name */
    uint16_t           port;
    int                width, height, fps;   /* advertised as TXT records */
    bool               want_publish;
};

/* Forward decl: (re)create the entry group with the current name/port. Must be
 * called with the threaded-poll lock held (or from an avahi callback). */
static void publish_locked(CasterMdns *m) {
    if (!m->client || avahi_client_get_state(m->client) != AVAHI_CLIENT_S_RUNNING)
        return;                       /* publish when the client goes RUNNING */
    if (!m->want_publish) return;

    if (!m->group)
        m->group = avahi_entry_group_new(m->client, NULL, NULL);
    if (!m->group) {
        fprintf(stderr, "pivid-caster: avahi_entry_group_new failed: %s\n",
                avahi_strerror(avahi_client_errno(m->client)));
        return;
    }
    if (!avahi_entry_group_is_empty(m->group)) return;   /* already published */

    /* TXT records: codec + the video geometry the player sizes its window from.
     * avahi copies these strings during add_service, so stack buffers are fine. */
    char txt_w[24], txt_h[24], txt_fps[24];
    snprintf(txt_w,   sizeof txt_w,   "width=%d",  m->width);
    snprintf(txt_h,   sizeof txt_h,   "height=%d", m->height);
    snprintf(txt_fps, sizeof txt_fps, "fps=%d",    m->fps);

    int rc = avahi_entry_group_add_service(
        m->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, 0,
        m->name, "_pivid._udp", NULL, NULL, m->port,
        "codec=h264", txt_w, txt_h, txt_fps, NULL);
    if (rc < 0) {
        fprintf(stderr, "pivid-caster: add_service failed: %s\n", avahi_strerror(rc));
        return;
    }
    rc = avahi_entry_group_commit(m->group);
    if (rc < 0)
        fprintf(stderr, "pivid-caster: group_commit failed: %s\n", avahi_strerror(rc));
}

static void client_cb(AvahiClient *client, AvahiClientState state, void *userdata) {
    CasterMdns *m = userdata;
    m->client = client;
    if (state == AVAHI_CLIENT_S_RUNNING)
        publish_locked(m);
    else if (state == AVAHI_CLIENT_FAILURE)
        fprintf(stderr, "pivid-caster: avahi client failure: %s\n",
                avahi_strerror(avahi_client_errno(client)));
}

CasterMdns *mdns_create(void) {
    CasterMdns *m = avahi_malloc(sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    m->poll = avahi_threaded_poll_new();
    if (!m->poll) { avahi_free(m); return NULL; }
    int err = 0;
    m->client = avahi_client_new(avahi_threaded_poll_get(m->poll),
                                 AVAHI_CLIENT_NO_FAIL, client_cb, m, &err);
    if (!m->client) {
        fprintf(stderr, "pivid-caster: avahi_client_new failed: %s\n", avahi_strerror(err));
        avahi_threaded_poll_free(m->poll);
        avahi_free(m);
        return NULL;
    }
    if (avahi_threaded_poll_start(m->poll) < 0) {
        avahi_client_free(m->client);
        avahi_threaded_poll_free(m->poll);
        avahi_free(m);
        return NULL;
    }
    return m;
}

void mdns_publish(CasterMdns *m, const char *stream_name, uint16_t ctrl_port,
                  int width, int height, int fps) {
    if (!m) return;
    avahi_threaded_poll_lock(m->poll);
    snprintf(m->name, sizeof m->name, "%s", stream_name && *stream_name ? stream_name : "PiVid");
    m->port = ctrl_port;
    m->width = width;
    m->height = height;
    m->fps = fps;
    m->want_publish = true;
    publish_locked(m);
    avahi_threaded_poll_unlock(m->poll);
}

void mdns_withdraw(CasterMdns *m) {
    if (!m) return;
    avahi_threaded_poll_lock(m->poll);
    m->want_publish = false;
    if (m->group) avahi_entry_group_reset(m->group);
    avahi_threaded_poll_unlock(m->poll);
}

void mdns_destroy(CasterMdns *m) {
    if (!m) return;
    if (m->poll) avahi_threaded_poll_stop(m->poll);
    if (m->client) avahi_client_free(m->client);   /* also frees the group */
    if (m->poll) avahi_threaded_poll_free(m->poll);
    avahi_free(m);
}
