/* apps/pivid-player/src/telem_recv.c -- see telem_recv.h. */
#define _POSIX_C_SOURCE 200809L
#include "telem_recv.h"
#include "osd_grid.h"
#include "player_clock.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *run(void *arg) {
    TelemRecv *r = (TelemRecv *)arg;
    uint8_t buf[OSD_GRID_WIRE_LEN];
    while (!atomic_load(&r->stop)) {
        struct pollfd p = { .fd = r->fd, .events = POLLIN };
        int rc = poll(&p, 1, 200);          /* 200 ms so stop is responsive */
        if (rc <= 0) continue;
        if (!(p.revents & POLLIN)) continue;
        ssize_t n = recv(r->fd, buf, sizeof buf, 0);
        if (n <= 0) continue;
        osd_grid_t grid;
        if (osd_grid_wire_deserialize(buf, (size_t)n, &grid) == 0)
            telem_slot_publish(r->slot, &grid, now_ns());
    }
    return NULL;
}

int telem_recv_start(TelemRecv *r, uint16_t telem_port, TelemSlot *slot) {
    memset(r, 0, sizeof *r);
    r->telem_port = telem_port;
    r->slot = slot;
    atomic_store(&r->stop, false);

    r->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (r->fd < 0) { perror("telem_recv socket"); return -1; }
    int one = 1;
    setsockopt(r->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(telem_port);     /* 0 => OS assigns an ephemeral port */
    if (bind(r->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("telem_recv bind"); close(r->fd); r->fd = -1; return -1;
    }
    /* Read back the actually-bound port (the OS-chosen one when telem_port==0)
     * so the caller can advertise it to the caster. */
    struct sockaddr_in bound; socklen_t blen = sizeof bound;
    if (getsockname(r->fd, (struct sockaddr *)&bound, &blen) == 0)
        r->telem_port = ntohs(bound.sin_port);
    if (pthread_create(&r->thread, NULL, run, r) != 0) {
        close(r->fd); r->fd = -1; return -1;
    }
    return 0;
}

void telem_recv_stop(TelemRecv *r) {
    atomic_store(&r->stop, true);
    pthread_join(r->thread, NULL);
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}
