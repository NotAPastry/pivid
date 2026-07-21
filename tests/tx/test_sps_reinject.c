/* Host-side tests for apps/pivid-tx/src/sps_reinject.c.
 *
 * The module caches SPS (NAL type 7) / PPS (type 8) as they flow past and,
 * on a coded-slice NAL, re-injects the cached pair if interval_ns has elapsed
 * since the params were last sent -- so an intra-refresh (GDR, i_period=0)
 * stream stays re-acquirable on the one-way link without recurring IDRs.
 */

#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-tx/src/sps_reinject.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* Emit recorder. */
#define MAX_EMITS 16
#define MAX_EBYTES 512
static struct { uint8_t bytes[MAX_EBYTES]; size_t len; } g_emits[MAX_EMITS];
static size_t g_emit_count;
static void rec_reset(void) { g_emit_count = 0; }
static void rec_emit(const uint8_t *nal, size_t len, void *user) {
    (void)user;
    if (g_emit_count >= MAX_EMITS) return;
    size_t n = len < MAX_EBYTES ? len : MAX_EBYTES;
    memcpy(g_emits[g_emit_count].bytes, nal, n);
    g_emits[g_emit_count].len = len;
    g_emit_count++;
}

/* Build a NAL of given type with a payload byte tag so we can tell SPS/PPS apart. */
static size_t mknal(uint8_t *buf, uint8_t type, uint8_t tag, size_t len) {
    buf[0] = type & 0x1F;          /* nal_ref_idc 0 is fine for the test */
    for (size_t i = 1; i < len; i++) buf[i] = tag;
    return len;
}

#define NS_PER_S 1000000000ULL

int main(void) {
    uint8_t sps[32], pps[32], idr[64], pslice[64];
    size_t sps_n = mknal(sps, 7, 0xA7, 12);
    size_t pps_n = mknal(pps, 8, 0xB8, 6);
    size_t idr_n = mknal(idr, 5, 0xC5, 40);
    size_t ps_n  = mknal(pslice, 1, 0xD1, 40);

    /* 1. A coded slice before any SPS/PPS seen -> no re-injection. */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        rec_reset();
        sps_reinject_on_nal(&r, pslice, ps_n, 5 * NS_PER_S, rec_emit, NULL);
        EXPECT(g_emit_count == 0, "no params cached yet -> nothing re-injected");
    }

    /* 2. SPS+PPS cached, slice WITHIN the interval -> no re-injection. */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        rec_reset();
        sps_reinject_on_nal(&r, sps,    sps_n, 0,                 rec_emit, NULL);
        sps_reinject_on_nal(&r, pps,    pps_n, 0,                 rec_emit, NULL);
        sps_reinject_on_nal(&r, idr,    idr_n, 0,                 rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n,  NS_PER_S / 2,      rec_emit, NULL);
        EXPECT(g_emit_count == 0, "within interval -> no re-inject (params just sent)");
    }

    /* 3. SPS+PPS cached, slice AFTER the interval -> re-inject SPS then PPS. */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        rec_reset();
        sps_reinject_on_nal(&r, sps,    sps_n, 0,            rec_emit, NULL);
        sps_reinject_on_nal(&r, pps,    pps_n, 0,            rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n,  2 * NS_PER_S, rec_emit, NULL);
        EXPECT(g_emit_count == 2, "interval elapsed -> SPS+PPS re-injected before slice");
        EXPECT(g_emit_count >= 1 && (g_emits[0].bytes[0] & 0x1F) == 7, "first re-inject is SPS");
        EXPECT(g_emit_count >= 2 && (g_emits[1].bytes[0] & 0x1F) == 8, "second re-inject is PPS");
        EXPECT(g_emit_count >= 1 && g_emits[0].len == sps_n, "SPS bytes preserved");
        EXPECT(g_emit_count >= 2 && g_emits[1].len == pps_n, "PPS bytes preserved");
    }

    /* 4. Re-injection resets the timer: a second slice right after does NOT re-inject. */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        sps_reinject_on_nal(&r, sps, sps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pps, pps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n, 2 * NS_PER_S, rec_emit, NULL);  /* re-injects */
        rec_reset();
        sps_reinject_on_nal(&r, pslice, ps_n, 2 * NS_PER_S + 1000, rec_emit, NULL);
        EXPECT(g_emit_count == 0, "timer reset by re-inject -> next slice quiet");
    }

    /* 5. interval_ns == 0 disables re-injection entirely. */
    {
        SpsReinject r; sps_reinject_init(&r, 0);
        rec_reset();
        sps_reinject_on_nal(&r, sps, sps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pps, pps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n, 100 * NS_PER_S, rec_emit, NULL);
        EXPECT(g_emit_count == 0, "interval 0 -> disabled, never re-injects");
    }

    /* 6. Only SPS seen (no PPS) -> no re-injection (need both). */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        rec_reset();
        sps_reinject_on_nal(&r, sps, sps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n, 2 * NS_PER_S, rec_emit, NULL);
        EXPECT(g_emit_count == 0, "PPS missing -> no re-inject");
    }

    /* 7. An oversize SPS is not cached (can't re-inject a truncated param set). */
    {
        SpsReinject r; sps_reinject_init(&r, 1 * NS_PER_S);
        rec_reset();
        uint8_t big[SPS_REINJECT_PARAM_MAX + 64];
        size_t big_n = mknal(big, 7, 0xEE, sizeof big);
        sps_reinject_on_nal(&r, big, big_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pps, pps_n, 0, rec_emit, NULL);
        sps_reinject_on_nal(&r, pslice, ps_n, 2 * NS_PER_S, rec_emit, NULL);
        EXPECT(g_emit_count == 0, "oversize SPS not cached -> no re-inject");
    }

    if (g_failures == 0) printf("test_sps_reinject: all passed\n");
    else fprintf(stderr, "test_sps_reinject: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
