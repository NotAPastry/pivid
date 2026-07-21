# Measured results

What was measured, how, and what the numbers were. This is the evidence behind
[DECISIONS.md](DECISIONS.md); read that first for the conclusions.

Everything here came off real hardware over the real FLRC link unless a section
says otherwise. Neither the raw per-run counter dumps nor the bench harnesses
that produced them are kept in the repo — this is the distilled result. Where a
measurement can be re-run using instrumentation that ships in the driver or the
daemons, the section says so.

Campaign labels (`IMP-n`, `BUG-n`) appear in code comments; they are indexed at
the bottom.

---

## Latency

### End-to-end latency budget (S1–S8)

**Question.** Where does the pipeline's latency actually sit?

**Method.** Per-stage residency histograms compiled into LATPROBE builds of
`pivid-tx` / `pivid-rx` / `pivid-caster` and the player, driven over the live
link with a frame-paced pre-encoded reference clip, harvested via SIGUSR1.

**Numbers.** Production glass-to-glass through the pipeline measured **~49 ms**,
of which ~30 ms was addressable. This sized the two fixes below.

**Reproduce.** Needs LATPROBE builds of the daemons and a bench harness to drive
and harvest them; the harness is not kept in the repo.

### Filmed glass-to-glass, and the frame-trailer NAL

**Question.** What is the real event-to-eyeball latency, and how much does the
frame-trailer NAL reclaim?

**Method.** Filmed at 240 fps with an on-screen timer, RX-local player, using a
bar-transition extraction method. Production-like motion content.

**Numbers.**
- Absolute filmed glass-to-glass ≈ **150 ms median** (IQR 142–163, p90 ~175,
  n=132), including the TX camera and both displays.
- Frame trailer is worth **−17…−21 ms median** on production-like motion
  (150 ms vs 167/171 ms, n=132/201, chunk-stable).
- Still-scene fast tail: p10 **138 ms → 46 ms**. The median there is masked by
  FEC k=16 batching.
- Tail-slice hold went **34 ms → 0**.
- Residual versus the nominal 33 ms is FEC block granularity — a partial tail
  block waits for the next frame's data. That is the next tail-latency lever if
  one is ever needed, **not** the trailer path.

**Of the ~150 ms**, the PiVid pipeline is only **~35–40 ms**. The remaining
~110 ms is capture and display physics: TX camera ~35–45 ms (30 fps sensor
readout — the single biggest chunk; a 60 fps mode would cut ~20 ms), frame
airtime ~25 ms (overlapping the pipeline), RX monitor ~10–25 ms plus ~8 ms
present quantization, and FEC partial-tail-block ~13 ms. **The split within that
110 ms is estimated, not individually measured.**

Latency levers, ranked: camera fps → monitor → FEC frame-boundary block flush.

**Method note for any re-run:** a fixed-grid block detector drifts and mislabels
blocks in ±100 ms steps — detect block centres per 1200-frame segment instead.

### RX FEC early-close

**Question.** Can a block be emitted as soon as all `k` data symbols are present,
rather than waiting for parity?

**Method.** Live A/B with an early-emit path plus a stale-bid ghost guard.

**Numbers.** Stage S5 mean **20 ms → 15.3 ms**; IDR tail max **65 ms → 39 ms**.
Recovery was unhurt — `fec_unrec` = 0 at 10 % injected loss. The win was about a
quarter of the pre-measurement guess: S5 is data-gather-dominated at `m_ref=2`.

### Player present-path push

**Question.** Does replacing the player's 60 Hz `nanosleep` poll with a waitable
generation counter help?

**Method.** `frame_gate` (pure pthread) woken by the decode thread on publish.
Live A/B.

**Numbers.** Stage S8 held **94 % → 7.5 %**; present latency **~9.7 ms →
~1.1 ms** (−8.6 ms). Safe on the RX-local radio under motion: zero added
`crc_drops`.

---

## Radio, RF and SPI throughput

### SPI turnaround and PA ramp

**Question.** How much per-packet time is spent in host/SPI overhead rather than
on air?

**Method.** Three stacked changes, each A/B'd individually with the TXPERF
instrumented driver build, then deployed and picture-verified.

**Numbers.**
- Overall: period **2838 µs → 2446 µs** = **−392 µs/packet**, **+16.0 %**
  saturation throughput.
- (b) BUSY-wait spin ≤20 µs then `usleep_range(2,10)`: submit **282 µs →
  176 µs**, period −151 µs, no loss regression across 4 interleaved runs.
- (c) FIFO-flags Get+Clear moved to 1 Hz: TX cadence neutral, RX framing
  identical, `rx_fifo_overflow` noise **94k → 37 per run**.
- (a) PA ramp 0x0A → **0x03** (128 µs → 16 µs): a further **−224 µs**. The ramp
  is paid roughly twice per packet, up and down.

**Ramp boundary sweep**, 100k packets per arm at 511 B saturation, **with** (b):
clean at ≥16 µs — `bad_header` was **exactly 4 in all seven clean arms**, a
sharp invariant, with `rx_walk_drops` 389–393. First degradation whiskers at
8 µs (`bad_header` 7, stray fragment). **Without** (b), the same 16 µs collapsed
at **28 % host-side loss with zero CRC errors** — the BUSY-wait spin is what buys
the margin, not the ramp. Spectrum and ACP were unchanged ±1 dB down to 16 µs and
were **not** verified below that.

The web capacity calculator was re-anchored to 2446 µs (`OV_PER_PKT_US`
846 → 303), moving dashboard `max_kbps` **1368 → 1671**.

**Instrumentation kept in the driver:** the `tx_ramp_idx` module parameter and
the `rssi_inst` debugfs node, so a ramp sweep and an off-channel RSSI/ACP check
can be re-run without a code change. The TXPERF build (`make txperf-driver`)
prints the submit/air/gap/period figures.

### PA config register, spec-cleaned

**Question.** Is the `pa_config` register value correct, and does correcting it
change TX power?

**Method.** Interleaved A/B, 600-sample RSSI sweeps per arm across
2092/2096/2100/2104/2108 MHz with TX on and off; 50 000-packet PER runs.

**Numbers.** Carrier TX-on **−41.0 / −41.0 / −40.0 dBm** (median/p95/max) in
**both** arms; skirts within ±1 dB; PER 49999/50000 in both. Deployed to both
Pis: `cmd_err` = 0, 648 frames over 25 s at the viewer, 0 NAL gaps.

**Conclusion.** `{0x06,0x07,0x10}` → `{0x80,0x67,0x10}`, TX power provably
unchanged. The prescribed 0x04 was **wrong**: the datasheet's stacked-cell
convention is top = MSB (proven independently by the CR-nibble airtime delta and
`rx_path` bit 0), so `pa_sel` is bit 7, not bit 2. The old value actually had
`pa_sel` = LF plus two RFU bits set, and worked only because `SelPa(HF)` runs
immediately after.

### FLRC CRC width, 16 vs 24 vs 32-bit

**Question.** The `flrc_crc_width` parameter existed end-to-end but had never
been verified on silicon or costed. Does it reach the chip, and what does it
cost?

**Method.** 100k packets per arm at 511 B saturation, plus a deliberate
TX16/RX32 mismatch arm.

**Numbers.**
- PER and framing were **identical in all arms**: loss 6–9, `rx_walk_drops`
  **391 in every arm**, chip `crc_err` 0, `payload_mismatch` 0.
- Airtime: air +4.0 µs / +8.0 µs; period **2446.6 → 2450.1 / 2454.4 µs** =
  **+0.14 % / +0.32 %** for 24/32-bit (predicted ~0.4 % for 32-bit).
- Mismatch TX16/RX32: **link stone dead**, chip `crc_err` = 100 % of synced
  packets — which is the proof that the code reaches the silicon.

All four datasheet codes (0=off, 1=16, 2=24, 3=32) are now test-pinned. CRC OFF
is rejected at the config layer; the dashboard shows a labeled 16/24/32 enum.
Default stays 16-bit.

**Re-running it** means flipping `flrc_crc_width` on *both* ends in one
operation — a mismatch is a dead link.

### RX wedge chip-liveness gate

**Question.** Can the wedge watchdog tell "chip wedged" from "RF just quiet"?

**Method.** `GetFlrcRxStats`-first gate: at >250 ms of DIO9 silence, skip
recovery if and only if the chip's own counters are frozen, mode is RX, and the
RX FIFO is empty. Fault-injection A/B with three 1-second TX outages.

**Numbers.** Legacy behaviour produced **+48 spurious recovers** (and
+2 `payload_mismatch`); the gate produced **0**, at equal loss.

**Stall RCA.** The ~95-packet stall bursts are RX-side genuine stalls, rescued
1:1 by soft recover (`wedge_soft` excess ≈ burst count). The TX is clean, and the
bursts are independent of module and ramp. They are **episodic**: 15 in 620k
packets during one 70-minute window, then **0 in 1.03M packets** afterwards
(including a 250k run and four 100k reload-cycled runs). The trigger is
environmental; the new diagnosis line (`dpkt/dcrc/dlen/mode/fifo/dio9`) will
self-classify the next occurrence — `dpkt>0` means the host lost IRQs,
frozen-with-`fifo>0` means a drain deadlock, `mode≠4` means it fell out of Rx.

Separately characterized: a benign 3–4-packet loss every ~161 s at saturation,
below the action threshold.

New knobs: `wedge_chip_gate` (default 1, runtime-writable), `wedge_quiet_skips`
debugfs counter.

**Instrumentation kept in the driver:** the `wedge_chip_gate` module parameter
(runtime-writable) and the `wedge_quiet_skips` debugfs counter, plus the
self-classifying dmesg diagnosis line.

### Earlier wedge-watchdog soak

Over a ~33-minute soak on the RX Pi the chip wedged 15 times; **every** event
recovered via the soft path (`SetStandby` + `SetRx`). `wedge_hard` stayed at 0 —
the NRESET fallback never fired.

### `fifo_read_pio`

**Question.** Does feeding the RX FIFO read from the CPU instead of DMA reduce
the residual stray rate?

**Method.** Live RF link, interleaved 90-second windows on the RX Pi.

**Numbers.** Under real production load, stray was **DMA 1.08 % vs PIO 3.11 %** —
PIO about **3× worse**, because the CPU-fed chunk seams add their own mid-stream
gaps. Under heavy `loadhog` contention both collapsed to ~19 % (a tie).

**Conclusion.** The "PIO drives the residual to ~0" hypothesis is refuted. DMA
stays the default; the knob remains only as a documented dead end.

Related: a `loadhog` A/B of the SPI inter-phase gap found a **U-shaped** response
— ~29 % stray with no gap, **~1.4 % at 1 µs**, rising again past ~50 µs. 1 µs is
the measured minimum.

### TX fallback mode

`RxTxFallbackMode = FS` keeps the synthesiser locked between packets, skipping
crystal wake-up and PLL relock on the next `SetTx`. **Measured 2026-06-05: at
saturation this gives no throughput benefit** — the TX path is
host-per-packet-bound at ~357 pkt/s regardless of CR, payload length or fallback
setting, not chip-turnaround-bound. Kept anyway (harmless,
`tx_fifo_underflow` = 0, may still help bursty traffic) and toggleable via
`tx_fallback_fs`.

### RX FIFO drain over-read (BUG-2)

**Question.** Was the final drain's `pkt_len+1` read stealing the next packet's
first byte?

**Method.** A compile-gated probe sampled `GetRxFifoLevel` at the exact instant
production would issue the over-read, comparing it against the current packet's
legitimate remainder. Surplus above that is real next-packet data.

**Numbers.** The theft occurred **3.7 %** of the time. After the fix — read
exactly `pkt_len`, with a 1-byte top-up only on a detected stray — a live A/B
showed combined misframe **2.29 % → 0.029 %**, roughly **79×**.

RX-side only. The probe has since been removed; the fix is validated by
production counters.

### FE calibration "bit 9 every Rx" (BUG-1) — refuted

**Question.** Does the chip raise `RXFREQ_NO_FE_CAL_ERR` (bit 9) on every Rx at
2.03/2.1 GHz?

**Method.** A once-per-second `GetErrors` poll on hardware, with a positive
control that skipped `CalibFE`.

**Numbers.** Bit 9 stayed **clear** across Rx entries. The positive control read
`0x0200` with `chip_err=1` — proving both that the detector works and that a
modprobe NRESET wipes the FE calibration. Round-to-nearest slot 508 is accepted
for 2030 MHz.

**Conclusion.** Refuted; no code shipped. A no-arg `CalibFE` candidate was clean
but fixed nothing. RSSI-versus-temperature drift is a separate, benign item.

---

## FEC and video quality

### FEC `m_ref` — where offline sweeps were wrong

**Question.** Can `m_ref` drop from 2 to 1 and reclaim ~24 % of reference-class
parity airtime?

**Method (offline).** A 7668-run joint sweep, then a re-sweep putting
`m_ref` 0/1/2 through synthetic Gilbert-Elliott bursts up to length 34, anchored
to a real on-air measurement of 5–34-packet bursts.

**Numbers (offline).** PSNR stayed **flat at 16.06–16.10 dB** across `m_ref`
even under those bursts, while `m_ref=2` costs ~24 % more parity than `m_ref=0`.
`m_anchor=9` was re-validated as monotonic (higher is better). On the ~iid bench
channel the whole win was `m_anchor` 5 → 9; `m_ref=2` showed no measurable
benefit. On this evidence `m_ref` was dropped 2 → 1.

**Numbers (live RCA).** On the real link the player showed intermittent blocky
lines. An RX `fec_unrec` root-cause split (`unrec_short` / `_present` /
`_deficit`) proved **100 % of unrecoverable reference-P blocks were SHORT** —
they had fewer than `k` symbols, with a mean deficit of ~1. A bursty RF erasure
loses `m+1` symbols and `m_ref=1` tolerates only 1, so RS decode failed and the
decoder concealed the slice. Restoring `m_ref` 1 → 2 drove `fec_unrec` from
**~12/min to 0** for **+~4 % wire**; AQM never fired. User-confirmed: the bands
were gone.

**Conclusion.** `m_ref=2`. The offline sweeps were blind to the real failure
mode, and the ~1 % parity reclaim at `m_ref=1` was a false economy paid for in
visible artifacts. This also refuted the competing "WiFi hop" and carrier
hypotheses.

**This is the single most important methodological result in the project:** mean
PSNR — and even worst-frame PSNR — buried a failure mode that the live
`fec_unrec` split found immediately.

### `slice_mb`

**Clean link (production VPU, held 650 kbps VBR).** `slice_mb=120` is the
**worst** row of the `{0,120,160,200,400}` sweep: **+13 % airtime** (per-packet
framing overhead — wire bytes are flat across the sweep) and **−0.93 dB PSNR**
versus single-slice.

**Under loss (GE bursts, `{0,120,160,200}`).** `slice_mb=120` has the **best**
PSNR (**14.95 dB**) *and* the best NAL delivery (**0.882**). Single-slice is
worst — one lost packet kills a whole-frame NAL — and 200 does not close the gap.

**Conclusion.** Kept at 120. The link is loss-limited, so the modest clean-link
cost buys that resilience. Separately, `dlen=8` decisively beats `dlen=4` under
burst: PSNR **14.99 vs 14.79**.

### FEC parameter operating point

On-air A/B: **9,2,16** beats the prior production **5,0** at every loss band,
with `sink_err` = 0 up to **27 % loss**. The `droppable` recipe's reference-P is
only ~24 % of P frames (the rest are disposable and FEC-off), so `m_ref` is cheap
there.

### FEC column interleave

At the sliced production config, `fec_tx` only column-interleaves equal-`(k,n)`
held blocks, but sliced video produces one small block per NAL with varying `k`,
so the paired path never fires — **interleave on and off produce byte-identical
elementary streams**. The block/timing interleaver depth sweep found no knee.

### Motion-artifact bisection

"Motion artifacts" were traced to **VBR encoder starvation** — a cap around
730k causing QP spikes — plus a ~1–2 % SPI floor. **Not RF**: air loss is under
0.2 %, and Pi and Mac decode byte- and pixel-identically. The lever is bitrate.

---

## Framing and reassembly correctness

### DYNAMIC video frame-sync

**Question.** Can the outer packet format for video drop its LE16 length field
and use the chip's DYNAMIC framing?

**Numbers.** Without the length field the RX driver's per-packet SPI frame-sync
falls back to a weak heuristic and misframes: **57–88 % ambiguous** on the clean
path in an exhaustive offline spike, and full-LE16 / FLRC_MODE variants misframed
**57–95 %**.

**Conclusion.** FIXED outer framing is kept for video and FEC. The 2 bytes cost
~0.3 % airtime, which is noise on a loss-limited link. The stray byte the
frame-sync absorbs (~1.1 %) is bcm2835 DMA jitter. OSD-grid fragments stay
DYNAMIC — they carry their own flags+version signature the frame-sync
recognizes.

### Chip LenError semantics

**Question.** When the chip raises `LenError`, does it leave a stale RX FIFO that
would misframe the next packet — i.e. does the handler need a `ClearRxFifo`?

**Method.** A probe driver build with the RX configured max at 255 B. TX sent
out-of-range on-air lengths (300, 400) interleaved with valid 100 B sends and one
below-minimum 3 B send, logging the raw `GetRxFifoLevel` on every `LenError` IRQ.

**Numbers.** Exactly **2** LenErrors for the 2 provocations, with `crc=0 sync=0
tmout=0`. `GetRxFifoLevel` read all-zero on both. An independent linktest view
recorded `recs=5 loss=0 misframe=0` — the 4×100 B plus the 3 B arrived, the two
over-range packets were never delivered, and **misframe stayed 0 across the whole
run including both LenErrors**.

**Conclusions.**
- The chip **auto-discards** the errored packet and leaves the RX FIFO **empty**;
  the pointer is not left stale. Corroborated three independent ways: the FIFO
  level reads zero, the counter rose by exactly the number of provocations, and
  every interleaved valid packet framed byte-clean. So the handler is
  **count-only — no `ClearRxFifo`, no drain.** Sync self-restores.
- **LenError fires only for a length ABOVE the configured max, never below a
  minimum.** `pld_len=3` — under the FLRC dynamic minimum of 6 — was transmitted
  and received as a valid 3 B packet, not flagged. Production therefore sizes the
  RX configured max at the real 511 ceiling so legitimate large packets are never
  rejected, and treats LenError as a corrupted/over-range-header signal.

### Dynamic length mechanism

Confirmed on hardware: the chip auto-generates the 9-bit on-air length header
from `SetFlrcPacketParams.pld_len` and transmits exactly `pld_len` payload bytes;
the RX decodes it back via `GetRxPktLength` with payload intact and CRC valid. A
host-encoded length header is unnecessary. The cost of dynamic mode is one extra
`SetFlrcPacketParams` SPI per packet, roughly 30–50 µs.

### Coalesced RX_DONE misframing

An in-band stream walker eliminated bottom-slice truncation: **83 → 0** at clean
RF. This is what allowed `--frame-trailer` to be re-enabled (padded to 6 B).

### Sub-6-byte TX guard (BUG-4)

Dynamic records shorter than the FLRC 6-byte minimum were being **dropped, not
padded**. Fixed, with a `tx_runt_drops` debugfs counter — expected 0 in
production, since userspace pads the trailer to 6 B.

### Bottom-half / still-scene corruption

Four distinct fixes: TX ring-retry on full, frame-trailer consumed correctly,
player flush, and caster subscriber keying by `(ip, ctrl src port)`.

---

## Player and render

### Player idle-flush threshold

**Question.** Should the player's idle-flush fire sooner than 250 ms?

**Numbers.** Sub-~100 ms thresholds sit **inside** the RX FEC block-hold: a k=16
block fills across several small still-scene frames, so tail slices legitimately
arrive ~100 ms late. A low threshold fired mid-frame **1–3 times per second** and
was the primary cause of still-scene bottom-half corruption.

**Conclusion.** 250 ms, as a true-stall fallback only. Latency is owned by
trailer-triggered AU emit — the buffer is empty whenever a flush could
legitimately fire. The proposed "lower to ~15 ms" is refuted; do not re-run it.

### TX idle-flush — reverted

Tried and reverted: `nal_parser_finish` emits unconditionally mid-slice, which
corrupts the picture. Not an RF problem. The frame-trailer NAL is the safe
clean-boundary flush.

### SDL render backend

Pinning `SDL_RENDER_DRIVER=opengles2` took Mesa errors to **0** and "acquired
video" from **20119 ms → 221 ms**. See DECISIONS.md for why this is nearly
undiagnosable without `MESA_DEBUG=1`.

### RX-local player boot time

**Question.** Why does the RX HDMI picture take over a minute to appear?

**Method.** systemd critical-chain analysis on the RX with WiFi working.

**Numbers.** `ifup@wlan0` took **60.2 s** → `network.target` 65.8 s → caster
65.8 s → player, while `pivid-rx` (the radio) had been up since **4.8 s**. With
no AP in range, `ifup` blocks to its full 5-minute timeout — meaning ~5 minutes
of black HDMI on a receiver whose entire video path is loopback-only.

After removing the single `After=network.target` line: caster **8.14 s**, player
**8.30 s**, while `ifup@wlan0` / `network.target` reached **17.66 s**. The
picture now starts **9.4 s before the network exists**.

The remote-player path was verified against that late network: a unicast mDNS
query to the RX returned the full record including the address wlan0 only
acquired at 17.66 s, and a synthetic remote SUBSCRIBE took `subscribers` 1 → 2
with **238 telemetry datagrams / 230 KB** fanned out, `subs_rejected` = 0.
Loopback ingest was proven separately. Boot-to-picture: **66 s → 8.3 s**.

Guarded by a regression lint in `tests/agent/test_config_units.sh`, asserted
failing before the fix.

### RX-local player toggle

Dashboard toggle of `PLAYER_LOCAL` via `POST /api/apply`: ON in ~957 ms, OFF in
~947 ms, with `pivid.conf`, `is-enabled` and the unit state all tracking. Two
defects were found and fixed during validation: a crashloop when no display is
connected (~37 restarts — fixed by an `ExecStartPre` that blocks until an HDMI
connector reports `connected`, with `TimeoutStartSec=infinity`), and a dashboard
hang because that blocking `ExecStartPre` made `systemctl enable --now` block —
fixed by enabling with `--no-block`. With no display attached the unit now waits
quietly in `start-pre` with `NRestarts=0`.

### `rx_boost` on hardware

Portal apply 4 → 7 → 4 on `pivid-rx`: the `/sys` parameter, `modprobe.d` file and
`pivid.conf` all tracked, and the link was clean at both values — boost=7: 32k
NALs, 0 gaps; boost=4: 137k NALs, 0 gaps. Picture verified clean in both states.

**The sensitivity question could not be answered on the bench**: a full 30 dB
TX-power sweep only reached RSSI **−62 dBm**, roughly 37 dB above the floor, and
boost only helps a sensitivity-limited link.

---

## Encoder

### bcm2835 encoder parameter survey

Fourteen firmware parameters were proven **recognized but dead or useless**:
EEDE, `sei_enable`, `frame_limit_bits`, `peak_rate`, `separate_nal_bufs`,
ADAPTIVE and BOTH refresh modes, CYCLIC_MROWS, the HRD flags, and Main/High
profile with CBR.

The real levers are **slices** (spatial bound, near-free), **cyclic intra
refresh** (partial rolling heal, ~29.5 dB over ~15 frames, −39 % peak frame),
**short-GOP IDR** (the only arm giving full pixel-perfect recovery, at a steady
bandwidth cost), and **droppable-P** (converts 60–75 % of losses into one-frame
glitches).

### Clean-encode quality on real footage

100-frame real motion clip, 1.5 Mbps CBR, 3 slices, decoded on the same bcm2835
hardware decoder the RX uses:

| Recipe | Avg PSNR | Min frame | vs reference |
|--------|----------|-----------|--------------|
| CBR i30 (reference) | 35.75 dB | 31.67 dB | — |
| Intra-refresh | 33.78 dB | 23.00 dB | **−1.97 dB** |
| Droppable len4 | 35.48 dB | 31.64 dB | **−0.27 dB** |

**Droppable costs almost nothing** on real footage; **intra-refresh is the
expensive one**. An earlier synthetic-clip result claiming a +128 % droppable
bitrate penalty and a −13 dB refresh cost was a **snow-clip artifact** and does
not transfer.

Peak frame size was **~8.4 KB for every strategy** — 3.7× under the 32 KB ring.
An earlier 56 KB "anchor frame" figure was a fixed-QP artifact; under real CBR
the rate controller equalizes frame sizes.

### Hardware decoder under loss

| Injected | Frames out | Behaviour |
|----------|-----------|-----------|
| One slice of a **disposable** frame | 100/100, rc=0 | one frame ~14 dB, recovers next → 1-frame glitch |
| A **whole disposable** frame | 99/100, rc=0 | one fewer frame, no freeze, no DPB flush → judder |
| One slice of a **reference** frame | 100/100, rc=0 | smear over ~7 frames, then recovers |

**Every case returned rc=0 — the real hardware decoder never froze or errored.**
This was the central unknown, since all prior healing data came from software
decoders. Smear length is content-dependent: real motion self-heals faster than a
synthetic pan, which smeared for 44 frames.

### Structural facts established

- `pic_order_cnt_type = 2`, `gaps_in_frame_num_allowed_flag = 0`,
  `max_num_ref_frames = 1`. POC is derived from `frame_num`, so dropping a
  disposable creates **no POC gap** — provided only `nal_ref_idc == 0` NALs are
  ever dropped. That condition is *required*, not advisory.
- `frame_num` only advances on reference frames; disposables repeat the prior
  value. Verified on-box: `0, 1,1,1, 2,2,2,2, 3,3,3,3, 4,…`
- No packet ever bridges two NALs — the parser emits once per NAL and the
  chunker fragments one NAL into ≤253 B chunks.
- A multi-packet NAL requires `seq == expected_seq` on every continuation, so
  **one lost packet discards the entire NAL**. At ~74 packets per anchor slice
  and 1 % per-packet loss, P(losing the NAL) ≈ 52 %. Going from 3 to 9 slices
  takes that to ~25 packets and ~22 % — this is why slicing matters.
- A misaligned `number_of_mbs_in_a_slice` is **not rejected**; the firmware
  rounds the per-slice MB count up to whole MB-rows. For 640×480 (30 MB-rows)
  use 5, 6 or 10 slices — **not 9**.
- Stock `rpicam-vid` exposes no rate-control-mode flag, and behaves VBR-like: a
  target of 800k achieved ~1491 kbps on motion. At an adequate target, CBR and
  VBR land within ~1 % of each other.
- Slicing does **not** prevent ring overflow — it shrinks individual NALs but not
  the frame's total instantaneous burst. There is no TX pacer; a pacer would have
  to be a dedicated egress thread, never a blocking sleep in the push path, since
  the `pivid-tx` data path is single-threaded and would backpressure into stdin
  and stall `rpicam-vid`.

---

## Reference assets

**Motion reference clip.** Real hand-motion in front of the imx290, deliberately
high-motion so the encoder is bitrate-constrained. Raw I420, 640×480, 30 fps,
437 frames (~14.57 s); frame size 460 800 B; file 201 369 600 B; sha256
`83f1fd0ff3780fe289f2d022f7bf297c81fd400230eea02b3b2bd6cdf9d9fa26`. Kept on the
TX Pi at `/root/pivid-assets/` as both the raw file and a 55 MB lossless FFV1
`.mkv` that round-trips bit-exact. **Not in git.** Regenerate with:

```sh
ffmpeg -v error -i <ffv1>.mkv -f rawvideo -pix_fmt yuv420p ref.yuv
# then verify the sha256
```

---

## Measurement pitfalls

Each of these produced a wrong number at least once in this project.

- **ffmpeg PSNR frame pairing.** Pairing by timebase (H.264 `1/1200000` versus
  raw `1/25`) reads ~15 dB because only frame 0 aligns. Pair **by index**:
  `settb=AVTB,setpts=N` on both inputs.
- **Aggregate NAL count is blind to tail truncation.** A mid-frame split still
  emits roughly one NAL per frame, so NAL-count sweeps and residency histograms
  passed while the picture was visibly corrupt.
- **The `nal_resid` histogram is blind to the frame-trailer effect** — it counts
  the trailer's hold, not the slice's. Per-NAL arrival-gap capture disambiguates.
- **`fec_unrecoverable` counts are a counting artifact** across `m_ref` and
  `slice_mb`: configurations that create fewer protected blocks trivially have
  fewer blocks that *can* be counted unrecoverable. PSNR, or the live
  `fec_unrec_short` / `_present` split, is the decision metric.
- **Mean PSNR is blind to single concealed slices**, and worst-frame PSNR did not
  catch it either. Only the live `fec_unrec` root-cause split did.
- **Fixed QP masks rate-control features.** Cyclic intra refresh is a
  rate-control feature and no-ops under fixed QP — this produced a wrong
  "firmware-dead" verdict.
- **Byte-`cmp` alone is insufficient for encoder parameters.** Inspect the
  structural signal (`nal_ref_idc`, NAL-type histograms) and sweep companion
  parameters and GOP context.
- **Synthetic incompressible clips do not transfer.** Panned snow produced a
  +128 % droppable bitrate penalty and a −13 dB refresh cost; both were refuted
  on real footage.
- **Peak frame < ring size is necessary but not sufficient.** The binding
  transport limit is average bitrate versus average drain rate, and only a live
  TX run exposes it.
- **Sequential arms drift.** RF drifts several dB over minutes, so
  back-to-back configurations are not comparable. Interleave arms (A/B/A/B…) and
  match on *realized* loss, not the dial setting.
- **The fixed-grid block detector in the filmed-latency readout drifts** and
  mislabels blocks by exactly ±100 ms. Detect block centres per 1200-frame
  segment.
- **16-bit chip counters wrap**, and do not reset on an NRESET-pin reload. Use
  per-run deltas.
- **Never `pkill -f <pattern>` over ssh** — the pattern matches the ssh command's
  own argv and kills the session. Kill by exact saved PID.

---

## Campaign index

Labels that appear in code comments:

| Label | Campaign |
|-------|----------|
| BUG-1 | FE-calibration "bit 9 every Rx" — refuted |
| BUG-2 | RX FIFO drain over-read by 1 byte |
| BUG-3 | `pa_config` register spec-clean |
| BUG-4 | Sub-6-byte TX record guard (also IMP-8) |
| IMP-1 | Tail-of-frame latency: frame-trailer NAL + player AU assembly |
| IMP-2 | SPI turnaround: (a) PA ramp, (b) BUSY-wait spin, (c) 1 Hz FIFO flags |
| IMP-3 | `rx_boost` 7 vs 4 (open — see TODO.md) |
| IMP-4 | RX FEC early-close on all-k-data-present |
| IMP-5 | FLRC CRC width 16/24/32-bit |
| IMP-6 | Bit-rate "range gear" (open — see TODO.md) |
| IMP-7 | Player present-path push |
| IMP-8 | RX SIGHUP checked config loader; sub-6-byte TX guard |
