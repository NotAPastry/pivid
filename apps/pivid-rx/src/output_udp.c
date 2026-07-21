/* UDP output sink. Connected SOCK_DGRAM -- one send() per NAL.
 *
 * Datagram size: the largest NAL the reassembler emits is REASM_BUF_BYTES
 * (currently 65536). That exceeds the Pi's typical 1500-byte path MTU, so
 * the IP layer fragments large datagrams into ~44 fragments for the worst
 * case. UDP receivers tolerate fragment loss as "datagram dropped" -- VLC
 * and ffplay cope. Future optimization: chunk NALs into ~1400-byte payloads
 * with our own header if observed loss becomes problematic. */

#define _POSIX_C_SOURCE 200809L

#include "output.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int fd;
} UDP_State;

static int udp_write(Output_Sink *self, const uint8_t *nal, size_t len) {
    UDP_State *st = self->state;
    ssize_t n = send(st->fd, nal, len, MSG_NOSIGNAL);
    if (n < 0 || (size_t)n != len) {
        self->writes_failed++;
        return -1;
    }
    self->writes_ok++;
    return 0;
}

static void udp_close(Output_Sink *self) {
    UDP_State *st = self->state;
    if (st) {
        if (st->fd >= 0) close(st->fd);
        free(st);
        self->state = NULL;
    }
}

int output_udp_open(Output_Sink *out, const char *addr, int port) {
    memset(out, 0, sizeof(*out));

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket AF_INET DGRAM"); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "output_udp: invalid address '%s'\n", addr);
        close(fd);
        return -1;
    }

    if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect UDP");
        close(fd);
        return -1;
    }

    UDP_State *st = calloc(1, sizeof(*st));
    if (!st) { close(fd); return -1; }
    st->fd = fd;

    out->state = st;
    out->write = udp_write;
    out->close = udp_close;
    return 0;
}
