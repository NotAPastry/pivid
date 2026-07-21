/* apps/pivid-player/src/control_client.c -- see control_client.h. */
#define _POSIX_C_SOURCE 200809L
#include "control_client.h"
#include "control.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KEEPALIVE_MS 2000

static void send_subscribe(ControlClient *c, const struct sockaddr_in *dst) {
    uint8_t buf[CASTER_CTRL_SUBSCRIBE_LEN];
    size_t n = caster_ctrl_build_subscribe(buf, sizeof buf, c->video_port, c->telem_port);
    if (n) sendto(c->fd, buf, n, 0, (const struct sockaddr *)dst, sizeof *dst);
}

static void *run(void *arg) {
    ControlClient *c = (ControlClient *)arg;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(c->control_port);
    inet_pton(AF_INET, c->caster_ip, &dst.sin_addr);

    while (!atomic_load(&c->stop)) {
        send_subscribe(c, &dst);
        /* Sleep KEEPALIVE_MS in short slices so stop is responsive. */
        for (int slept = 0; slept < KEEPALIVE_MS && !atomic_load(&c->stop); slept += 100) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    /* Clean exit: one UNSUBSCRIBE. */
    uint8_t ub[CASTER_CTRL_UNSUBSCRIBE_LEN];
    size_t n = caster_ctrl_build_unsubscribe(ub, sizeof ub);
    if (n) sendto(c->fd, ub, n, 0, (const struct sockaddr *)&dst, sizeof dst);
    return NULL;
}

int control_client_start(ControlClient *c, const char *caster_ip,
                         uint16_t control_port, uint16_t video_port,
                         uint16_t telem_port) {
    memset(c, 0, sizeof *c);
    snprintf(c->caster_ip, sizeof c->caster_ip, "%s", caster_ip);
    c->control_port = control_port;
    c->video_port = video_port;
    c->telem_port = telem_port;
    atomic_store(&c->stop, false);

    c->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c->fd < 0) { perror("control_client socket"); return -1; }
    struct in_addr probe;
    if (inet_pton(AF_INET, c->caster_ip, &probe) != 1) {
        fprintf(stderr, "control_client: invalid caster IP '%s'\n", c->caster_ip);
        close(c->fd); c->fd = -1; return -1;
    }
    if (pthread_create(&c->thread, NULL, run, c) != 0) {
        close(c->fd); c->fd = -1; return -1;
    }
    return 0;
}

void control_client_stop(ControlClient *c) {
    atomic_store(&c->stop, true);
    pthread_join(c->thread, NULL);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}
