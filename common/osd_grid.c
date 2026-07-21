/* common/osd_grid.c */
#include "osd_grid.h"
#include "protocol.h"
#include <string.h>

void osd_grid_clear(osd_grid_t *g) {
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++) {
        g->cells[i].ch = OSD_GRID_BLANK;
        g->cells[i].attr = 0;
    }
}

void osd_grid_write_string(osd_grid_t *g, uint8_t row, uint8_t col,
                           uint8_t attr, const uint8_t *chars, size_t n) {
    if (row >= OSD_GRID_ROWS) return;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned)col + (unsigned)i;
        if (c >= OSD_GRID_COLS) break;             /* clip at row end, no wrap */
        osd_cell_t *cell = &g->cells[row * OSD_GRID_COLS + c];
        cell->ch = chars[i];
        cell->attr = attr;
    }
}

int osd_grid_encode(const osd_grid_t *g, uint8_t *out, size_t cap) {
    size_t o = 0;
    if (cap < 1) return -1;
    size_t n_spans_pos = o; out[o++] = 0;       /* patched below */
    uint8_t n_spans = 0;
    for (unsigned r = 0; r < OSD_GRID_ROWS; r++) {
        unsigned c = 0;
        while (c < OSD_GRID_COLS) {
            if (g->cells[r * OSD_GRID_COLS + c].ch == OSD_GRID_BLANK) { c++; continue; }
            unsigned start = c;
            while (c < OSD_GRID_COLS &&
                   g->cells[r * OSD_GRID_COLS + c].ch != OSD_GRID_BLANK) c++;
            unsigned len = c - start;
            if (n_spans == 0xFF) return -1;         /* span count overflow */
            if (o + 3 + len > cap) return -1;
            out[o++] = (uint8_t)r;
            out[o++] = (uint8_t)start;
            out[o++] = (uint8_t)len;
            for (unsigned i = 0; i < len; i++)
                out[o++] = g->cells[r * OSD_GRID_COLS + start + i].ch;
            n_spans++;
        }
    }
    out[n_spans_pos] = n_spans;
    /* attribute exception list */
    if (o + 2 > cap) return -1;
    size_t n_attr_pos = o; o += 2;
    uint16_t n_attr = 0;
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++) {
        if (g->cells[i].attr == 0) continue;
        if (o + 3 > cap) return -1;
        pack_le16(&out[o], (uint16_t)i); o += 2;
        out[o++] = g->cells[i].attr;
        n_attr++;
    }
    pack_le16(&out[n_attr_pos], n_attr);
    return (int)o;
}

int osd_grid_fragment(const uint8_t *keyframe, size_t kf_len,
                      uint8_t frame_id, uint32_t tx_video_total,
                      uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX],
                      size_t frag_lens[OSD_GRID_MAX_FRAGS]) {
    size_t need = (kf_len + OSD_GRID_FRAG_CHUNK - 1) / OSD_GRID_FRAG_CHUNK;
    if (need == 0) need = 1;                       /* empty keyframe still 1 frag */
    if (need > OSD_GRID_MAX_FRAGS) return -1;
    size_t off = 0;
    for (size_t i = 0; i < need; i++) {
        size_t chunk = kf_len - off;
        if (chunk > OSD_GRID_FRAG_CHUNK) chunk = OSD_GRID_FRAG_CHUNK;
        uint8_t *p = frags[i];
        p[0] = OSD_GRID_VERSION;
        p[1] = frame_id;
        p[2] = (uint8_t)i;
        p[3] = (uint8_t)need;
        pack_le32(&p[4], tx_video_total);
        memcpy(&p[OSD_GRID_FRAG_HDR], &keyframe[off], chunk);
        frag_lens[i] = OSD_GRID_FRAG_HDR + chunk;
        off += chunk;
    }
    return (int)need;
}

void osd_grid_reasm_init(osd_grid_reasm_t *r) { memset(r, 0, sizeof *r); }

/* wrap-aware "a is newer than b" over a mod-256 counter */
static bool id_newer(uint8_t a, uint8_t b) {
    return (uint8_t)(a - b) != 0 && (uint8_t)(a - b) < 128u;
}

int osd_grid_reasm_feed(osd_grid_reasm_t *r, const uint8_t *payload, size_t len,
                        osd_grid_t *out, uint32_t *out_tx_total) {
    if (len < OSD_GRID_FRAG_HDR) return -1;
    if (payload[0] != OSD_GRID_VERSION) return -1;
    uint8_t fid   = payload[1];
    uint8_t index = payload[2];
    uint8_t count = payload[3];
    uint32_t txv  = unpack_le32(&payload[4]);
    size_t chunk  = len - OSD_GRID_FRAG_HDR;
    if (count == 0 || count > OSD_GRID_MAX_FRAGS) return -1;
    if (index >= count) return -1;
    if (chunk > OSD_GRID_FRAG_CHUNK) return -1;

    /* drop a straggler that is older than the frame we're on / last applied */
    if (r->in_progress && id_newer(r->frame_id, fid)) { r->frags_stale++; return 0; }
    if (!r->in_progress && r->have_last && !id_newer(fid, r->last_frame_id)) {
        r->frags_stale++; return 0;
    }
    /* a newer frame_id while mid-frame -> abandon the partial */
    if (r->in_progress && fid != r->frame_id) {
        r->frames_dropped++;
        r->in_progress = false;
    }
    if (!r->in_progress) {
        r->in_progress = true;
        r->frame_id = fid;
        r->frag_count = count;
        r->got_mask = 0;
        r->tx_video_total = txv;
    }
    if (count != r->frag_count) return -1;         /* inconsistent fragmentation */
    if (r->got_mask & (1u << index)) return 0;     /* duplicate fragment -> idempotent no-op */
    r->frags_rx++;
    r->got_mask |= (1u << index);
    memcpy(&r->buf[index * OSD_GRID_FRAG_CHUNK], &payload[OSD_GRID_FRAG_HDR], chunk);
    r->chunk_len[index] = chunk;

    uint32_t full = (1u << count) - 1u;            /* count is 1..OSD_GRID_MAX_FRAGS (<=4) */
    if ((r->got_mask & full) != full) return 0;    /* still missing fragments */

    /* got_mask is zeroed on frame start; chunk_len[i] is always written
     * before its mask bit is set, so stale prior-frame values are unreachable. */
    /* concatenate chunks in order, then decode */
    uint8_t kf[OSD_GRID_MAX_FRAGS * OSD_GRID_FRAG_CHUNK];
    size_t o = 0;
    for (uint8_t i = 0; i < count; i++) {
        memcpy(&kf[o], &r->buf[i * OSD_GRID_FRAG_CHUNK], r->chunk_len[i]);
        o += r->chunk_len[i];
    }
    int rc = osd_grid_decode(out, kf, o);
    r->in_progress = false;
    r->have_last = true;
    r->last_frame_id = fid;
    if (rc != 0) { r->frames_dropped++; return -1; }
    r->frames_done++;
    if (out_tx_total) *out_tx_total = r->tx_video_total;
    return 1;
}

static void put(osd_grid_t *g, uint8_t row, uint8_t col, uint8_t ch) {
    if (row < OSD_GRID_ROWS && col < OSD_GRID_COLS) {
        g->cells[row * OSD_GRID_COLS + col].ch   = ch;
        g->cells[row * OSD_GRID_COLS + col].attr = 0;   /* hard mask: drop any BF attr */
    }
}

void osd_grid_inject_vid(osd_grid_t *g,
                         uint8_t rssi_row, uint8_t rssi_col,
                         uint8_t lq_row, uint8_t lq_col,
                         int16_t vid_rssi_dbm, uint8_t vid_lq_pct) {
    /* RSSI: icon + signed dBm (e.g. "-66"), or "--" when no real RSSI yet.
     * Real RSSI is always negative dBm; dbm >= 0 means unset (init value 0). */
    put(g, rssi_row, rssi_col, OSD_SYM_RSSI);
    if (vid_rssi_dbm >= 0) {
        put(g, rssi_row, (uint8_t)(rssi_col + 1), '-');
        put(g, rssi_row, (uint8_t)(rssi_col + 2), '-');
    } else {
        char tmp[8]; int t = 0;
        int v = -vid_rssi_dbm;
        char digs[6]; int nd = 0;
        do { digs[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 5);
        tmp[t++] = '-';
        while (nd) tmp[t++] = digs[--nd];
        for (int i = 0; i < t; i++) put(g, rssi_row, (uint8_t)(rssi_col + 1 + i), (uint8_t)tmp[i]);
    }

    /* LQ: icon + value or "--" */
    put(g, lq_row, lq_col, OSD_SYM_LQ);
    if (vid_lq_pct == OSD_VID_LQ_NA) {
        put(g, lq_row, (uint8_t)(lq_col + 1), '-');
        put(g, lq_row, (uint8_t)(lq_col + 2), '-');
    } else {
        uint8_t q = vid_lq_pct > 100 ? 100 : vid_lq_pct;
        char ld[3]; int n = 0;
        do { ld[n++] = (char)('0' + q % 10); q /= 10; } while (q && n < 3);
        for (int i = 0; i < n; i++)
            put(g, lq_row, (uint8_t)(lq_col + 1 + i), (uint8_t)ld[n - 1 - i]);
    }
}

size_t osd_grid_wire_serialize(uint8_t *buf, size_t cap, const osd_grid_t *g) {
    if (cap < OSD_GRID_WIRE_LEN) return 0;
    size_t o = 0;
    pack_le32(&buf[o], OSD_GRID_WIRE_MAGIC); o += 4;
    buf[o++] = OSD_GRID_WIRE_VERSION;
    buf[o++] = (uint8_t)OSD_GRID_ROWS;
    buf[o++] = (uint8_t)OSD_GRID_COLS;
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++) {
        buf[o++] = g->cells[i].ch;
        buf[o++] = g->cells[i].attr;
    }
    return o;
}

int osd_grid_wire_deserialize(const uint8_t *buf, size_t len, osd_grid_t *out) {
    if (len < OSD_GRID_WIRE_LEN) return -1;
    if (unpack_le32(buf) != OSD_GRID_WIRE_MAGIC) return -1;
    if (buf[4] != OSD_GRID_WIRE_VERSION) return -1;
    if (buf[5] != OSD_GRID_ROWS || buf[6] != OSD_GRID_COLS) return -1;
    size_t o = 7;
    for (unsigned i = 0; i < OSD_GRID_CELLS; i++) {
        out->cells[i].ch = buf[o++];
        out->cells[i].attr = buf[o++];
    }
    return 0;
}

int osd_grid_decode(osd_grid_t *g, const uint8_t *in, size_t len) {
    osd_grid_clear(g);
    size_t o = 0;
    if (len < 1) return -1;
    uint8_t n_spans = in[o++];
    for (uint8_t s = 0; s < n_spans; s++) {
        if (o + 3 > len) return -1;
        uint8_t r = in[o++], col = in[o++], slen = in[o++];
        if (slen == 0) return -1;        /* zero-length span -> malformed */
        if (r >= OSD_GRID_ROWS || (unsigned)col + slen > OSD_GRID_COLS) return -1;
        if (o + slen > len) return -1;
        for (uint8_t i = 0; i < slen; i++)
            g->cells[r * OSD_GRID_COLS + col + i].ch = in[o++];
    }
    if (o + 2 > len) return -1;
    uint16_t n_attr = unpack_le16(&in[o]); o += 2;
    for (uint16_t a = 0; a < n_attr; a++) {
        if (o + 3 > len) return -1;
        uint16_t off = unpack_le16(&in[o]); o += 2;
        uint8_t attr = in[o++];
        if (off >= OSD_GRID_CELLS) return -1;
        g->cells[off].attr = attr;
    }
    if (o != len) return -1;        /* trailing garbage -> malformed */
    return 0;
}
