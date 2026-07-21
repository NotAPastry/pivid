# 4. NAL Reassembly (fragmentation framing)

Reassembly is the top transport layer: it turns a stream of inner records back
into whole H.264 NALs and hands them to the output path
([reasm.c](../../apps/pivid-rx/src/reasm.c)). It uses the **START / END / SEQ**
flag bits and a 2-byte total-length prefix.

## What feeds reassembly

Reassembly always parses its input as a **FIXED** record
(`[flags][len16][payload]`, [reasm.c](../../apps/pivid-rx/src/reasm.c)). It
receives records from two upstream paths — and this is the key framing rule:

| Source | Who carries START/END/SEQ | Note |
|--------|---------------------------|------|
| **Plain video** (FEC off / disposable) | the **outer** packet itself | `push_plain` builds it directly ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)) |
| **FEC-protected video** | the **inner** symbol record | the FEC sublayer strips the outer/pair header and emits the recovered inner records ([fec_rx.c](../../apps/pivid-rx/src/fec_rx.c)) |

So a protected NAL is framed **twice**: an inner FIXED record (START/END/SEQ for
reassembly) lives *inside* a data symbol, which is RS-coded and then wrapped in an
outer FEC packet (FEC flag only). A plain NAL is framed **once** (the outer packet
is the reassembly record). Either way, reassembly sees the same record shape.

## SEQ continuity

`SEQ` is a 4-bit counter (mod 16, [protocol.h](../../common/protocol.h))
shared across the whole video stream via a single `seq_counter` on the TX
([encoder.c](../../apps/pivid-tx/src/encoder.c)). Every emitted inner/plain record
increments it ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c),
[fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)). Reassembly tracks
`expected_seq` and drops the in-progress NAL on any gap.

## Fragmentation framing

A NAL is at most 65535 B (`u16` length prefix; oversize NALs are dropped at the
FEC stage, [fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)).

### Single-packet NAL (fits one record)

```
[ flags: START|END|SEQ ][ len16 ][ whole NAL ]
```

`START && END` ⇒ reassembly emits the payload immediately; no length prefix
([reasm.c](../../apps/pivid-rx/src/reasm.c)).

### Multi-fragment NAL

The **first** fragment (`START && !END`) carries a 2-byte LE **total-length
prefix** ahead of the first chunk; continuations and the END fragment carry raw
chunk bytes:

```
fragment 0 (START,!END):  [ flags ][ len16 ][ total_len(u16 LE) ][ chunk 0   ]
fragment 1 (cont)      :  [ flags ][ len16 ][ chunk 1                         ]
   ...
fragment N (END)       :  [ flags ][ len16 ][ chunk N                         ]
```

RX logic ([reasm.c](../../apps/pivid-rx/src/reasm.c)):

1. **START** (multi): read `total_len` from the first 2 payload bytes, copy the
   remaining body into the reassembly buffer, set `expected_seq = seq+1`. A START
   shorter than 2 bytes is dropped as invalid.
2. **Continuation / END**: require an in-progress NAL and `seq == expected_seq`;
   append the payload; advance `expected_seq`.
3. **END**: the accumulated `buf_len` must `==` `total_len`, else
   `dropped_len_mismatch`. On match, emit the whole NAL.

### Drop / reset counters

| Counter | Cause |
|---------|-------|
| `dropped_invalid` | outer parse failed, or a multi-START < 2 bytes |
| `dropped_seq_mismatch` | a new START while mid-NAL, or a SEQ gap |
| `dropped_overflow` | body would exceed `REASM_BUF_BYTES` (64 KB) |
| `dropped_len_mismatch` | END reached but `buf_len != total_len` |
| `dropped_timeout` | no progress for `REASM_TIMEOUT_NS` (500 ms) → `reasm_tick` resets |

> Wire-format history: the 2-byte START length prefix is the SEQ-aliasing fix —
> without it a 16-multiple fragment drop could silently splice two NALs. The
> length prefix + strict END length check make that loud. (Deploy TX+RX together;
> it is a wire-format change.)

## The 64 KB IP-fragmentation hazard (downstream, downstream)

Reassembly emits one NAL per call; the RX output path (`output_udp.c`) sends one
NAL per UDP datagram, up to 64 KB. On the `forward_to_pc` / WiFi hop to the Mac
viewer, a NAL larger than the path MTU is IP-fragmented and **one lost IP fragment
drops the whole datagram** — for an IDR, that's the keyframe. This is a LAN-side
issue (loopback is fine) and is **fixed** — see
[07-lan-chunking.md](07-lan-chunking.md). Note the chunking covers the
caster→player leg; RX→caster stays whole-NAL over loopback. It does **not** affect the RF framing above.
