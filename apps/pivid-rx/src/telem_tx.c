/* apps/pivid-rx/src/telem_tx.c -- see telem_tx.h. */
#define _POSIX_C_SOURCE 200809L
#include "telem_tx.h"
#include "osd_grid.h"
#include "loopback.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TELEM_TX_PERIOD_NS  (50 * 1000 * 1000L)  /* 20 Hz */

static void *telem_tx_run(void *arg) {
    TelemTx *t = arg;
    while (!atomic_load_explicit(&t->stop, memory_order_relaxed)) {
        osd_grid_t grid;
        rx_grid_read(t->grid, &grid);
        uint8_t buf[OSD_GRID_WIRE_LEN];
        size_t n = osd_grid_wire_serialize(buf, sizeof buf, &grid);
        if (n) {
            ssize_t w = send(t->fd, buf, n, MSG_NOSIGNAL);
            if (w == (ssize_t)n) t->sends_ok++; else t->sends_failed++;
        }
        struct timespec d = { .tv_sec = 0, .tv_nsec = TELEM_TX_PERIOD_NS };
        nanosleep(&d, NULL);
    }
    return NULL;
}

int telem_tx_start(TelemTx *t, RxGrid *grid) {
    memset(t, 0, sizeof *t);
    t->grid = grid;
    t->fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("telem_tx socket"); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)LOOPBACK_TELEM_PORT);
    if (inet_pton(AF_INET, LOOPBACK_ADDR, &sa.sin_addr) != 1) {
        fprintf(stderr, "telem_tx: bad loopback addr\n"); close(fd); return -1;
    }
    if (connect(fd, (const struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("telem_tx connect"); close(fd); return -1;
    }
    t->fd = fd;
    atomic_store_explicit(&t->stop, 0, memory_order_relaxed);
    if (pthread_create(&t->th, NULL, telem_tx_run, t) != 0) {
        perror("telem_tx pthread_create"); close(fd); t->fd = -1; return -1;
    }
    t->started = 1;
    return 0;
}

void telem_tx_stop(TelemTx *t) {
    if (!t->started) return;
    atomic_store_explicit(&t->stop, 1, memory_order_relaxed);
    pthread_join(t->th, NULL);
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
    t->started = 0;
}
