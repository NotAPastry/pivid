# PiVid v2.1 — Wire Protocol Reference

Complete description of **every packet framing** on the PiVid v2.1 link, derived
from the live source (not from memory or stale code comments). Each document
cites the authoritative source file so it can be re-verified. References are
deliberately file-and-symbol, not `file:line` — line numbers rot faster than the
docs get updated.

## The layered stack

A camera NAL travels down this stack on the TX and back up on the RX. Each layer
has one job and a well-defined record format handed to the layer below.

```
            TX                                       RX
  ┌───────────────────────┐                ┌───────────────────────┐
  │ H.264 NAL (rpicam)    │                │ H.264 NAL → player    │
  ├───────────────────────┤                ├───────────────────────┤
  │ FEC sublayer          │  blocks/syms   │ Reassembly (reasm.c)  │  NAL fragments
  │ (fec_tx.c)            │                │ FEC decode (fec_rx.c) │
  ├───────────────────────┤                ├───────────────────────┤
  │ Outer protocol packet │  [flags][...]  │ rx_loop TYPE dispatch │
  │ (protocol_build)      │                │ + protocol_parse      │
  ├───────────────────────┤                ├───────────────────────┤
  │ FLRC chip container    │  sync+CRC      │ FLRC chip (CRC-valid) │
  │ (LR2021 driver)        │                │ record delivered      │
  └───────────────────────┘                └───────────────────────┘
            └──────────────── 2.1 GHz RF ────────────────┘
```

Two independent traffic classes share the link, separated by the **TYPE** bit of
the outer flags byte:

| TYPE | Class | FEC? | Outer mode | Doc |
|------|-------|------|-----------|-----|
| 0 | Video (H.264) | optional (per-class) | FIXED | [03-fec.md](03-fec.md), [04-reassembly.md](04-reassembly.md) |
| 1 | OSD grid (MSP DisplayPort) | never | DYNAMIC | [05-osd-grid.md](05-osd-grid.md) |

A third framing, the **bind packet**, is self-describing FIXED and used only
during the pre-config bind handshake ([06-telemetry-bind.md](06-telemetry-bind.md)).

## Document index

| # | Document | Covers |
|---|----------|--------|
| 1 | [01-link-flrc.md](01-link-flrc.md) | FLRC chip container: sync word, CRC, DYNAMIC vs FIXED on-air framing, the mode-mismatch landmine |
| 2 | [02-outer-packet.md](02-outer-packet.md) | The outer protocol packet: flags byte bit-map, FIXED/DYNAMIC headers, RX type dispatch |
| 3 | [03-fec.md](03-fec.md) | FEC sublayer: blocks, symbols, the RS code, the pair packet, column interleave, SPS/PPS merge |
| 4 | [04-reassembly.md](04-reassembly.md) | NAL fragmentation/reassembly: START/END/SEQ, the length prefix, the two-level framing rule |
| 5 | [05-osd-grid.md](05-osd-grid.md) | OSD-grid (TYPE=1) fragments, sparse-span keyframe codec, the loopback wire |
| 6 | [06-telemetry-bind.md](06-telemetry-bind.md) | The 35-byte telemetry record and the 8-byte bind packet |
| 7 | [07-lan-chunking.md](07-lan-chunking.md) | LAN-hop video chunking on the caster→player IP/WiFi leg (downstream of the RF link) |

> **Docs 1–6 are the RF link** (TX↔RX). **Doc 7 is the IP/LAN distribution hop**
> (caster→player), a separate subsystem downstream of the RX — included here for a
> complete framing reference.

## Conventions used throughout

- **Byte order is little-endian** on the wire. Helpers `pack_le16/32` /
  `unpack_le16/32` ([protocol.h](../../common/protocol.h)) are
  used everywhere instead of pointer casts — aarch64 strict-aliasing/alignment
  safety (see the project memory on aarch64 C rules).
- "Record" = one CRC-valid chip payload as handed to/from the app = one outer
  protocol packet.
- "Symbol" = one fixed 252-byte (`FEC_SYMBOL_LEN`) Reed–Solomon unit.
- "Block" = one RS code unit: `k` data symbols + `m` parity symbols.
- Field tables read top-to-bottom = low offset to high offset.

## Key constants (verified against source)

| Constant | Value | Source |
|----------|-------|--------|
| `PROTOCOL_MAX_PAYLOAD` | 508 | [protocol.h](../../common/protocol.h) |
| FIXED outer header | 3 B (`flags`+`len16`) | [protocol.c](../../common/protocol.c) |
| DYNAMIC outer header | 1 B (`flags`) | [protocol.c](../../common/protocol.c) |
| `FEC_SYMBOL_LEN` | 252 | [fec.h](../../common/fec.h) |
| `FEC_HDR_BYTES` (pair header) | 4 | [protocol.h](../../common/protocol.h) |
| `FEC_K_MAX` / `FEC_M_MAX` | 200 / 32 | [fec.h](../../common/fec.h) |
| Production `m_anchor` / `m_ref` / `k_bound` | 9 / 2 / 16 | [settings.c](../../common/settings.c) |
| `REASM_BUF_BYTES` / timeout | 65536 / 500 ms | [reasm.h](../../apps/pivid-rx/src/reasm.h) |
| FLRC bitrate / max payload | 2.6 Mbps / 511 B | [lr2021_flrc_cfg.h](../../drivers/lr2021/lr2021_flrc_cfg.h) |
