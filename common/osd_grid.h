/* common/osd_grid.h -- MSP DisplayPort 30x16 character grid: cell model,
 * apply ops, sparse-span keyframe codec, bounded fragmentation + single-frame
 * reassembly, RX vid-metric injection, and the loopback grid wire format.
 * Pure (no radio, no FC, no sockets) and host-unit-tested. */
#ifndef COMMON_OSD_GRID_H
#define COMMON_OSD_GRID_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OSD_GRID_COLS   30u
#define OSD_GRID_ROWS   16u
#define OSD_GRID_CELLS  (OSD_GRID_COLS * OSD_GRID_ROWS)   /* 480 */
#define OSD_GRID_BLANK  0x20u   /* space glyph = empty cell */

/* One cell: BF font char index (0..255) + opaque attribute byte. The codec
 * never interprets attr bits; only the player does (blink/invert/page). */
typedef struct { uint8_t ch; uint8_t attr; } osd_cell_t;

/* Row-major: cell (row,col) is cells[row*OSD_GRID_COLS + col]. */
typedef struct { osd_cell_t cells[OSD_GRID_CELLS]; } osd_grid_t;

/* ---- Apply ops (TX side: fold BF DisplayPort sub-commands into a grid) ---- */
void osd_grid_clear(osd_grid_t *g);   /* all cells -> {BLANK, 0} */
void osd_grid_write_string(osd_grid_t *g, uint8_t row, uint8_t col,
                           uint8_t attr, const uint8_t *chars, size_t n);

/* ---- Sparse-span keyframe inner codec (no fragment header) ---- */
/* Encode grid -> contiguous keyframe buffer. Returns bytes written, or -1 if
 * the encoding does not fit in `cap`. */
int osd_grid_encode(const osd_grid_t *g, uint8_t *out, size_t cap);
/* Decode a contiguous keyframe buffer into *g (cleared first). Returns 0 on
 * success, -1 on malformed/over-range input. */
int osd_grid_decode(osd_grid_t *g, const uint8_t *in, size_t len);

/* ---- Fragmentation + reassembly (RF transport of one keyframe) ---- */
#define OSD_GRID_VERSION       1u
#define OSD_GRID_FRAG_HDR      8u        /* ver,id,index,count + u32 tx_total */
#define OSD_GRID_FRAG_CHUNK    256u      /* max keyframe bytes per fragment; sized
                                          * so a typical OSD (~208 B) rides one
                                          * fragment with headroom, worst-case
                                          * (~531 B) stays at 3 frags, and each
                                          * fragment (264 B) fits one radio packet
                                          * (PROTOCOL_MAX_PAYLOAD=508). */
#define OSD_GRID_MAX_FRAGS     4u
#define OSD_GRID_FRAG_MAX      (OSD_GRID_FRAG_HDR + OSD_GRID_FRAG_CHUNK)  /* 264 */

/* Split an encoded keyframe into PKT_OSD_GRID payloads. Writes each
 * fragment into frags[i] (length frag_lens[i]); returns the fragment count
 * (1..OSD_GRID_MAX_FRAGS) or -1 if the keyframe needs more fragments. */
int osd_grid_fragment(const uint8_t *keyframe, size_t kf_len,
                      uint8_t frame_id, uint32_t tx_video_total,
                      uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX],
                      size_t frag_lens[OSD_GRID_MAX_FRAGS]);

typedef struct {
    uint8_t  buf[OSD_GRID_MAX_FRAGS * OSD_GRID_FRAG_CHUNK];
    size_t   chunk_len[OSD_GRID_MAX_FRAGS];
    uint32_t got_mask;          /* received frag_index bitmask */
    uint8_t  frame_id;
    uint8_t  frag_count;
    uint32_t tx_video_total;
    bool     in_progress;
    bool     have_last;         /* a frame_id was completed at least once */
    uint8_t  last_frame_id;
    /* receive-health counters */
    uint64_t frags_rx, frames_done, frames_dropped, frags_stale;
} osd_grid_reasm_t;

void osd_grid_reasm_init(osd_grid_reasm_t *r);
/* Feed one PKT_OSD_GRID payload. Returns 1 and fills *out (+ *out_tx_total)
 * when this fragment completes a frame; 0 if more fragments are needed or the
 * fragment was dropped (stale/duplicate); -1 on malformed input. */
int osd_grid_reasm_feed(osd_grid_reasm_t *r, const uint8_t *payload, size_t len,
                        osd_grid_t *out, uint32_t *out_tx_total);

/* ---- RX vid-metric injection (hard mask of reserved cells) ---- */
/* BF font symbol indices for the injected icons, confirmed against Betaflight
 * src/main/drivers/osd_symbols.h: SYM_RSSI=0x01, SYM_LINK_QUALITY=0x7B. */
#define OSD_SYM_RSSI  0x01u
#define OSD_SYM_LQ    0x7Bu      /* BF SYM_LINK_QUALITY */
#define OSD_VID_LQ_NA 0xFFu      /* vid-LQ "N/A" sentinel: 0xFF -> renders "--" */

/* Overwrite the reserved cells with vid RSSI/LQ glyphs (unconditional -> this
 * IS the hard mask). dbm <= -1 expected; lq 0..100 or OSD_VID_LQ_NA -> "--". */
void osd_grid_inject_vid(osd_grid_t *g,
                         uint8_t rssi_row, uint8_t rssi_col,
                         uint8_t lq_row, uint8_t lq_col,
                         int16_t vid_rssi_dbm, uint8_t vid_lq_pct);

/* ---- Loopback grid wire (RX -> caster -> player, full grid, lossless) ---- */
#define OSD_GRID_WIRE_MAGIC   0x47564F50u  /* 'P','O','V','G' little-endian */
#define OSD_GRID_WIRE_VERSION 1u
#define OSD_GRID_WIRE_LEN     (7u + OSD_GRID_CELLS * 2u)   /* 967 */
size_t osd_grid_wire_serialize(uint8_t *buf, size_t cap, const osd_grid_t *g);
int    osd_grid_wire_deserialize(const uint8_t *buf, size_t len, osd_grid_t *out);

#endif
