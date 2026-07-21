/* tests/test_osd_grid.c */
#include "osd_grid.h"
#include "protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static int idx(int r, int c) { return r * OSD_GRID_COLS + c; }

static void test_clear_blanks_every_cell(void) {
    osd_grid_t g;
    memset(&g, 0xAB, sizeof g);
    osd_grid_clear(&g);
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++) {
        assert(g.cells[i].ch == OSD_GRID_BLANK);
        assert(g.cells[i].attr == 0);
    }
}

static void test_write_string_places_chars(void) {
    osd_grid_t g; osd_grid_clear(&g);
    const uint8_t s[] = { 'A', 'B', 'C' };
    osd_grid_write_string(&g, 2, 5, 0x80, s, 3);
    assert(g.cells[idx(2,5)].ch == 'A' && g.cells[idx(2,5)].attr == 0x80);
    assert(g.cells[idx(2,6)].ch == 'B');
    assert(g.cells[idx(2,7)].ch == 'C');
    assert(g.cells[idx(2,8)].ch == OSD_GRID_BLANK);  /* untouched */
}

static void test_write_string_clips_at_row_end(void) {
    osd_grid_t g; osd_grid_clear(&g);
    const uint8_t s[] = { 'X', 'Y', 'Z' };
    osd_grid_write_string(&g, 0, OSD_GRID_COLS - 1, 0, s, 3);  /* only 'X' fits */
    assert(g.cells[idx(0, OSD_GRID_COLS - 1)].ch == 'X');
    /* must NOT wrap to row 1 col 0 */
    assert(g.cells[idx(1, 0)].ch == OSD_GRID_BLANK);
}

static void fill_busy(osd_grid_t *g) {
    osd_grid_clear(g);
    const uint8_t a[] = {'A','L','T',':','1','2','3'};
    const uint8_t b[] = {'V','B','A','T',' ','1','6','.','8'};
    osd_grid_write_string(g, 0, 0, 0, a, sizeof a);
    osd_grid_write_string(g, 15, 20, 0, b, sizeof b);
    g->cells[5 * OSD_GRID_COLS + 10].ch = 'W';
    g->cells[5 * OSD_GRID_COLS + 10].attr = 0x80;   /* an attr exception */
}

static void test_codec_roundtrip(void) {
    osd_grid_t g, back; fill_busy(&g);
    uint8_t buf[1200];
    int n = osd_grid_encode(&g, buf, sizeof buf);
    assert(n > 0);
    assert(osd_grid_decode(&back, buf, (size_t)n) == 0);
    assert(memcmp(&g, &back, sizeof g) == 0);
}

static void test_codec_empty_grid(void) {
    osd_grid_t g, back; osd_grid_clear(&g);
    uint8_t buf[1200];
    int n = osd_grid_encode(&g, buf, sizeof buf);
    assert(n == 3);                 /* n_spans(1)=0 + n_attr(2)=0 */
    assert(osd_grid_decode(&back, buf, (size_t)n) == 0);
    assert(memcmp(&g, &back, sizeof g) == 0);
}

static void test_decode_rejects_truncated(void) {
    osd_grid_t g; fill_busy(&g);
    uint8_t buf[1200];
    int n = osd_grid_encode(&g, buf, sizeof buf);
    osd_grid_t back;
    assert(osd_grid_decode(&back, buf, (size_t)n - 1) == -1);
}

static void test_decode_rejects_malformed_fields(void) {
    osd_grid_t g;

    /* row >= OSD_GRID_ROWS: 1 span at row=16, col=0, len=1, char='A', n_attr=0 */
    {
        uint8_t buf[] = { 1, 16, 0, 1, 'A', 0, 0 };
        assert(osd_grid_decode(&g, buf, sizeof buf) == -1);
    }

    /* col + slen > OSD_GRID_COLS: 1 span at row=0, col=28, len=5, 5 chars, n_attr=0 */
    {
        uint8_t buf[] = { 1, 0, 28, 5, 'A','B','C','D','E', 0, 0 };
        assert(osd_grid_decode(&g, buf, sizeof buf) == -1);
    }

    /* slen == 0: 1 span at row=0, col=0, len=0, n_attr=0 */
    {
        uint8_t buf[] = { 1, 0, 0, 0, 0, 0 };
        assert(osd_grid_decode(&g, buf, sizeof buf) == -1);
    }

    /* attr offset >= OSD_GRID_CELLS: 0 spans, n_attr=1, offset=0x01E0=480, attr=0x80 */
    {
        uint8_t buf[] = { 0, 1, 0, 0xE0, 0x01, 0x80 };
        assert(osd_grid_decode(&g, buf, sizeof buf) == -1);
    }
}

static void test_fragment_typical_one_packet(void) {
    osd_grid_t g; fill_busy(&g);
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    assert(kn > 0 && kn <= (int)OSD_GRID_FRAG_CHUNK);   /* busy OSD fits 1 frag */
    uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX];
    size_t lens[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 7, 0xDEADBEEF, frags, lens);
    assert(nf == 1);
    assert(frags[0][0] == OSD_GRID_VERSION && frags[0][1] == 7);
    assert(unpack_le32(&frags[0][4]) == 0xDEADBEEF);
}

static void test_reasm_roundtrip(void) {
    osd_grid_t g; fill_busy(&g);
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX];
    size_t lens[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 9, 0x11223344, frags, lens);
    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx = 0; int done = 0;
    for (int i = 0; i < nf; i++)
        done = osd_grid_reasm_feed(&rs, frags[i], lens[i], &out, &tx);
    assert(done == 1);
    assert(tx == 0x11223344);
    assert(memcmp(&g, &out, sizeof g) == 0);
    assert(rs.frames_done == 1);
}

static void test_reasm_incomplete_then_newer_frame_resets(void) {
    osd_grid_t big; osd_grid_clear(&big);
    for (unsigned r = 0; r < OSD_GRID_ROWS; r++)
        for (unsigned c = 0; c < OSD_GRID_COLS; c++)
            big.cells[r*OSD_GRID_COLS+c].ch = (uint8_t)('0' + (c % 10));
    uint8_t kf[1200]; int kn = osd_grid_encode(&big, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 1, 0, f, fl);
    assert(nf >= 2);
    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;
    assert(osd_grid_reasm_feed(&rs, f[0], fl[0], &out, &tx) == 0);  /* partial */

    osd_grid_t small; fill_busy(&small);
    uint8_t k2[1200]; int kn2 = osd_grid_encode(&small, k2, sizeof k2);
    uint8_t g2[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t gl[OSD_GRID_MAX_FRAGS];
    int nf2 = osd_grid_fragment(k2, (size_t)kn2, 2, 0, g2, gl);  /* newer id=2 */
    assert(nf2 == 1);
    assert(osd_grid_reasm_feed(&rs, g2[0], gl[0], &out, &tx) == 1);
    assert(memcmp(&small, &out, sizeof small) == 0);
    assert(rs.frames_dropped == 1);   /* the abandoned frame 1 was discarded */
}

static void test_reasm_drops_stale_older_frame(void) {
    osd_grid_t a; fill_busy(&a);
    uint8_t ka[1200]; int kna = osd_grid_encode(&a, ka, sizeof ka);
    uint8_t fa[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t la[OSD_GRID_MAX_FRAGS];
    osd_grid_fragment(ka, (size_t)kna, 50, 0, fa, la);
    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;
    assert(osd_grid_reasm_feed(&rs, fa[0], la[0], &out, &tx) == 1);  /* frame 50 done */
    uint8_t fb[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t lb[OSD_GRID_MAX_FRAGS];
    osd_grid_fragment(ka, (size_t)kna, 40, 0, fb, lb);
    assert(osd_grid_reasm_feed(&rs, fb[0], lb[0], &out, &tx) == 0);
    assert(rs.frags_stale == 1);
}

/* Helper: fill grid with dense repeating-digit pattern so all cells are
 * non-blank; guarantees the encoded keyframe requires >= 2 fragments. */
static void fill_dense(osd_grid_t *g) {
    osd_grid_clear(g);
    for (unsigned r = 0; r < OSD_GRID_ROWS; r++)
        for (unsigned c = 0; c < OSD_GRID_COLS; c++)
            g->cells[r * OSD_GRID_COLS + c].ch = (uint8_t)('0' + (c % 10));
}

static void test_reasm_duplicate_fragment_idempotent(void) {
    osd_grid_t big; fill_dense(&big);
    uint8_t kf[1200]; int kn = osd_grid_encode(&big, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 20, 0, f, fl);
    assert(nf >= 2);   /* dense grid must span multiple fragments */

    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;

    /* Feed frag[0] once, then feed it again (duplicate). */
    assert(osd_grid_reasm_feed(&rs, f[0], fl[0], &out, &tx) == 0);
    assert(osd_grid_reasm_feed(&rs, f[0], fl[0], &out, &tx) == 0); /* duplicate */

    /* Feed remaining frags; last one should complete the frame. */
    int done = 0;
    for (int i = 1; i < nf; i++)
        done = osd_grid_reasm_feed(&rs, f[i], fl[i], &out, &tx);
    assert(done == 1);
    assert(memcmp(&big, &out, sizeof big) == 0);
    assert(rs.frames_done == 1);
    assert(rs.frags_rx == (uint64_t)nf);   /* duplicate must NOT inflate frags_rx */
}

static void test_reasm_out_of_order(void) {
    osd_grid_t big; fill_dense(&big);
    uint8_t kf[1200]; int kn = osd_grid_encode(&big, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 30, 0, f, fl);
    assert(nf >= 2);

    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;
    int done = 0;
    /* Feed in reverse index order: last fragment first. */
    for (int i = nf - 1; i >= 0; i--)
        done = osd_grid_reasm_feed(&rs, f[i], fl[i], &out, &tx);
    assert(done == 1);
    assert(memcmp(&big, &out, sizeof big) == 0);
}

static void test_reasm_frame_id_wraps(void) {
    osd_grid_t g; fill_busy(&g);
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;

    /* Complete a frame with id=255. */
    osd_grid_fragment(kf, (size_t)kn, 255, 0, f, fl);
    assert(osd_grid_reasm_feed(&rs, f[0], fl[0], &out, &tx) == 1);
    assert(rs.frames_done == 1);

    /* id=0 wraps around and must be accepted as newer (id_newer(0,255) == true). */
    osd_grid_fragment(kf, (size_t)kn, 0, 0, f, fl);
    int rc = osd_grid_reasm_feed(&rs, f[0], fl[0], &out, &tx);
    assert(rc == 1);
    assert(rs.frames_done == 2);
}

static void test_reasm_decode_failure_counts_dropped(void) {
    /* Build one fragment whose keyframe body is malformed (row 16 out of range). */
    uint8_t payload[OSD_GRID_FRAG_MAX];
    payload[0] = OSD_GRID_VERSION;
    payload[1] = 5;    /* frame_id */
    payload[2] = 0;    /* index */
    payload[3] = 1;    /* count */
    pack_le32(&payload[4], 0);   /* tx_video_total */
    /* Malformed keyframe body: 1 span at row=16 (out of range), col=0, len=1, 'A', n_attr=0 */
    payload[OSD_GRID_FRAG_HDR + 0] = 1;    /* n_spans */
    payload[OSD_GRID_FRAG_HDR + 1] = 16;   /* row >= OSD_GRID_ROWS -> rejected */
    payload[OSD_GRID_FRAG_HDR + 2] = 0;    /* col */
    payload[OSD_GRID_FRAG_HDR + 3] = 1;    /* slen */
    payload[OSD_GRID_FRAG_HDR + 4] = 'A';  /* char */
    payload[OSD_GRID_FRAG_HDR + 5] = 0;    /* n_attr lo */
    payload[OSD_GRID_FRAG_HDR + 6] = 0;    /* n_attr hi */
    size_t plen = OSD_GRID_FRAG_HDR + 7;

    osd_grid_reasm_t rs; osd_grid_reasm_init(&rs);
    osd_grid_t out; uint32_t tx;
    int rc = osd_grid_reasm_feed(&rs, payload, plen, &out, &tx);
    assert(rc == -1);
    assert(rs.frames_dropped == 1);
}

/* Pin typical vs worst-case keyframe sizes. This is the de-risk gate: a
 * realistic busy OSD MUST fit one fragment; a maximally dense grid MUST fit
 * within OSD_GRID_MAX_FRAGS. Failure here means revisit the codec/cap. */
static void test_size_typical_fits_one_fragment(void) {
    /* ~20 spans of realistic OSD text */
    osd_grid_t g; osd_grid_clear(&g);
    const char *lines[] = {
        "16.8V", "0.0A", "ALT 123", "DIS", "ANGLE", "RSSI 99", "0:00",
        "GPS 12", "CRAFT_NAME", "PITCH 0", "ROLL 0", "THR 50", "MAH 250",
        "ARMED", "VTX 25", "CELL 4.2", "DIST 0", "SPD 0", "HOME 0", "SAT 12",
    };
    for (unsigned i = 0; i < 20; i++) {
        uint8_t s[OSD_GRID_COLS];
        size_t n = strlen(lines[i]); if (n > OSD_GRID_COLS) n = OSD_GRID_COLS;
        for (size_t k = 0; k < n; k++) s[k] = (uint8_t)lines[i][k];
        osd_grid_write_string(&g, (uint8_t)(i % OSD_GRID_ROWS),
                              (uint8_t)((i / OSD_GRID_ROWS) * 12), 0, s, n);
    }
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    printf("  typical keyframe = %d bytes\n", kn);
    assert(kn > 0 && kn <= (int)OSD_GRID_FRAG_CHUNK);   /* one fragment */
}

static void test_size_worst_case_within_cap(void) {
    osd_grid_t g; osd_grid_clear(&g);
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++)
        g.cells[i].ch = (uint8_t)('A' + (i % 26));      /* all non-blank */
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 0, 0, f, fl);
    printf("  worst-case keyframe = %d bytes, %d fragments\n", kn, nf);
    assert(nf >= 1 && nf <= (int)OSD_GRID_MAX_FRAGS);
}

static void test_inject_formats_rssi_lq(void) {
    osd_grid_t g; osd_grid_clear(&g);
    osd_grid_inject_vid(&g, 0, 0, 1, 0, -66, 99);
    /* RSSI cell row 0: icon then '-','6','6' */
    assert(g.cells[0].ch == OSD_SYM_RSSI);
    assert(g.cells[1].ch == '-' && g.cells[2].ch == '6' && g.cells[3].ch == '6');
    /* LQ row 1: icon then '9','9' */
    assert(g.cells[1*OSD_GRID_COLS+0].ch == OSD_SYM_LQ);
    assert(g.cells[1*OSD_GRID_COLS+1].ch == '9');
}

static void test_inject_na_renders_dashes(void) {
    osd_grid_t g; osd_grid_clear(&g);
    osd_grid_inject_vid(&g, 0, 0, 1, 0, 0, OSD_VID_LQ_NA);
    assert(g.cells[1*OSD_GRID_COLS+1].ch == '-' &&
           g.cells[1*OSD_GRID_COLS+2].ch == '-');
}

static void test_inject_rssi_na(void) {
    /* dbm=0 (init/unset): RSSI must render icon then "--", not "0" */
    osd_grid_t g; osd_grid_clear(&g);
    osd_grid_inject_vid(&g, 0, 0, 1, 0, 0, 99);
    assert(g.cells[0].ch == OSD_SYM_RSSI);          /* icon present */
    assert(g.cells[1].ch == '-' && g.cells[2].ch == '-');  /* N/A dashes */
}

static void test_grid_wire_roundtrip(void) {
    osd_grid_t g, back; fill_busy(&g);
    uint8_t buf[OSD_GRID_WIRE_LEN + 4];
    size_t n = osd_grid_wire_serialize(buf, sizeof buf, &g);
    assert(n == OSD_GRID_WIRE_LEN);
    assert(osd_grid_wire_deserialize(buf, n, &back) == 0);
    assert(memcmp(&g, &back, sizeof g) == 0);
    assert(osd_grid_wire_deserialize(buf, n - 1, &back) == -1);   /* short */
}

int main(void) {
    test_clear_blanks_every_cell();
    test_write_string_places_chars();
    test_write_string_clips_at_row_end();
    test_codec_roundtrip();
    test_codec_empty_grid();
    test_decode_rejects_truncated();
    test_decode_rejects_malformed_fields();
    test_fragment_typical_one_packet();
    test_reasm_roundtrip();
    test_reasm_incomplete_then_newer_frame_resets();
    test_reasm_drops_stale_older_frame();
    test_reasm_duplicate_fragment_idempotent();
    test_reasm_out_of_order();
    test_reasm_frame_id_wraps();
    test_reasm_decode_failure_counts_dropped();
    test_size_typical_fits_one_fragment();
    test_size_worst_case_within_cap();
    test_inject_formats_rssi_lq();
    test_inject_na_renders_dashes();
    test_inject_rssi_na();
    test_grid_wire_roundtrip();
    printf("test_osd_grid: OK\n");
    return 0;
}
