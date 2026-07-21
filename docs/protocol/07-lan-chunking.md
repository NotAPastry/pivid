# 7. LAN-hop video chunking (caster → player)

> This is the IP/LAN distribution hop — a *different subsystem* from the RF link
> documented in 01–06. It does not touch the radio FEC/reassembly path.

## Where this sits

The RF docs (01–06) end when the RX hands a whole H.264 NAL to its output path. From
there the NAL travels over **IP/Ethernet/WiFi**, not RF:

```
  pivid-rx  ──loopback──▶  pivid-caster  ──LAN/WiFi──▶  pivid-player (Mac/Linux)
  (reasm.c)   127.0.0.1      (fan_out)      subscriber      (lan_reasm.c +
   emits NAL   :5600        chunks here       video_port      lan_source_next_au)
```

- **RX → caster** is loopback (`127.0.0.1:5600`); loopback MTU is 64 KB, so a whole
  framed NAL (`[00 00 00 01] + NAL`, ≤64 KB) is one datagram and never IP-fragments.
  ([main.c emit_cb](../../apps/pivid-rx/src/main.c), [output_udp.c](../../apps/pivid-rx/src/output_udp.c))
- **caster → player** is the exposed LAN/WiFi leg. A >MTU datagram IP-fragments (a
  64 KB IDR NAL → ~44 fragments) and **one lost fragment dropped the whole datagram**
  — for an IDR, the whole keyframe. This doc's framing fixes that by app-layer
  chunking so no datagram exceeds the path MTU.

## The chunk header (8 bytes)

Every **video** datagram on the caster→player leg carries this header, then a slice
of the framed NAL. Telemetry datagrams are unchanged (small, never fragment).
Little-endian via the byte-shift helpers in
[lanfrag.h](../../common/lanfrag.h) (no pointer casts; aarch64-safe).

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | u8 | `magic` | `0xA5` — first byte ≠ `0x00`, so a chunk is distinguishable from a legacy raw NAL (which starts `00 00 00 01`) |
| 1 | u8 | `version` | `0x01` |
| 2 | u16 | `nal_id` | rolling per-NAL id (wraps at 65536); groups a NAL's chunks |
| 4 | u16 | `frag_off` | byte offset of this chunk within the framed NAL (always a multiple of `LANFRAG_CHUNK_PAYLOAD`) |
| 6 | u16 | `total_len` | total framed-NAL length (`[00 00 00 01]` start code + NAL), ≤ 65535 |
| 8 | … | `payload` | ≤ `LANFRAG_CHUNK_PAYLOAD` (1400) framed-NAL bytes |

- `payload_len = datagram_len − 8`. **START = (`frag_off == 0`)**, **END =
  (`frag_off + payload_len == total_len`)** — derived, not flagged.
- Offset-addressed (not the RF link's 4-bit `SEQ` train): a 64 KB NAL is ~47 chunks,
  which would wrap a 4-bit SEQ ~3× *within one NAL*, and — more importantly —
  offset addressing lets the receiver keep the chunks it *did* get and forward a
  partial prefix. The SEQ-train model resets on any gap (RF relies on FEC below it).

### Key constants ([lanfrag.h](../../common/lanfrag.h))

| Constant | Value |
|----------|-------|
| `LANFRAG_MAGIC` / `LANFRAG_VERSION` | `0xA5` / `0x01` |
| `LANFRAG_HDR_BYTES` | 8 |
| `LANFRAG_CHUNK_PAYLOAD` | 1400 (→ 1408 B datagram < 1472 = 1500 MTU − 28 IP/UDP) |
| `LANFRAG_MAX_NAL` | 65535 (u16 `total_len` ceiling) |

## Caster side (chunk)

[caster.c `fan_out_video`](../../apps/pivid-caster/src/caster.c) splits each framed
NAL via `lanfrag_chunk_nal` and sends each chunk with `sendmsg`/iovec (header +
pointer into the NAL — zero payload copy) to every subscriber's `video_port`. One
`nal_id` per NAL. `handle_video` uses `recv(MSG_TRUNC)` and drops + counts
(`oversize_drops`) any framed NAL > 65535 instead of silently truncating it.
Counters: `video_fwd` now counts **chunks** (≈tens× a NAL), `oversize_drops`,
`chunk_send_fail`.

> **Max-NAL corner:** the u16 `total_len` ceiling is 65535, but the RX frames
> start-code + NAL up to `REASM_BUF_BYTES` (65536) + 4 = **65540 B**. A framed NAL in
> the `(65535, 65540]` range is dropped on the LAN hop (counted `oversize_drops`) —
> the player simply re-acquires on the next keyframe. In practice IDRs are tens of KB,
> far below this; raising the ceiling would need a u32 header (rejected — 8-byte
> header was the chosen trade).

## Player side (reassemble + partial-forward)

[lan_reasm.c](../../apps/pivid-player/src/lan_reasm.c) is a 4-slot open-NAL window
keyed by `nal_id`; each slot tracks received 1400-B chunks in a `uint64_t`
bitmap. On flush it emits the **contiguous prefix `[0 … first_gap)`** in ascending
`nal_id` order — **truncate-at-first-gap, never zero-fill a hole**. Flush triggers:
complete; superseded (evict the oldest slot as partial when all 4 are in use);
150 ms timeout backstop.

[lan_source.c](../../apps/pivid-player/src/lan_source.c) owns the UDP socket and the
reassembler and exposes **`lan_source_next_au()`**: it recvs datagrams, runs chunk
reassembly, assembles access units via `au_asm`, and serves one AU per call from a
6-slot ring. libavformat is bypassed entirely — `video_decode` feeds AUs straight
to `avcodec_send_packet`. A partial NAL's clean prefix reaches
the **SW conceal-in-place decoder** (`OUTPUT_CORRUPT`, `err_recognition=0`), so a
lost-fragment IDR degrades to a *concealed* keyframe instead of a blank one.
Counters: `nals_complete`, `nals_partial`, `chunks_dropped`, `nal_degenerate`,
`bad_magic`.

## Why not constrain encoder slice size instead?

`--slice-mb` (keep NALs < MTU) was rejected: it is a TX-side change that adds NAL
overhead to the **radio** airtime (the constrained link) to fix a **LAN-only**
problem, and the codec has no byte-bounded slice mode anyway. The chunking lives entirely on the IP hop.
