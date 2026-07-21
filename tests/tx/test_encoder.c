/* Host-side tests for apps/pivid-tx/src/encoder.c.
 *
 * Strategy: link against mock_tx.c (recording stubs) instead of the real
 * tx.c. The encoder reads stdin from a pipe writable from the test, and
 * the wake_fd is a regular eventfd.
 */

#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-tx/src/encoder.h"
#include "../apps/pivid-tx/src/tx.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* From mock_tx.c. */
typedef struct {
    uint8_t  bytes[512];   /* must match mock_tx.c (511 B FLRC ceiling) */
    size_t   len;
} Mock_Call;
extern Mock_Call mock_calls[];
extern size_t    mock_call_count;
extern size_t    mock_attempt_count;
extern int       mock_fail_every_nth;
void mock_reset(void);

static int g_failures = 0;

/* Defaults matching the pre-refactor getenv fallbacks, so existing tests are
 * unchanged. */
static const EncoderConfig DEF_EC = {
    .fec_m_anchor = 5, .fec_m_ref = 0, .fec_k = 16, .sps_reinject_ms = 0
};

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* FIXED-mode outer header: flags(1) + len(u16-LE). */
static uint8_t  flags_of(const Mock_Call *c)  { return c->bytes[0]; }
static uint16_t length_of(const Mock_Call *c) { return unpack_le16(&c->bytes[1]); }
static const uint8_t *payload_of(const Mock_Call *c) { return &c->bytes[3]; }

/* --- NAL parser test recorder ----------------------------------- */
#define MAX_NAL_EMITS  16
#define MAX_NAL_BYTES  4096

typedef struct {
    uint8_t bytes[MAX_NAL_BYTES];
    size_t  len;
} Nal_Emit;

static Nal_Emit g_nal_emits[MAX_NAL_EMITS];
static int      g_nal_emit_count;

static void reset_nal_recorder(void) {
    memset(g_nal_emits, 0, sizeof(g_nal_emits));
    g_nal_emit_count = 0;
}

static void nal_recorder(const uint8_t *nal, size_t len, void *user) {
    (void)user;
    if (g_nal_emit_count >= MAX_NAL_EMITS) return;
    if (len > MAX_NAL_BYTES) len = MAX_NAL_BYTES;
    memcpy(g_nal_emits[g_nal_emit_count].bytes, nal, len);
    g_nal_emits[g_nal_emit_count].len = len;
    g_nal_emit_count++;
}

/* --- NAL parser cases ------------------------------------------------ */
static void test_nal_parser_single_nal(void) {
    reset_nal_recorder();
    Encoder_NalParser p;
    encoder_nal_parser_init(&p);

    /* Stream: 4-byte start code + 5-byte NAL payload + 4-byte start
     * code + 3-byte NAL payload. We expect TWO emits, one per NAL. */
    const uint8_t stream[] = {
        0x00, 0x00, 0x00, 0x01,  0x67, 0x42, 0x00, 0x29, 0xab,
        0x00, 0x00, 0x00, 0x01,  0x68, 0xce, 0x32,
    };
    encoder_nal_parser_feed(&p, stream, sizeof stream, nal_recorder, NULL);
    encoder_nal_parser_finish(&p, nal_recorder, NULL);

    EXPECT(g_nal_emit_count == 2, "two NALs emitted");
    EXPECT(g_nal_emits[0].len == 5, "first NAL is 5 bytes");
    EXPECT(g_nal_emits[0].bytes[0] == 0x67, "first NAL starts with 0x67");
    EXPECT(g_nal_emits[1].len == 3, "second NAL is 3 bytes");
    EXPECT(g_nal_emits[1].bytes[0] == 0x68, "second NAL starts with 0x68");
}

static void test_nal_parser_split_reads(void) {
    reset_nal_recorder();
    Encoder_NalParser p;
    encoder_nal_parser_init(&p);

    /* Same two-NAL stream, but fed in 3 chunks that intentionally
     * split the second start code across a read boundary. */
    const uint8_t chunk1[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00 };
    const uint8_t chunk2[] = { 0x29, 0xab, 0x00, 0x00 };          /* tail */
    const uint8_t chunk3[] = { 0x00, 0x01, 0x68, 0xce, 0x32 };    /* completes start code */

    encoder_nal_parser_feed(&p, chunk1, sizeof chunk1, nal_recorder, NULL);
    encoder_nal_parser_feed(&p, chunk2, sizeof chunk2, nal_recorder, NULL);
    encoder_nal_parser_feed(&p, chunk3, sizeof chunk3, nal_recorder, NULL);
    encoder_nal_parser_finish(&p, nal_recorder, NULL);

    EXPECT(g_nal_emit_count == 2, "split reads still yield two NALs");
    EXPECT(g_nal_emits[0].len == 5, "first NAL length intact across split");
    EXPECT(g_nal_emits[1].len == 3, "second NAL length intact across split");
    EXPECT(g_nal_emits[0].bytes[0] == 0x67, "first NAL byte preserved");
    EXPECT(g_nal_emits[1].bytes[0] == 0x68, "second NAL byte preserved");
}

static void test_nal_parser_embedded_zeros(void) {
    reset_nal_recorder();
    Encoder_NalParser p;
    encoder_nal_parser_init(&p);

    /* Single NAL with five 0x00 bytes mid-body followed by a non-
     * start-code byte. Parser must not chop the NAL into pieces. */
    const uint8_t stream[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0xff, 0x10,
    };
    encoder_nal_parser_feed(&p, stream, sizeof stream, nal_recorder, NULL);
    encoder_nal_parser_finish(&p, nal_recorder, NULL);

    EXPECT(g_nal_emit_count == 1, "embedded zeros stay in same NAL");
    EXPECT(g_nal_emits[0].len == 9, "NAL length is 9 bytes (incl. zeros)");
}

/* --- Helpers --------------------------------------------------------- */

static void make_pipe(int fds[2]) {
    if (pipe(fds) != 0) { perror("pipe"); exit(1); }
}

/* Write `n` bytes to `fd` -- short writes shouldn't happen on a freshly
 * created pipe with default 64 KB buffer, but be defensive. */
static void write_all(int fd, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) { perror("write"); exit(1); }
        off += (size_t)w;
    }
}

/* --- Case 1: single short read --------------------------------------- */
static void test_single_short_read(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    uint8_t payload[100];
    for (int i = 0; i < 100; i++) payload[i] = (uint8_t)i;
    write_all(pipe_fd[1], payload, 100);
    close(pipe_fd[1]);    /* EOF after the 100 bytes are read */

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "stdin EOF returns 1");
    EXPECT(mock_call_count == 1, "exactly one push");

    const Mock_Call *c = &mock_calls[0];
    uint8_t f = flags_of(c);
    EXPECT((f & PROTO_FLAG_START) != 0, "single chunk has START=1");
    EXPECT((f & PROTO_FLAG_END)   != 0, "single chunk has END=1");
    EXPECT(PROTO_SEQ_FROM_FLAGS(f) == 0, "SEQ=0 on first packet");
    EXPECT(length_of(c) == 100, "length byte is 100");
    EXPECT(memcmp(payload_of(c), payload, 100) == 0, "payload bytes match");

    close(pipe_fd[0]); close(wake);
}

/* --- Case 2: multi-chunk read (1000 bytes -> 4 chunks) --------------- */
static void test_multi_chunk_read(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    uint8_t payload[1000];
    for (int i = 0; i < 1000; i++) payload[i] = (uint8_t)(i & 0xFF);
    write_all(pipe_fd[1], payload, 1000);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF after multi-chunk read");
    /* 1000 bytes plain -> multi-fragment, so ch0 carries a 2-byte total-length
     * prefix: ch0 payload = [len u16][506 data] = 508; ch1 = 494 data.
     * 506 + 494 = 1000. */
    EXPECT(mock_call_count == 2, "two chunks pushed");

    EXPECT((flags_of(&mock_calls[0]) & PROTO_FLAG_START) != 0, "ch0 START");
    EXPECT((flags_of(&mock_calls[0]) & PROTO_FLAG_END)   == 0, "ch0 !END");
    EXPECT((flags_of(&mock_calls[1]) & PROTO_FLAG_START) == 0, "ch1 !START");
    EXPECT((flags_of(&mock_calls[1]) & PROTO_FLAG_END)   != 0, "ch1 END");

    EXPECT(length_of(&mock_calls[0]) == 508, "ch0 size 508 (2 prefix + 506 data)");
    EXPECT(length_of(&mock_calls[1]) == 494, "ch1 size 494 (1000 - 506)");
    EXPECT(unpack_le16(payload_of(&mock_calls[0])) == 1000, "ch0 carries total len 1000");

    /* Concatenated DATA (prefix stripped from ch0) equals input. */
    uint8_t recon[1000]; size_t off = 0;
    {
        const uint8_t *p0 = payload_of(&mock_calls[0]);
        memcpy(recon + off, p0 + 2, length_of(&mock_calls[0]) - 2);   /* skip 2-byte prefix */
        off += length_of(&mock_calls[0]) - 2;
        memcpy(recon + off, payload_of(&mock_calls[1]), length_of(&mock_calls[1]));
        off += length_of(&mock_calls[1]);
    }
    EXPECT(off == 1000, "reconstructed size 1000");
    EXPECT(memcmp(recon, payload, 1000) == 0, "reconstructed bytes match");

    /* SEQ counter increments per chunk, starting from 0. */
    for (int i = 0; i < 2; i++) {
        EXPECT(PROTO_SEQ_FROM_FLAGS(flags_of(&mock_calls[i])) == (uint8_t)i,
               "SEQ increments per chunk");
    }

    close(pipe_fd[0]); close(wake);
}

/* Writer-thread context: write `data` in two halves with a sleep between,
 * so the encoder definitely reads them as two separate read() calls. */
typedef struct {
    int            fd;
    const uint8_t *a; size_t a_len;
    const uint8_t *b; size_t b_len;
    long           gap_ms;
} Writer_Job;

static void *paced_writer(void *arg) {
    Writer_Job *j = arg;
    /* Slight delay so the encoder is in poll() when first chunk arrives. */
    struct timespec t = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
    nanosleep(&t, NULL);
    write_all(j->fd, j->a, j->a_len);
    t.tv_nsec = j->gap_ms * 1000 * 1000;
    nanosleep(&t, NULL);
    write_all(j->fd, j->b, j->b_len);
    close(j->fd);
    return NULL;
}

/* --- Case 3: two back-to-back reads, SEQ continues across read boundary - */
static void test_two_reads_seq_continues(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    /* Half-1 on the wire: [start-code] [500-byte payload A] [start-code]
     * Half-2 on the wire: [500-byte payload B]
     *
     * NAL1 (500 B of A) is completed by the trailing start code in
     * half-1 and pushed at half-1's end. NAL2 (500 B of B) is held
     * pending until EOF and pushed by encoder_nal_parser_finish.
     *
     * Both halves arrive in distinct encoder read()s (paced writer).
     * START/END now mark NAL boundaries; SEQ still continues across
     * them, which is the property this test cares about. */
    uint8_t half1[4 + 500 + 4];
    uint8_t half2[500];
    half1[0] = 0x00; half1[1] = 0x00; half1[2] = 0x00; half1[3] = 0x01;
    for (int i = 0; i < 500; i++) half1[4 + i] = (uint8_t)i;
    half1[504] = 0x00; half1[505] = 0x00; half1[506] = 0x00; half1[507] = 0x01;
    for (int i = 0; i < 500; i++) half2[i] = (uint8_t)(0x80 ^ i);

    Writer_Job job = { pipe_fd[1],
                       half1, sizeof half1,
                       half2, sizeof half2,
                       /*gap_ms=*/100 };
    pthread_t tid;
    pthread_create(&tid, NULL, paced_writer, &job);

    int rc = encoder_run(&e);
    pthread_join(tid, NULL);

    EXPECT(rc == 1, "EOF after two reads");

    /* Each NAL is 500 B -> ceil(500/508) = 1 chunk. 2 chunks total. */
    EXPECT(mock_call_count == 2, "2 chunks (2 NALs x 1 chunk)");

    /* SEQ runs 0,1 across NAL boundaries -- no reset. */
    for (int i = 0; i < 2; i++) {
        EXPECT(PROTO_SEQ_FROM_FLAGS(flags_of(&mock_calls[i])) == (uint8_t)i,
               "SEQ keeps incrementing across NAL boundaries");
    }

    /* Each single-chunk NAL carries both START and END. */
    EXPECT((flags_of(&mock_calls[0]) & PROTO_FLAG_START) != 0, "NAL1 START");
    EXPECT((flags_of(&mock_calls[0]) & PROTO_FLAG_END)   != 0, "NAL1 END");
    EXPECT((flags_of(&mock_calls[1]) & PROTO_FLAG_START) != 0, "NAL2 START");
    EXPECT((flags_of(&mock_calls[1]) & PROTO_FLAG_END)   != 0, "NAL2 END");

    close(pipe_fd[0]); close(wake);
}

/* --- Case 4: SEQ wraps after 16 packets ------------------------------- */
static void test_seq_wrap(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    /* Paced writer: two raw (no start code) halves form ONE NAL of 10160 B,
     * emitted at EOF. Multi-fragment START carries a 2-byte length prefix, so
     * ch0 holds 506 data bytes; the remaining 9654 split into ceil(9654/508)=19
     * full + 1 short = 20 continuations -> 21 chunks total. SEQ goes 0..15 then
     * wraps to 0..4. (508 = PLAIN_CHUNK_MAX = PROTOCOL_MAX_PAYLOAD.) */
    uint8_t a[5080], b[5080];
    for (size_t i = 0; i < sizeof a; i++) a[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof b; i++) b[i] = (uint8_t)(0xAA ^ i);
    Writer_Job job = { pipe_fd[1], a, sizeof a, b, sizeof b, /*gap_ms=*/100 };
    pthread_t tid;
    pthread_create(&tid, NULL, paced_writer, &job);

    int rc = encoder_run(&e);
    pthread_join(tid, NULL);

    EXPECT(rc == 1, "EOF after 21-chunk feed");
    EXPECT(mock_call_count == 21, "21 chunks pushed");

    for (size_t i = 0; i < 21; i++) {
        uint8_t expect_seq = (uint8_t)(i & 0x0F);
        EXPECT(PROTO_SEQ_FROM_FLAGS(flags_of(&mock_calls[i])) == expect_seq,
               "SEQ wraps mod 16");
    }

    close(pipe_fd[0]); close(wake);
}

/* --- Case 5: wake_fd interrupts a blocking read ----------------------- */
static void *wake_after_delay(void *arg) {
    int *fd_p = arg;
    /* Sleep ~50 ms, then signal. encoder_run should return promptly. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    uint64_t one = 1;
    (void)write(*fd_p, &one, sizeof one);
    return NULL;
}

static void test_wake_during_read(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    pthread_t tid;
    pthread_create(&tid, NULL, wake_after_delay, &wake);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = encoder_run(&e);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(tid, NULL);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    EXPECT(rc == 2, "wake_fd return = 2");
    EXPECT(elapsed_ms < 500, "encoder returns within 500 ms of wake");
    EXPECT(mock_call_count == 0, "no data was pushed");

    close(pipe_fd[0]); close(pipe_fd[1]); close(wake);
}

/* --- Case 6: EOF returns 1 -------------------------------------------- */
static void test_eof(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    /* Close writer immediately. */
    close(pipe_fd[1]);
    int rc = encoder_run(&e);
    EXPECT(rc == 1, "immediate EOF returns 1");
    EXPECT(mock_call_count == 0, "no chunks");

    close(pipe_fd[0]); close(wake);
}

/* --- Case 7: transient ring-full is RETRIED, not dropped ---------------
 * A full kernel ring is a microburst condition (IDR FEC flush): the kernel
 * drains a slot every ~2.5 ms, so the producer must wait briefly and retry
 * instead of silently discarding the packet (silent tail-slice loss was the
 * bottom-half-corruption bug, 2026-07-05 RCA). */
static void test_ring_full_transient_retried(void) {
    mock_reset();
    mock_fail_every_nth = 3;     /* every 3rd ATTEMPT fails (transient) */
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    /* 9*508 bytes -> multi-fragment: ch0 holds 506 data (2-byte prefix), the
     * remaining 4066 split into ceil(4066/508)=9 continuations -> 10 chunks.
     * The modulus is on ATTEMPTS, so attempts 3, 6, 9, 12 fail transiently;
     * each is retried on the next attempt, so ALL 10 chunks are delivered:
     * 10 successes + 4 retried fulls = 14 attempts, 0 drops. */
    uint8_t data[9 * 508];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)i;
    write_all(pipe_fd[1], data, sizeof data);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF after retry test");
    EXPECT(mock_call_count == 10, "all 10 chunks delivered despite transient fulls");
    EXPECT(dev.dropped_full == 0, "no packets dropped on transient full");
    EXPECT(mock_attempt_count == 14, "14 attempts (10 successes + 4 retried fulls)");

    close(pipe_fd[0]); close(wake);
}

/* --- Case 7b: sustained ring-full gives up after a bounded budget ------
 * If the ring never drains (radio wedge), the retry loop must not hang the
 * encoder: after TX_PUSH_TRIES attempts + one final counted push it drops
 * the packet and moves on, and dropped_full records the real loss. */
static void test_ring_full_sustained_bounded(void) {
    mock_reset();
    mock_fail_every_nth = 1;     /* every attempt fails (sustained full) */
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    uint8_t data[100];           /* one plain chunk (0x41 = P slice) */
    memset(data, 0x41, sizeof data);
    write_all(pipe_fd[1], data, sizeof data);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF after sustained-full test");
    EXPECT(mock_call_count == 0, "nothing delivered on sustained full");
    EXPECT(dev.dropped_full == 1, "exactly one packet counted as dropped");
    /* 25 bounded retries + 1 final counted attempt = 26. */
    EXPECT(mock_attempt_count == 26, "gave up after the bounded retry budget");

    close(pipe_fd[0]); close(wake);
}


/* --- Filler NAL (frame trailer) IS transmitted, padded to the FLRC min ----
 * The --frame-trailer filler rides the air as the player's AU-end marker
 * (au_asm closes each frame on it, IMP-1). rpicam emits a 2-byte runt (NAL
 * header + rbsp stop); as a PROTO_MODE_FIXED outer packet that is 5 bytes,
 * one below the LR2021 FLRC 6-byte minimum (DS 18.2.2). The encoder pads the
 * filler to 3 payload bytes so the outer packet is >= 6, keeping the on-air
 * length exact (no driver-side pad that would desync the RX in-band framer).
 * Transmitting the runt was unsafe until the RX coalesced-RX_DONE misframing
 * fix (lr2021_rx_walk.h, RCA 2026-07-05). */
static void test_filler_nal_padded_and_transmitted(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    encoder_init(&e, &dev, pipe_fd[0], wake, &DEF_EC);

    /* slice (type 1) + trailer (type 12 filler) + next-frame slice: all three
     * NALs now reach the ring (the trailer as the AU-end marker). */
    uint8_t stream[] = {
        0,0,0,1, 0x41, 0x80, 0xAA, 0xBB,   /* P slice, 4 bytes    */
        0,0,0,1, 0x0C, 0x80,               /* filler trailer (2 B) */
        0,0,0,1, 0x41, 0x80, 0xCC, 0xDD,   /* next frame's slice  */
    };
    write_all(pipe_fd[1], stream, sizeof stream);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF after filler test");
    EXPECT(mock_call_count == 3, "all three NALs (incl. trailer) were pushed");

    /* Exactly one pushed packet carries a filler NAL (type 12) at the payload
     * head (after the 3-byte FIXED header flags+LE16). It must be padded so the
     * whole outer packet is >= 6 bytes (FLRC minimum) and remain a well-formed
     * filler NAL: header 0x0C, then 0xFF ff_byte(s), then the 0x80 rbsp stop. */
    int trailer_seen = 0;
    for (size_t i = 0; i < mock_call_count; i++) {
        if ((mock_calls[i].bytes[3] & 0x1F) != 12) continue;
        trailer_seen++;
        EXPECT(mock_calls[i].len >= 6, "trailer packet padded to >= 6 B on wire");
        /* payload = bytes[3..]; LE16 payload length at [1..2] == len-3 */
        size_t pay = (size_t)mock_calls[i].bytes[1] | ((size_t)mock_calls[i].bytes[2] << 8);
        EXPECT(pay == mock_calls[i].len - 3, "trailer LE16 length matches on-air");
        EXPECT(pay >= 3, "trailer NAL padded to >= 3 payload bytes");
        EXPECT(mock_calls[i].bytes[4] == 0xFF, "filler pad byte is an ff_byte");
        EXPECT(mock_calls[i].bytes[3 + pay - 1] == 0x80, "rbsp stop preserved");
    }
    EXPECT(trailer_seen == 1, "exactly one filler NAL on the wire");
    close(pipe_fd[0]); close(wake);
}

/* --- FEC integration: anchor NAL with FEC_M_ANCHOR=2 emits FEC packets --- */
static void test_encoder_fec_on(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    EncoderConfig ec = { .fec_m_anchor = 2, .fec_m_ref = 0, .fec_k = 16, .sps_reinject_ms = 0 };
    encoder_init(&e, &dev, pipe_fd[0], wake, &ec);

    /* Annex-B: 4-byte start code, then an ANCHOR NAL header (0x65, IDR),
     * then 500 payload bytes -> one anchor NAL through the FEC stage. */
    uint8_t stream[4 + 1 + 500];
    stream[0]=0; stream[1]=0; stream[2]=0; stream[3]=1; stream[4]=0x65;
    for (size_t i = 5; i < sizeof stream; i++) stream[i] = (uint8_t)(i*7);
    write_all(pipe_fd[1], stream, sizeof stream);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF returns 1");
    int fec_seen = 0, plain_seen = 0;
    for (size_t i = 0; i < mock_call_count; i++) {
        if (mock_calls[i].bytes[0] & PROTO_FLAG_FEC) fec_seen++;
        else plain_seen++;
    }
    EXPECT(fec_seen > 0, "fec_m_anchor=2: anchor NAL emits FEC-flagged packets");
    EXPECT(e.fec.fec_blocks == 1, "one FEC block flushed at EOF");
    close(pipe_fd[0]); close(wake);
}

/* --- FEC off: FEC_M_ANCHOR=0 -> no FEC flag, today's plain path --- */
static void test_encoder_fec_off(void) {
    mock_reset();
    int pipe_fd[2]; make_pipe(pipe_fd);
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TX_Dev dev = {0}; dev.fd = -1;
    Encoder e;
    EncoderConfig ec = { .fec_m_anchor = 0, .fec_m_ref = 0, .fec_k = 16, .sps_reinject_ms = 0 };
    encoder_init(&e, &dev, pipe_fd[0], wake, &ec);

    uint8_t stream[4 + 1 + 500];
    stream[0]=0; stream[1]=0; stream[2]=0; stream[3]=1; stream[4]=0x65;
    for (size_t i = 5; i < sizeof stream; i++) stream[i] = (uint8_t)(i*7);
    write_all(pipe_fd[1], stream, sizeof stream);
    close(pipe_fd[1]);

    int rc = encoder_run(&e);
    EXPECT(rc == 1, "EOF returns 1");
    int fec_seen = 0;
    for (size_t i = 0; i < mock_call_count; i++)
        if (mock_calls[i].bytes[0] & PROTO_FLAG_FEC) fec_seen++;
    EXPECT(fec_seen == 0, "fec_m_anchor=0: no FEC flag");
    close(pipe_fd[0]); close(wake);
}

static void test_encoder_set_fec_live(void) {
    extern void mock_reset(void);
    extern size_t mock_call_count;
    TX_Dev dev; tx_open(&dev, 0);
    Encoder e; encoder_init(&e, &dev, -1, -1, &DEF_EC);
    encoder_set_fec(&e, 4, 1, 8);       /* anchor m=4, ref m=1, k=8 */
    assert(e.fec.m_anchor == 4 && e.fec.m_ref == 1 && e.fec.k_bound == 8);
    /* seq_counter pointer must still target the encoder's own counter. */
    assert(e.fec.seq_counter == &e.seq_counter);
    /* Out-of-range is ignored. */
    encoder_set_fec(&e, 999, 1, 8);
    assert(e.fec.m_anchor == 4 && e.fec.m_ref == 1);
    printf("test_encoder_set_fec_live: OK\n");
}

/* --- Config path: encoder_init reads FEC/SPS from EncoderConfig, not env --- */
static void test_encoder_config_no_env(void) {
    /* ensure no FEC env leaks in */
    unsetenv("FEC_M_ANCHOR"); unsetenv("FEC_M_REF");
    unsetenv("FEC_K"); unsetenv("SPS_REINJECT_MS");
    EncoderConfig ec = { .fec_m_anchor = 9, .fec_m_ref = 2, .fec_k = 16, .sps_reinject_ms = 0 };
    Encoder e; TX_Dev dev = {0}; dev.fd = -1;
    encoder_init(&e, &dev, -1, -1, &ec);
    EXPECT(encoder_get_fec_anchor(&e) == 9, "fec anchor from cfg");
    EXPECT(encoder_get_fec_k(&e) == 16, "fec k from cfg");
    printf("test_encoder_config_no_env: %s\n", "OK");
}

int main(void) {
    test_single_short_read();
    test_multi_chunk_read();
    test_two_reads_seq_continues();
    test_seq_wrap();
    test_wake_during_read();
    test_eof();
    test_ring_full_transient_retried();
    test_ring_full_sustained_bounded();
    test_filler_nal_padded_and_transmitted();
    test_nal_parser_single_nal();
    test_nal_parser_split_reads();
    test_nal_parser_embedded_zeros();
    test_encoder_fec_on();
    test_encoder_fec_off();
    test_encoder_set_fec_live();
    test_encoder_config_no_env();
    if (g_failures) {
        fprintf(stderr, "test_encoder: %d failures\n", g_failures);
        return 1;
    }
    printf("test_encoder: all cases pass\n");
    return 0;
}
