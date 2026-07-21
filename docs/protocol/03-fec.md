# 3. FEC Sublayer (blocks, symbols, the pair packet)

The FEC sublayer sits **between** reassembly and the outer packet. It turns a NAL
into Reed–Solomon (RS) blocks so that whole-packet erasures (a CRC-failed FLRC
frame the chip dropped) can be recovered. It is per-class: anchor NALs
(IDR/SPS/PPS) get heavy parity, reference-P gets light parity, disposables get
none.

TX: [fec_tx.c](../../apps/pivid-tx/src/fec_tx.c) · RX:
[fec_rx.c](../../apps/pivid-rx/src/fec_rx.c) · shared math + constants:
[fec.h](../../common/fec.h).

## Symbols, blocks, and the RS code

- **Symbol** = a fixed `FEC_SYMBOL_LEN = 252`-byte unit.
  - A **data** symbol is itself a small *inner* FIXED protocol record
    (`[flags][len16][video bytes]`) zero-padded to 252
    ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)). The inner
    `len16` records the real byte count; the pad is virtual.
  - A **parity** symbol is 252 B of RS output (full width).
  - **On the wire, a data symbol is truncated to its real inner record**
    (`3 + len16` bytes) — the zero pad is never transmitted. Parity ships full
    width. TX side: `fec_sym_wire_len`; RX mirror: `fec_walk_lengths`. The
    252-byte width is what RS encodes over, not what the radio carries.
- **Block** = `k` data symbols + `m` parity symbols = `n = k + m` total. The RS
  code is **systematic** (data symbols are sent as plaintext) and **Cauchy**-based:
  **any `k` of the `n` symbols reconstruct the original `k`**. Lose up to `m`
  symbols → recover; lose more → that block is unrecoverable.
- A 7-bit **`block_id`** stamps each block so the RX can regroup its symbols. It
  increments per emitted block and wraps at 128
  ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)).

Limits and production values:

| Knob | Value | Meaning |
|------|-------|---------|
| `FEC_K_MAX` | 200 | hard cap on data symbols per block |
| `FEC_M_MAX` | 32 | hard cap on parity symbols per block |
| `k_bound` (`fec_k`) | **16** | soft close: flush the block at this many data symbols ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)) |
| `m_anchor` | **9** | parity for IDR/SPS/PPS class blocks |
| `m_ref` | **2** | parity for reference-P blocks |

`fec_tx_nal` still honours `m <= 0` by sending the NAL plain, but **`m_ref = 0`
is unreachable in production**: the config layer clamps `FEC_M_REF` to 1..32
("FEC is always on" — see [DECISIONS.md](../DECISIONS.md)).

## From NAL to blocks (TX)

`fec_tx_nal` ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)) classifies the
NAL by its header byte:

| Class | `nal_unit_type` / ref | Parity `m` |
|-------|-----------------------|-----------|
| `FEC_NAL_IDR` | type 5 | `m_anchor` |
| `FEC_NAL_SPS` | type 7 | `m_anchor` |
| `FEC_NAL_PPS` | type 8 | `m_anchor` |
| `FEC_NAL_REF` | `nal_ref_idc != 0`, not above | `m_ref` |
| `FEC_NAL_DISP` | `nal_ref_idc == 0` | none (plain) |

([fec_class.h](../../apps/pivid-tx/src/fec_class.h))

- If `m <= 0` (disposable, or ref-P at `m_ref=0`): flush any open block, then send
  the NAL **plain** via `push_plain` — no FEC, no pair header. See
  [04-reassembly.md](04-reassembly.md) for the plain framing.
- Otherwise the NAL's bytes are sliced into ≤249-byte chunks
  (`FEC_INNER_PAYLOAD_MAX = FEC_SYMBOL_LEN - 3`), each wrapped as an inner FIXED
  record and appended as a data symbol. At `k_bound` the block flushes and a new
  one opens, so a large NAL (an IDR) becomes **several back-to-back blocks**.
- **Blocks are grouped, not strictly single-class.** A NAL whose *group key*
  differs from the open block flushes it first. The key
  ([fec_class.h](../../apps/pivid-tx/src/fec_class.h)) maps **SPS and PPS to a
  shared `GRP_PARAMSET`**, so they form one `k=2` anchor block paying a single
  `m_anchor` parity set instead of two. IDR keeps its own group so it stays a
  clean multi-block interleave unit. Parity `m` is still chosen per class by
  `fec_class_m`; the key only decides block boundaries.

`fec_tx_flush` ([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c)) RS-encodes
the open block (appends `m` parity symbols) and emits the packets.

## The FEC pair packet (current wire format)

To fill the 511 B FLRC container efficiently, symbols are packed **two per outer
packet**. The outer flags byte sets `PROTO_FLAG_FEC`; the payload is a 4-byte
**pair sub-header** followed by 1 or 2 full 252-byte symbols
([fec_tx.c](../../apps/pivid-tx/src/fec_tx.c),
[protocol.h](../../common/protocol.h)):

```
outer:   [ flags=FEC(1) ][ len16(2) ][ pair-hdr(4) ][ sym0(252) ][ sym1(252)? ]
                          └──────────────── FLRC payload, ≤508 ───────────────┘
   count=2 → 4 + 252 + 252 = 508 payload → 511 B on the wire  (MAXIMUM)
   count=1 → 4 + 252       = 256 payload → 259 B on the wire  (MAXIMUM)
```

Those are **upper bounds**, reached only when both symbols are full-width parity.
Because data symbols ship truncated, a typical packet is shorter — the RX derives
each symbol's length from the stream rather than assuming a fixed stride.

Pair sub-header (4 bytes, `FEC_HDR_BYTES`):

| Byte | Field | Notes |
|------|-------|-------|
| 0 | bit7 = `count` (1→0, 2→1) · bits6..0 = `block_id` | count folded into the top bit so a count=1 tail keeps a 4-byte header |
| 1 | `base_idx` | index of the **first** symbol in the block |
| 2 | `k` | RS data-symbol count |
| 3 | `n` | `= k + m`, total RS symbols |

- In a **legacy** (non-interleaved) packet a `count=2` packet carries symbols
  `base_idx` and `base_idx+1` — consecutive, same block. `emit_block_legacy`
  walks the block `(0,1),(2,3),…` with a final odd symbol as `count=1`. This is
  the fallback path; see "Column interleave" below for the default.
- Symbol index `i`: `0..k-1` = data, `k..n-1` = parity. There is **no** explicit
  data/parity bit — the index says which. (In an interleaved packet byte 1 is a
  shared *column*, and both symbols sit at that index in two sibling blocks.)
- There is **no per-packet mode bit**: a TX/RX mode mismatch is caught downstream
  by the RX sanity gate + strict length-equality check (loud mass drops), not by a
  flag.

### Why the legacy packing concentrates burst damage

This is what column interleave (below) exists to fix; it is the behaviour of the
fallback path only. A legacy `count=2` packet's two symbols come from **one**
block. Lose that one packet and
you burn **two** of that block's `m` erasure slots at once. A burst of `b`
consecutive lost packets costs a single block `2b` symbols → burst tolerance is
only **`m/2` packets**; a ~5-packet RF burst can blow a whole block (including an
IDR sub-block) while its neighbours sit untouched.

## RX side (current)

`fec_rx_feed` ([fec_rx.c](../../apps/pivid-rx/src/fec_rx.c)):

1. Parse the FIXED outer record. No `PROTO_FLAG_FEC` ⇒ it's a plain/disposable
   record: flush the open block, pass the record straight to reassembly.
2. **Sanity gate** ([fec_rx.c](../../apps/pivid-rx/src/fec_rx.c)):
   `count ∈ {1,2}`, `1 ≤ k ≤ n`, `n ≤ k + FEC_M_MAX`, plus the absolute array
   bounds `k ≤ FEC_K_MAX` and `n ≤ FEC_K_MAX + FEC_M_MAX`. The
   `base_idx + count ≤ n` bound applies to **non-interleaved packets only**; an
   interleaved packet is bounded by `col < n` instead. The `(int)` casts are
   load-bearing (they stop a `base_idx=254 + count=2` u8 wrap).
3. **Per-symbol length walk** (`fec_walk_lengths`): each symbol declares its own
   length — a data symbol is `3 + len16`, capped at `FEC_SYMBOL_LEN`; a parity
   symbol is full width. Every read is bounded by the parsed payload length, and
   the walked total must exactly equal it. This replaced an older strict
   `4 + count*252` equality check, which the wire-truncation of data symbols made
   invalid. It rejects both a truncated packet and an over-long misframe.
4. Demux the `count` symbols into the slot for their `block_id` (see the slot
   array below), mark `present[]`.
5. **Close** a slot when **all `k` data symbols** are present
   (`slot_data_complete` — the fast path, no RS decode needed), when the slot must
   be evicted because all `FEC_RX_SLOTS` are in use, when a non-FEC record
   arrives, or on the 500 ms `FEC_RX_TIMEOUT_NS`. Note there is **no** close on
   `block_id` change and **no** close on all `n` present — waiting for parity that
   is not needed would only add latency. On close: emit the data symbols directly
   if they all arrived, else RS-decode; if fewer than `k` survive, emit only what
   arrived and reassembly drops the gapped NAL.
6. **Emit in ascending `block_id` order** (`drain_ordered`) — see the ordered
   flush rule below, which is load-bearing for SEQ continuity.

Each emitted data symbol is unwrapped to its real inner record (`3 + len16`
bytes) and forwarded to reassembly
([fec_rx.c](../../apps/pivid-rx/src/fec_rx.c)).

A `block_id`-step diagnostic (`bid_noninc`,
[fec_rx.c](../../apps/pivid-rx/src/fec_rx.c)) flags fabricated block ids from
byte-shifted (misframed) sub-headers. It fires on a **new slot open** whose
serial step from the last open is `0` or greater than `8`.

---

# Column interleave + SPS/PPS merge

> **Status: implemented and on the wire.** Both the column interleave and the
> SPS/PPS merge ship; the mechanisms below describe current behaviour, not a plan.
>
> **But interleave is INERT at the sliced production config.** Pairing requires
> two held blocks with equal `(k, n)`, and sliced video produces one small block
> per NAL with varying `k`, so `emit_pair_interleaved` never fires — interleave on
> and off produce byte-identical elementary streams. It engages only for
> single-slice large-NAL profiles. Making it effective under slicing would need
> cross-NAL block pairing, which is shelved as low-ROI. See
> [DECISIONS.md](../DECISIONS.md) and [MEASUREMENTS.md](../MEASUREMENTS.md).
>
> Not to be confused with the separate **block/timing** interleaver
> (`fec_tx_set_block_depth`): that one was swept, found no knee, and does **not**
> ship enabled — production runs `FEC_TX_RING_MAX 2`.

## Design decisions

1. **Scope A — intra-NAL interleave only.** Interleave across the blocks of one
   multi-block NAL (the IDR is the prize, re-injected ~1/s on GDR recipes).
   Single-block NALs (most ref-P) ship exactly as today → zero added latency for
   them.
2. **Approach 1 — pairwise column interleave**, fitting the **existing 4-byte**
   pair header (the 252-byte symbol size leaves *no* room to grow it).
3. **`PROTO_FLAG_FEC_ILV` discriminator** — claim one of the free outer-flag bits
   (START/END/SEQ are unused in FEC packets) to distinguish interleaved packets
   from legacy ones on the same wire. Zero header growth. A *new* RX stays
   backward-compatible with an *old* TX (legacy packets have ILV=0).
4. **SPS/PPS merge — decoupled (option i).** Relax the single-class flush so the
   group key treats `{SPS, PPS}` as one block (a `k=2` anchor block, one parity
   set instead of two) while keeping `IDR` a separate block group. The merged
   SPS/PPS block is a lone block → ships legacy (not interleaved); it's tiny.

## Pairwise column interleave — the mapping

Group a multi-block NAL's **full** blocks (all `k = k_bound`, equal `n`) into
pairs `(B0,B1), (B2,B3), …`. For a pair, transmit **column-wise**: packet *j*
carries `(BlockEven[j], BlockOdd[j])` — the **same** symbol index `j` from the two
sibling blocks.

```
        block B0:  s0 s1 s2 ... s_{n-1}
        block B1:  t0 t1 t2 ... t_{n-1}
   column packets: (s0,t0) (s1,t1) (s2,t2) ...   ← one packet per column
```

Lose one column packet → B0 loses `s_j`, B1 loses `t_j` — **one** erasure slot
each. A burst of `b` consecutive columns costs each block `b` symbols → tolerance
rises to **`m` packets** (vs `m/2`) = the **≈2×** burst resilience, at **zero**
airtime cost (same 2 symbols/packet, same header). Only **2 blocks** need to be
buffered+encoded at once → latency bounded to two blocks (tighter than "a frame's
worth").

Header re-interpretation when `PROTO_FLAG_FEC_ILV` is set (still 4 bytes):

| Byte | Legacy (ILV=0) | Interleaved (ILV=1) |
|------|----------------|---------------------|
| 0 | count + `block_id` | count(=2) + **`block_A`** (the lower of the pair's two real `block_id`s) |
| 1 | `base_idx` | **column `j`** (shared by both blocks) |
| 2 | `k` | `k` (shared by both blocks) |
| 3 | `n` | `n` (shared by both blocks) |

The 7-bit id field carries `block_A`'s **real** `block_id` — *not* a separate
`pair_id` namespace. Because the TX assigns `block_id` sequentially per emitted
block and pairs **adjacent** blocks, the sibling is always
**`block_B = (block_A + 1) & 0x7F`** — correct across the 127→0 wrap by
construction. This keeps one uniform `block_id` namespace for legacy and
interleaved packets, preserves the monotonic id stream (and the `bid_noninc`
diagnostic), and lets the RX treat `(id, id+1)` identically whether they arrive
legacy-sequential or column-interleaved. The RX splits column `j` into
`block_A[j]` / `block_B[j]` and runs RS on each block independently.

### Ragged tail / odd blocks

Pairs require equal `(k,n)`, which all **full** blocks share. The final **partial**
block of a NAL (`k_last < k_bound`), and any single unpaired full block, **fall
back to today's legacy intra-block packing** (ILV=0). These are small relative to
a multi-block IDR; the dominant keyframe blocks all interleave.

## RX deinterleave — the open-block slot array

Column interleave fills `block_A[j]` and `block_B[j]` *together*, so the RX must
hold **two open blocks at once** for a pair. It therefore keeps a fixed array of
open-block slots (`FecRxSlot slot[FEC_RX_SLOTS]`) rather than a single open
block:

- **`#define FEC_RX_SLOTS 4`** ([fec_rx.h](../../apps/pivid-rx/src/fec_rx.h)).
  The block-interleaver study build overrides it to 32. A pair needs 2; a legacy block adjacent to a pair
  (e.g. the merged SPS/PPS block still RS-decoding as the next pair opens) makes 3
  transiently live; 4 is defensive headroom. *Note:* the RX drains the **in-order**
  kernel SPI ring (single antenna) — there is no network reordering at this stage,
  so the extra slots are headroom, **not** reorder-recovery. (They also make the
  design forward-compatible if antenna-diversity combining ever introduces
  reordering.)
- Each slot is keyed by `block_id` and holds the existing per-block state
  (`sym[]`, `present[]`, `k`, `n`, `have`, `last_progress_ns`). **Memory:** a slot
  keeps full `FEC_K_MAX+FEC_M_MAX` capacity (~58 KB) because the wire `k`/`n` are
  `u8` and the sanity gate admits up to `FEC_K_MAX`; 4 slots ≈ ~234 KB on the RX
  Pi (today it is one such buffer).
- **Unified demux.** Legacy packet → route `count` symbols into `slot[block_id]`
  at `base_idx`. Interleaved packet → route column `j` into `slot[block_A]` and
  `slot[block_B]` at index `j`. The slot boundary also **bounds the hostile-data
  blast radius**: a forged/misframed header can only touch its own slot(s); it can
  never corrupt an unrelated block's state.
- **Eviction** (`slot_for`). If a new `block_id` needs a slot and all are full,
  force-close and emit the **serially oldest** — `(new - old) & 0x7F`, wrap-safe,
  so the newest is never evicted. Two extra guards live here: a ghost-slot guard
  refuses to *open* a slot for a `block_id` not serially newer than the last
  emitted block (its late parity is discarded, and a stale bid can therefore never
  evict a live block), and an eviction-collapse check rejects the case where both
  halves of an interleaved pair would resolve to the same slot.

### Ordered flush rule (load-bearing — SEQ continuity)

Reassembly requires **monotonic SEQ**: a continuation whose `seq != expected_seq`
drops the in-progress NAL ([reasm.c](../../apps/pivid-rx/src/reasm.c)). The
TX assigns inner SEQ sequentially as it builds symbols, so an IDR split across
`B0,B1` has `B0` = SEQ 0..15, `B1` = the next 0..15 (mod-16). Reassembly therefore
only works if the RX emits **`B0`[0..k-1] in order, then `B1`[0..k-1]** — i.e. in
ascending `block_id` order.

So the slot flush policy is: **emit closed slots in ascending serial-`block_id`
order; a completed *younger* block waits until all older open slots have closed
(completed, RS-recovered, evicted, or timed out).** Under loss a younger block can
fill before an older one — emitting it early would jump SEQ and drop the NAL.
(A permanently-gapped `B0` still emits its survivors first; reassembly drops the
spanning NAL at the gap exactly as today, and `B1` riding the same IDR was lost
anyway — no regression.) This is an RX-only flush-ordering rule; it does not touch
the wire format, but without it the interleave would *reduce* recovery, not
improve it.

## Fail-loud on mismatch

- **New TX → old RX (misdeploy):** the old RX ignores the ILV bit and mis-reads
  the header → the existing sanity + length-walk + `bid_noninc` guards convert
  it into loud `fec_unrecoverable` / drop-counter explosions, not silent black.
- **Old TX → new RX:** legacy packets have ILV=0 → the new RX runs the legacy path
  unchanged (backward compatible).

## Validation

Host-side coverage lives in `tests/tx/test_fec_tx.c`, `tests/rx/test_fec_rx.c`
and `tests/integration/test_fec_e2e.c`, which pin the interleave flag, the
paramset merge (`k=2`), and the wire-truncation widths. `test_fec_e2e` is built
with widened dims (`-DFEC_TX_RING_MAX=16 -DFEC_RX_SLOTS=32`) so it can exercise
the interleave path that production slicing leaves inert.
