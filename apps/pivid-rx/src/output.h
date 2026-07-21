#ifndef RX_OUTPUT_H
#define RX_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct Output_Sink Output_Sink;
struct Output_Sink {
    void *state;
    int  (*write)(Output_Sink *self, const uint8_t *nal, size_t len);
    void (*close)(Output_Sink *self);

    /* Stats (sink-owned; main reads at heartbeat time). */
    uint64_t writes_ok;
    uint64_t writes_failed;
};

/* UDP sink. Opens an AF_INET DGRAM socket and connect()s to addr:port.
 * Returns 0 on success, -1 on socket()/inet_aton()/connect() failure. */
int output_udp_open(Output_Sink *out, const char *addr, int port);

/* GStreamer sink. Spawns
 *   gst-launch-1.0 -e fdsrc fd=0 ! h264parse config-interval=-1
 *     ! v4l2h264dec ! kmssink sync=false
 * with the child's stdin wired to a pipe we hold the write-end of.
 * See output_gst.c for the rationale on config-interval and sync=false.
 * Returns 0 on success, -1 on pipe/posix_spawn failure. */
int output_gst_open(Output_Sink *out);

#endif
