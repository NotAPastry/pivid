# 2. The Outer Protocol Packet

Every record on the link — video, FEC, OSD-grid, bind — is built by
`protocol_build` and parsed by `protocol_parse`
([protocol.c](../../common/protocol.c)). It is the smallest common
frame: a 1-byte flags field, an optional 2-byte length (FIXED only), then the
payload.

## Header by mode

```
FIXED   (3-byte header):   [ flags(1) ][ len(u16 LE)(2) ][ payload (0..508) ]   (video, FEC, bind)
DYNAMIC (1-byte header):   [ flags(1) ][ payload (0..508) ]      len = record_len - 1   (OSD-grid)
```

- **Video and FEC outer packets are FIXED** (3-byte header) — and this is
  deliberate. The 2-byte LE16 length is **load-bearing for the RX driver's SPI
  frame-sync** (it disambiguates the leading stray byte via an exact `pkt_len-3`
  match). DYNAMIC outer framing for video was evaluated and **rejected** — it has
  no field the frame-sync can use and misframes (57–88 % ambiguous, proven
  offline). See [01-link-flrc.md](01-link-flrc.md) and
  [docs/MEASUREMENTS.md](../MEASUREMENTS.md) ("DYNAMIC video frame-sync").
- **OSD-grid fragments are DYNAMIC** (1-byte header) — they carry their own
  flags+version signature the driver frame-sync recognizes
  ([rx_loop.c](../../apps/pivid-rx/src/rx_loop.c)), so they do
  not need the LE16.
- `PROTOCOL_MAX_PAYLOAD = 508` ([protocol.h](../../common/protocol.h)):
  with the 511 B FLRC container and the 3-byte FIXED header, `511 - 3 = 508`.
- `protocol_parse` validates `len` against **both** `PROTOCOL_MAX_PAYLOAD` and the
  input bound *before any copy* — a malformed length can never index past either
  buffer ([protocol.c](../../common/protocol.c)).

> **The mode-mismatch landmine is closed by pinning the chip, not by changing the
> protocol.** The chip is pinned DYNAMIC (no `FLRC_MODE` knob), so the
> FIXED-protocol-over-DYNAMIC-chip arrangement is now the permanent, documented
> design — and the LE16 it carries is exactly what the frame-sync needs. See
> [01-link-flrc.md](01-link-flrc.md). The RS data symbol (inner) and bind packet
> are also FIXED/self-describing.

## The flags byte (bit map)

```
 bit:   7      6      5   4   3   2      1       0
       START  END   └──── SEQ ────┘    FEC     TYPE
```

| Bit(s) | Name | Meaning | Defined |
|--------|------|---------|---------|
| 7 | `START` / `FEC_ILV` | **dual-purpose.** On a video/reassembly record: first fragment of a train. On a FEC record (`FEC=1`): the packet is an interleaved column packet | [protocol.h](../../common/protocol.h) |
| 6 | `END` | last fragment of a reassembly train | [protocol.h](../../common/protocol.h) |
| 5..2 | `SEQ` | 4-bit fragment sequence (mod 16) | [protocol.h](../../common/protocol.h) |
| 1 | `FEC` | payload is a FEC pair packet (sublayer below reassembly) | [protocol.h](../../common/protocol.h) |
| 0 | `TYPE` | 0 = `PKT_VIDEO`, 1 = `PKT_OSD_GRID` | [protocol.h](../../common/protocol.h) |

`pivid_pack_flags` / `pivid_unpack_flags`
([protocol.h](../../common/protocol.h)) are the canonical
helpers.

### Important: START/END/SEQ mean different things at different layers

The same three bit positions are reused at two layers — read them in context:

- In a **plain (non-FEC) video packet**, START/END/SEQ belong to **NAL
  reassembly** ([04-reassembly.md](04-reassembly.md)).
- In a **FEC outer packet** (FEC=1), END and SEQ are **zero**, and **bit 7 is
  reused as `PROTO_FLAG_FEC_ILV`** — set on an interleaved column packet, clear
  on a legacy one ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c),
  [protocol.h](../../common/protocol.h)). The reassembly framing lives in the
  *inner* symbol records instead. See [03-fec.md](03-fec.md).

## RX type dispatch (the routing decision)

The RX inspects `buf[0]` (the flags byte — always first in every mode) before any
further parsing ([rx_loop.c](../../apps/pivid-rx/src/rx_loop.c)):

```
        record off the ring
                │
        n >= 1 ? read buf[0]
                │
        TYPE bit (bit 0)?
        ├─ 1 (PKT_OSD_GRID) ── FEC bit set?
        │                      ├─ yes → DROP (invalid: grid is never FEC-wrapped)
        │                      └─ no  → DYNAMIC parse → osd-grid reassembly
        └─ 0 (PKT_VIDEO) ────── bump grid rx-video counter → fec_rx_feed()
                                        │
                                FEC bit (bit 1)?
                                ├─ 1 → FEC sublayer (collect symbols, RS decode)
                                └─ 0 → plain passthrough straight to reassembly
```

The wire-protocol validity matrix:

| TYPE | FEC | Meaning | Valid? |
|------|-----|---------|--------|
| 0 | 0 | plain (FEC-off / disposable) video fragment | ✅ |
| 0 | 1 | FEC pair packet (protected video) | ✅ |
| 1 | 0 | OSD-grid fragment | ✅ |
| 1 | 1 | — | ❌ dropped (TX never emits; only RF corruption) |
