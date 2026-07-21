/* Caster egress hardening: the fan-out path must not silently swallow
 * send failures nor head-of-line-block on a full SO_SNDBUF, and the ingest
 * sockets must absorb an IDR burst.
 *
 * Host-only manual check (binds the fixed caster ports, like test_caster_smoke)
 * -- NOT in TEST_TARGETS. Drives caster_step() synchronously: no threads, no
 * timing races. Run by hand. */
#define _POSIX_C_SOURCE 200809L
#include "caster.h"
#include "control.h"
#include "loopback.h"

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    CasterCtx c;
    assert(caster_open(&c, false) == 0);

    /* (1) The egress socket must be non-blocking: a full SO_SNDBUF during an
     * 8-way IDR fan-out must fail fast (EAGAIN/ENOBUFS, counted) instead of
     * blocking the single-threaded poll loop and stalling all ingest. */
    int fl = fcntl(c.fwd_fd, F_GETFL, 0);
    assert(fl >= 0);
    assert((fl & O_NONBLOCK) && "fwd_fd must be O_NONBLOCK");

    /* (2) The ingest sockets must have an enlarged receive buffer so a burst of
     * rx->caster datagrams (an 8-NAL IDR) is absorbed rather than dropped at
     * the socket before the poll loop drains it. The kernel clamps SO_RCVBUF to
     * net.core.rmem_max (so an absolute threshold isn't portable across hosts);
     * what we can assert everywhere is that the ingest buffer is strictly larger
     * than a fresh UDP socket's default -- i.e. the bump took effect. */
    int base = 0; socklen_t bl = sizeof base;
    int bfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(bfd >= 0);
    assert(getsockopt(bfd, SOL_SOCKET, SO_RCVBUF, &base, &bl) == 0);
    close(bfd);
    for (int fd = c.video_fd; ; fd = c.telem_fd) {
        int got = 0; socklen_t sl = sizeof got;
        assert(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &sl) == 0);
        assert(got > base && "ingest receive buffer must be bumped above default");
        if (fd == c.telem_fd) break;
    }

    /* (3) Telemetry fan-out must COUNT a failed send, not swallow it. The video
     * chunk path already counts chunk_send_fail; the telem path did not, so an
     * ENOBUFS/short telem send during a burst was invisible. Force a
     * deterministic send failure and assert the counter moves. */
    /* SUBSCRIBE from loopback so fan_out_verbatim has a destination. */
    int cfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(cfd >= 0);
    struct sockaddr_in ca; memset(&ca, 0, sizeof ca);
    ca.sin_family = AF_INET; ca.sin_port = htons((uint16_t)CASTER_CTRL_PORT);
    inet_pton(AF_INET, "127.0.0.1", &ca.sin_addr);
    assert(connect(cfd, (struct sockaddr *)&ca, sizeof ca) == 0);
    uint8_t sub[CASTER_CTRL_SUBSCRIBE_LEN];
    caster_ctrl_build_subscribe(sub, sizeof sub, 5000, 5001);
    assert(send(cfd, sub, sizeof sub, 0) == (ssize_t)sizeof sub);
    assert(caster_step(&c, 100, 1000) >= 1);   /* process SUBSCRIBE */

    /* Sabotage egress: point fwd_fd at a non-socket so sendto fails (ENOTSOCK)
     * every time, deterministically. */
    close(c.fwd_fd);
    c.fwd_fd = open("/dev/null", O_WRONLY);
    assert(c.fwd_fd >= 0);

    /* Inject one telem datagram; the fan-out send will fail. */
    int tfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(tfd >= 0);
    struct sockaddr_in ta; memset(&ta, 0, sizeof ta);
    ta.sin_family = AF_INET; ta.sin_port = htons((uint16_t)LOOPBACK_TELEM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &ta.sin_addr);
    assert(connect(tfd, (struct sockaddr *)&ta, sizeof ta) == 0);
    const uint8_t tel[] = { 0x01, 0x02, 0x03, 0x04 };
    assert(send(tfd, tel, sizeof tel, 0) == (ssize_t)sizeof tel);
    assert(caster_step(&c, 100, 1001) >= 1);   /* process telem -> failed fan-out */

    assert(c.telem_send_fail == 1 && "telem fan-out must count send failures");
    assert(c.telem_fwd == 0 && "a failed telem send is not a forward");

    close(cfd); close(tfd);
    caster_close(&c);
    printf("test_caster_egress PASS\n");
    return 0;
}
