# 5. OSD Grid Packets (TYPE=1)

The OSD-grid path relays a Betaflight **MSP DisplayPort** 30×16 character grid
from the TX, with the RX masking/injecting the vid RSSI/LQ cells to the caster → player, which
renders it with the embedded BF font. On the RF link these are **TYPE=1, FEC=0,
DYNAMIC** outer packets. Pure codec in
[osd_grid.c](../../common/osd_grid.c) / [osd_grid.h](../../common/osd_grid.h).

The grid model: 30 cols × 16 rows = **480 cells**, each
`{ ch (font index 0..255), attr (opaque byte) }`. `OSD_GRID_BLANK = 0x20` (space)
is an empty cell. The codec never interprets `attr` — only the player does.

## RF wire: the OSD-grid fragment

Each fragment is a complete, self-contained DYNAMIC outer packet — it is **not**
fed to the FEC stage and does **not** advance the reassembler's `expected_seq`
([rx_loop.c](../../apps/pivid-rx/src/rx_loop.c)). TX emits them
with SEQ=0.

```
outer:   [ flags: TYPE=1, FEC=0 (1) ][ frag-hdr(8) ][ keyframe chunk (≤256) ]
                                      └──────── DYNAMIC payload ────────┘
```

Fragment header — `OSD_GRID_FRAG_HDR = 8` bytes
([osd_grid.c](../../common/osd_grid.c)):

| Byte | Field | Notes |
|------|-------|-------|
| 0 | `version` | `OSD_GRID_VERSION = 1`; RX rejects a mismatch |
| 1 | `frame_id` | mod-256 keyframe id; wrap-aware "newer" compare on RX |
| 2 | `index` | fragment index `0..count-1` |
| 3 | `count` | total fragments `1..4` (`OSD_GRID_MAX_FRAGS`) |
| 4..7 | `tx_video_total` (u32 LE) | TX's video-packet counter, surfaced for the player |

Sizes: `OSD_GRID_FRAG_CHUNK = 256` keyframe bytes max per fragment;
`OSD_GRID_FRAG_MAX = 264` (8 + 256) fits one radio packet with headroom
(`PROTOCOL_MAX_PAYLOAD = 508`). A worst-case OSD encodes to ≤4 fragments.

RX reassembly ([osd_grid.c](../../common/osd_grid.c)): collects
fragments of one `frame_id` via a `got_mask` bitmask; a newer `frame_id`
supersedes an in-progress older one; completes when all `count` fragments arrive.
A TYPE=1 packet that also has FEC=1 is **invalid** and dropped
([rx_loop.c](../../apps/pivid-rx/src/rx_loop.c)).

## Inner: the sparse-span keyframe codec

The keyframe carried across the fragments is a compact encoding of the grid
([osd_grid.c](../../common/osd_grid.c)). It is two sections — a
run of non-blank text spans, then an attribute exception list:

```
[ n_spans(1) ]
   span × n_spans:   [ row(1) ][ start_col(1) ][ len(1) ][ ch × len ]
[ n_attr(u16 LE) ]
   attr × n_attr:    [ cell_index(u16 LE) ][ attr(1) ]
```

- **Spans**: per row, each maximal run of non-`BLANK` cells is emitted as
  `row, start, len`, then `len` font-char bytes. Blank cells cost nothing. Span
  count is capped at 255 — the encoder checks `n_spans == 0xFF` *before*
  emitting, so a 256th span is what errors. Unreachable in practice: a 30×16 grid
  tops out at 240 spans.
- **Attribute list**: only cells with a non-zero `attr` are listed, as
  `(cell_index, attr)`. Most cells have `attr == 0` and are omitted.

`osd_grid_decode` clears the grid, then replays spans (chars) and the attr list.

## RX vid-metric injection (the hard mask)

Before fragmenting, the RX overwrites reserved cells with vid RSSI/LQ glyphs
unconditionally — this *is* the hard mask
([osd_grid.h](../../common/osd_grid.h)). Symbol indices are
confirmed against Betaflight `osd_symbols.h`: `SYM_RSSI = 0x01`,
`SYM_LINK_QUALITY = 0x7B`. `OSD_VID_LQ_NA = 0xFF` renders as `--`.

## Loopback wire (RX → caster → player, not RF)

Over the LAN hop the full grid is sent **losslessly** (no fragmentation, no font
masking), a different serialization from the RF fragment
([osd_grid.h](../../common/osd_grid.h)):

```
[ magic 'POVG' (u32 LE = 0x47564F50) ][ version(1) ][ rows(1)=16 ][ cols(1)=30 ][ 480 x {ch,attr} ]
```

`OSD_GRID_WIRE_LEN = 7 + 480*2 = 967` bytes. This frame never touches the radio;
it's the internal RX→player transport.
