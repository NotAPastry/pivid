/* OFFLINE INVESTIGATION SPIKE -- DYNAMIC video frame-offset disambiguation.
 *
 * Question: with outer video packets framed DYNAMIC ([flags][payload], 1-byte
 * header), can the kernel RX frame-offset decision (real data at offset 0 vs a
 * stray SPI byte at offset 1) be made with per-packet CERTAINTY across the real
 * distribution of video packets -- FEC packets AND plain/disposable packets?
 *
 * FIXED framing has a strong LE16 check (irq.c:74-81). DYNAMIC video has no LE16
 * for plain packets; FEC packets retain the 4-byte pair sub-header + the
 * self-describing inner length walk (fec_walk_lengths in fec_rx.c).
 *
 * This harness:
 *   1. Generates an authentic corpus via the REAL TX path (fec_tx.c) in both
 *      DYNAMIC and FIXED mode, capturing every on-air outer packet + its class.
 *   2. Implements a DYNAMIC disambiguator (FEC-strong + plain best-effort).
 *   3. Runs an exhaustive stray sweep: offset-0 (clean) and offset-1 (all 256
 *      possible stray bytes prepended), tallying correct/wrong/ambiguous per
 *      class.
 *   4. Repeats with the FIXED LE16 control.
 *
 * NOT production code. Build under tests/. Pure host build (clang/gcc).
 */
#include "../apps/pivid-tx/src/fec_tx.h"
#include "protocol.h"
#include "fec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- corpus --- */

#define MAXPKT 4096
#define PKTCAP 600           /* worst case packet < 511 + a couple of bytes */

enum cls {
    CLS_FEC_C1 = 0,          /* FEC packet, count=1 (tail symbol)          */
    CLS_FEC_C2,              /* FEC packet, count=2 (full pair)            */
    CLS_PLAIN_SINGLE,        /* plain single-fragment NAL (START|END)      */
    CLS_PLAIN_START,         /* plain multi-fragment START (START, !END)   */
    CLS_PLAIN_CONT,          /* plain continuation / END fragment          */
    NCLS
};
static const char *cls_name[NCLS] = {
    "FEC count=1 (tail)",
    "FEC count=2 (pair)",
    "plain single-frag",
    "plain multi-frag START",
    "plain continuation/END",
};

static uint8_t g_pkt[MAXPKT][PKTCAP];
static size_t  g_len[MAXPKT];
static int     g_cls[MAXPKT];
static int     g_n;

/* The TX push callback. We don't know the mode in here, so classify from the
 * raw on-air bytes assuming offset 0 (which is true at capture time). The first
 * byte is flags in BOTH modes; FEC presence + START/END live in flags. For FEC
 * the count lives in the pair sub-header, whose position differs by mode -- so
 * we capture mode in a global set just before each TX run. */
static proto_mode_t g_mode;

static void rec_push(const uint8_t *p, size_t n, void *u) {
    (void)u;
    if (g_n >= MAXPKT || n > PKTCAP) return;
    memcpy(g_pkt[g_n], p, n);
    g_len[g_n] = n;

    uint8_t flags = p[0];
    size_t hdroff = (g_mode == PROTO_MODE_DYNAMIC) ? 1u : 3u;  /* payload start */
    int klass;
    if (flags & PROTO_FLAG_FEC) {
        uint8_t bid, base, k, nn, count;
        proto_fec_pair_hdr_unpack(&p[hdroff], &bid, &base, &k, &nn, &count);
        klass = (count == 2) ? CLS_FEC_C2 : CLS_FEC_C1;
    } else {
        int start = (flags & PROTO_FLAG_START) != 0;
        int end   = (flags & PROTO_FLAG_END)   != 0;
        if (start && end)      klass = CLS_PLAIN_SINGLE;
        else if (start)        klass = CLS_PLAIN_START;
        else                   klass = CLS_PLAIN_CONT;
    }
    g_cls[g_n] = klass;
    g_n++;
}

static void reset(void) { g_n = 0; }

/* Build a NAL with a given header byte + size, fill with deterministic data. */
static void mk_nal(uint8_t *nal, size_t n, uint8_t hdr, unsigned seed) {
    nal[0] = hdr;
    for (size_t i = 1; i < n; i++) nal[i] = (uint8_t)(i * 7u + seed);
}

/* Feed a varied workload to a fresh FecTx in mode `mode`. Exercises all four
 * classes across several (m_anchor, m_ref, k_bound) configs and NAL sizes. */
static void gen_corpus(proto_mode_t mode) {
    static uint8_t nal[20000];
    struct { int ma, mr, kb; } cfgs[] = {
        { 9, 2, 16 }, { 5, 0, 16 }, { 2, 2, 4 },
    };
    /* IDR header 0x65 (anchor->FEC), ref-P 0x61 (FEC if m_ref>0 else plain),
     * disposable 0x01 (always plain), SPS 0x67 / PPS 0x68 (anchor->FEC). */
    size_t sizes[] = { 10, 40, 100, 200, 248, 249, 250, 300, 500, 600,
                       1000, 2000, 5000, 12000, 18000 };
    uint8_t headers[] = { 0x65 /*IDR*/, 0x61 /*ref-P*/, 0x01 /*DISP*/,
                          0x67 /*SPS*/, 0x68 /*PPS*/ };

    for (size_t c = 0; c < sizeof cfgs / sizeof cfgs[0]; c++) {
        for (size_t h = 0; h < sizeof headers / sizeof headers[0]; h++) {
            for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
                uint8_t seq = 0;
                FecTx f;
                fec_tx_init(&f, cfgs[c].ma, cfgs[c].mr, cfgs[c].kb,
                            &seq, mode, rec_push, NULL);
                mk_nal(nal, sizes[s], headers[h], (unsigned)(c * 31 + h * 7 + s));
                fec_tx_nal(&f, nal, sizes[s]);
                fec_tx_flush(&f);
            }
        }
        /* A SPS+PPS+IDR triple (merged paramset block + separate IDR block). */
        {
            uint8_t seq = 0; FecTx f;
            fec_tx_init(&f, cfgs[c].ma, cfgs[c].mr, cfgs[c].kb, &seq, mode, rec_push, NULL);
            mk_nal(nal, 40, 0x67, 1); fec_tx_nal(&f, nal, 40);
            mk_nal(nal, 12, 0x68, 2); fec_tx_nal(&f, nal, 12);
            mk_nal(nal, 800, 0x65, 3); fec_tx_nal(&f, nal, 800);
            fec_tx_flush(&f);
        }
    }
}

/* ------------------------------------------------ DYNAMIC disambiguator ---- */
/* Mirror of the RX FEC validation (fec_rx.c) applied at a candidate offset.
 * `buf`/`buflen` is the full received buffer; `o` is the candidate offset of the
 * flags byte. The DYNAMIC payload length at offset o is (buflen - o - 1). */

static int dyn_offset_valid(const uint8_t *buf, size_t buflen, size_t o) {
    if (o + 1 > buflen) return 0;          /* need flags byte */
    uint8_t flags = buf[o];

    /* Invariant for ALL video packets: TYPE bit (bit0) clear. */
    if (flags & FLAG_TYPE_MASK) return 0;

    size_t plen = buflen - o - 1;          /* DYNAMIC payload length */
    if (plen > PROTOCOL_MAX_PAYLOAD) return 0;
    const uint8_t *payload = buf + o + 1;

    if (flags & PROTO_FLAG_FEC) {
        /* FEC-strong path: 4-byte pair sub-header + inner length walk, exactly
         * as fec_rx_feed validates. */
        if (plen < FEC_HDR_BYTES) return 0;
        uint8_t bid, base, k, n, count;
        proto_fec_pair_hdr_unpack(payload, &bid, &base, &k, &n, &count);

        if (count < 1 || count > 2) return 0;
        if (k < 1 || k > n || n > (int)k + FEC_M_MAX) return 0;
        if (k > FEC_K_MAX || n > FEC_K_MAX + FEC_M_MAX) return 0;

        int ilv = (flags & PROTO_FLAG_FEC_ILV) != 0;
        if (!ilv && (int)base + (int)count > n) return 0;
        if (ilv) { if (count != 2) return 0; if (base >= n) return 0; }

        /* Inner length walk (mirror fec_walk_lengths): sum must == plen EXACTLY. */
        uint8_t idx0 = base;
        uint8_t idx1 = ilv ? base : (uint8_t)(base + 1);
        uint8_t idx[2] = { idx0, idx1 };
        size_t off = FEC_HDR_BYTES;
        for (uint8_t t = 0; t < count; t++) {
            size_t sl;
            if (idx[t] < k) {
                if (off + 3 > plen) return 0;
                sl = (size_t)3 + (size_t)unpack_le16(&payload[off + 1]);
                if (sl > FEC_SYMBOL_LEN) return 0;
            } else {
                sl = FEC_SYMBOL_LEN;
            }
            if (off + sl > plen) return 0;
            off += sl;
        }
        if (off != plen) return 0;
        return 1;
    }

    /* Plain (FEC clear) path: best-effort. The only structural fields are
     * START/END and (for multi-frag START) a 2-byte LE total-length prefix.
     *   - single-frag (START|END): nothing but bit0==0 -- inherently weak.
     *   - multi-frag START (START, !END): payload = [total_len LE16][chunk];
     *     total_len must be <= 0xFFFF (always true for u16) and >= the chunk
     *     carried, and the chunk must be <= PLAIN_CHUNK_MAX. The TX caps the
     *     first chunk at PROTOCOL_MAX_PAYLOAD-2, and total_len > that (else it
     *     wouldn't be multi-frag). So require total_len > (plen-2).
     *   - continuation/END (!START): nothing but bit0==0.
     * These checks are as strong as the wire format permits. */
    int start = (flags & PROTO_FLAG_START) != 0;
    int end   = (flags & PROTO_FLAG_END)   != 0;
    if (start && !end) {
        /* multi-frag START */
        if (plen < 3) return 0;            /* 2-byte len + >=1 chunk byte */
        size_t total = unpack_le16(payload);
        size_t chunk = plen - 2;
        if (total <= chunk) return 0;      /* multi-frag => total exceeds 1st chunk */
        if (total > 0xFFFF) return 0;      /* (u16, always ok) */
        return 1;
    }
    /* single-frag or continuation: only the bit0==0 invariant. */
    return 1;
}

/* Returns the chosen offset (0/1); sets *amb to 0 (decided), 1 (both valid --
 * a real collision), or 2 (neither valid -- falls back to sticky/learned). */
static int dyn_video_offset(const uint8_t *buf, size_t buflen, int *amb) {
    int v0 = dyn_offset_valid(buf, buflen, 0);
    int v1 = dyn_offset_valid(buf, buflen, 1);
    if (v0 && !v1) { *amb = 0; return 0; }
    if (v1 && !v0) { *amb = 0; return 1; }
    *amb = (v0 && v1) ? 1 : 2;             /* 1=both valid, 2=neither valid */
    return 0;
}

/* ------------------------------------------------ FIXED LE16 control ------- */
/* Replicate irq.c:74-81. pkt_len is the chip-reported length = the true packet
 * length (NOT including any stray; the chip reads pkt_len+1). For offset 0 the
 * buffer IS the packet (buflen == pkt_len). For offset 1 the buffer is
 * stray+packet (buflen == pkt_len+1), and pkt_len is still the true packet len.
 */
static int fixed_offset(const uint8_t *buf, size_t buflen, uint16_t pkt_len,
                        int *amb) {
    if (pkt_len < 4) { *amb = 1; return 0; }
    if (buflen < 4) { *amb = 1; return 0; }
    uint16_t want = (uint16_t)(pkt_len - 3);
    int v0 = ((uint16_t)(buf[1] | ((uint16_t)buf[2] << 8)) == want);
    int v1 = (buflen >= 5) &&
             ((uint16_t)(buf[2] | ((uint16_t)buf[3] << 8)) == want);
    if (v0 && !v1) { *amb = 0; return 0; }
    if (v1 && !v0) { *amb = 0; return 1; }
    *amb = 1; return 0;
}

/* ----------------------------------------------------------- sweep --------- */

struct tally { long total, correct, wrong, ambiguous, amb_both, amb_neither; };
static struct tally g_dyn[NCLS], g_fixed[NCLS];

/* Separate accounting for the clean (no-stray, true offset 0) packets only:
 * these are the COMMON path (most received packets have no stray). If any clean
 * packet is ambiguous, the disambiguator cannot even certify a stray-free
 * packet. */
static struct tally g_dyn_clean[NCLS];

static void sweep_dynamic(void) {
    for (int i = 0; i < g_n; i++) {
        int c = g_cls[i];
        size_t L = g_len[i];

        /* offset-0 case: buffer == packet, true offset = 0. */
        {
            int amb; int off = dyn_video_offset(g_pkt[i], L, &amb);
            g_dyn[c].total++;
            g_dyn_clean[c].total++;
            if (amb) { g_dyn[c].ambiguous++; if (amb == 1) g_dyn[c].amb_both++; else g_dyn[c].amb_neither++;
                       g_dyn_clean[c].ambiguous++; }
            else if (off == 0) { g_dyn[c].correct++; g_dyn_clean[c].correct++; }
            else { g_dyn[c].wrong++; g_dyn_clean[c].wrong++; }
        }
        /* offset-1 case: [stray] + packet, true offset = 1, for all 256 strays. */
        for (int stray = 0; stray < 256; stray++) {
            uint8_t b[PKTCAP + 1];
            b[0] = (uint8_t)stray;
            memcpy(b + 1, g_pkt[i], L);
            int amb; int off = dyn_video_offset(b, L + 1, &amb);
            g_dyn[c].total++;
            if (amb) { g_dyn[c].ambiguous++; if (amb == 1) g_dyn[c].amb_both++; else g_dyn[c].amb_neither++; }
            else if (off == 1) g_dyn[c].correct++;
            else g_dyn[c].wrong++;
        }
    }
}

static void sweep_fixed(void) {
    for (int i = 0; i < g_n; i++) {
        int c = g_cls[i];
        size_t L = g_len[i];
        uint16_t pkt_len = (uint16_t)L;    /* true packet length */

        {
            int amb; int off = fixed_offset(g_pkt[i], L, pkt_len, &amb);
            g_fixed[c].total++;
            if (amb) g_fixed[c].ambiguous++;
            else if (off == 0) g_fixed[c].correct++;
            else g_fixed[c].wrong++;
        }
        for (int stray = 0; stray < 256; stray++) {
            uint8_t b[PKTCAP + 1];
            b[0] = (uint8_t)stray;
            memcpy(b + 1, g_pkt[i], L);
            int amb; int off = fixed_offset(b, L + 1, pkt_len, &amb);
            g_fixed[c].total++;
            if (amb) g_fixed[c].ambiguous++;
            else if (off == 1) g_fixed[c].correct++;
            else g_fixed[c].wrong++;
        }
    }
}

static double pct(long a, long b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

static void print_table(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("  DYNAMIC video frame-offset disambiguation -- exhaustive stray sweep\n");
    printf("  (offset-0 clean + offset-1 with ALL 256 possible stray bytes, per packet)\n");
    printf("================================================================================\n\n");

    printf("%-26s | %-32s | %-32s\n", "", "DYNAMIC candidate", "FIXED control (LE16)");
    printf("%-26s | %10s %8s %8s | %10s %8s %8s\n",
           "packet class", "%correct", "%wrong", "%amb", "%correct", "%wrong", "%amb");
    printf("--------------------------------------------------------------------------------"
           "----------------------\n");
    for (int c = 0; c < NCLS; c++) {
        printf("%-26s | %9.4f%% %7.4f%% %7.4f%% | %9.4f%% %7.4f%% %7.4f%%\n",
               cls_name[c],
               pct(g_dyn[c].correct, g_dyn[c].total),
               pct(g_dyn[c].wrong,   g_dyn[c].total),
               pct(g_dyn[c].ambiguous, g_dyn[c].total),
               pct(g_fixed[c].correct, g_fixed[c].total),
               pct(g_fixed[c].wrong,   g_fixed[c].total),
               pct(g_fixed[c].ambiguous, g_fixed[c].total));
    }
    printf("\nRaw trial counts (total / correct / WRONG / ambiguous):\n");
    printf("%-26s | %-34s | %-34s\n", "packet class", "DYNAMIC", "FIXED");
    printf("--------------------------------------------------------------------------------"
           "----------------------\n");
    for (int c = 0; c < NCLS; c++) {
        printf("%-26s | %9ld %9ld %9ld %6ld | %9ld %9ld %9ld %6ld\n",
               cls_name[c],
               g_dyn[c].total, g_dyn[c].correct, g_dyn[c].wrong, g_dyn[c].ambiguous,
               g_fixed[c].total, g_fixed[c].correct, g_fixed[c].wrong, g_fixed[c].ambiguous);
    }

    /* Aggregates */
    struct tally dt = {0}, ft = {0};
    for (int c = 0; c < NCLS; c++) {
        dt.total += g_dyn[c].total; dt.correct += g_dyn[c].correct;
        dt.wrong += g_dyn[c].wrong; dt.ambiguous += g_dyn[c].ambiguous;
        ft.total += g_fixed[c].total; ft.correct += g_fixed[c].correct;
        ft.wrong += g_fixed[c].wrong; ft.ambiguous += g_fixed[c].ambiguous;
    }
    printf("--------------------------------------------------------------------------------"
           "----------------------\n");
    printf("%-26s | %9.4f%% %7.4f%% %7.4f%% | %9.4f%% %7.4f%% %7.4f%%\n", "ALL CLASSES",
           pct(dt.correct, dt.total), pct(dt.wrong, dt.total), pct(dt.ambiguous, dt.total),
           pct(ft.correct, ft.total), pct(ft.wrong, ft.total), pct(ft.ambiguous, ft.total));
    printf("\nDYNAMIC ambiguity breakdown (both-valid = real collision = DANGEROUS;\n"
           "neither-valid = falls back to learned sticky offset):\n");
    printf("%-26s | %12s %14s\n", "packet class", "amb both", "amb neither");
    printf("------------------------------------------------------------\n");
    for (int c = 0; c < NCLS; c++) {
        printf("%-26s | %12ld %14ld\n", cls_name[c],
               g_dyn[c].amb_both, g_dyn[c].amb_neither);
    }

    printf("\nClean-packet path only (no stray, true offset 0 -- the COMMON case):\n");
    printf("%-26s | %10s %8s %8s\n", "packet class", "%correct", "%wrong", "%amb");
    printf("------------------------------------------------------------\n");
    for (int c = 0; c < NCLS; c++) {
        printf("%-26s | %9.4f%% %7.4f%% %7.4f%%\n", cls_name[c],
               pct(g_dyn_clean[c].correct, g_dyn_clean[c].total),
               pct(g_dyn_clean[c].wrong, g_dyn_clean[c].total),
               pct(g_dyn_clean[c].ambiguous, g_dyn_clean[c].total));
    }

    printf("\nCorpus: %d captured packets per mode.\n", g_n);
}

int main(void) {
    fec_gf_init();

    /* DYNAMIC corpus + sweep */
    reset();
    g_mode = PROTO_MODE_DYNAMIC;
    gen_corpus(PROTO_MODE_DYNAMIC);
    sweep_dynamic();

    /* FIXED corpus + sweep (same NALs, FIXED framing) */
    reset();
    g_mode = PROTO_MODE_FIXED;
    gen_corpus(PROTO_MODE_FIXED);
    sweep_fixed();

    print_table();
    return 0;
}
