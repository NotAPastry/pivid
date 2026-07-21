# AQM — adaptive frame-drop on the TX ring

## What it is

The encoder can produce video faster than the radio can send it. When that
happens the 32 KB TX ring backs up, and once it is full the kernel tail-drops
whatever arrives next — which lands **mid-frame**, shredding a slice and leaving
visible block corruption at the viewer.

AQM is a gate in `pivid-tx` that prevents this by dropping **whole frames**
before the ring fills. A dropped frame is a clean skipped beat that the decoder
conceals in place; a shredded frame is corruption. The gate sits upstream of the
FEC packer, so a dropped frame costs no airtime at all.

It ships **on** (`AQM=enforce` in `pivid.conf`) and at the production bitrate it
drops nothing — it only engages when the link is oversubscribed.

Implementation: [`apps/pivid-tx/src/aqm.c`](../../apps/pivid-tx/src/aqm.c) /
[`aqm.h`](../../apps/pivid-tx/src/aqm.h).

## How it works

At each access-unit boundary the gate reads a congestion signal off the TX ring
and decides KEEP or DROP for that whole frame, based on the frame's class:

- **IDR** — never dropped, at any congestion level.
- **REF** (reference P) — dropped only in deeper congestion.
- **DISP** (disposable P) — shed first.

Two hysteresis bands drive a three-level state machine (`NONE` → `DISP` →
`REF`), so the gate does not chatter around a single threshold:

| Level | Enters at | Leaves at |
|-------|-----------|-----------|
| Drop disposables | occupancy ≥ 40 % | < 25 % |
| Also drop reference-P | occupancy ≥ 55 % | < 45 % |

The signal defaults to `peak` — the maximum occupancy seen since the last
decision. That matters because video arrives in 30 fps bursts: the ring fills on
each frame and drains between frames, so an instantaneous sample taken in a
trough reads near zero even while queueing delay is real. `peak` is immune to
that cycle.

### Why occupancy alone is not the whole story

From the baseline capture below: ring occupancy has a **median of 0 %**, while
record residency (enqueue → fully drained) has a **median of 14.4 ms**. The ring
genuinely reads empty at most 5 ms snapshots, yet records are still waiting
milliseconds to drain. Instantaneous occupancy systematically *hides* the
queueing delay.

This is why the trigger was derived from the time-domain cost and then expressed
as an occupancy watermark, rather than being guessed from occupancy directly.

## Configuration

The production knob is a single config key, exposed as an on/off toggle in the
dashboard:

| Key | Values | Default |
|-----|--------|---------|
| `AQM` (`pivid.conf`) | `0` / `enforce` | `enforce` |

- `0` — off. One branch in the NAL path, nothing else runs.
- `enforce` — drop the decided frames.

`settings_validate` rejects any other value, and
[`main.c`](../../apps/pivid-tx/src/main.c) maps anything that is not exactly
`enforce` to off.

Applying the key restarts the transmitter. The recipe class mapping tracks
`ENCODER_RECIPE` automatically (`aqm_recipe_from_encoder` in
[`main.c`](../../apps/pivid-tx/src/main.c)).

Tuning overrides stay environment-only:

| Variable | Values | Default |
|----------|--------|---------|
| `PIVID_AQM_SIGNAL` | `peak` / `inst` / `ewma` / `res` | `peak` |
| `PIVID_AQM_T_DISP_HIGH` / `_LOW` | percent of ring | 40 / 25 |
| `PIVID_AQM_T_REF_HIGH` / `_LOW` | percent of ring | 55 / 45 |

`res` and `inst` produce identical gate decisions (`est_res_ms` is derived from
`inst`); `res` is only a millisecond-unit logging alias.

## Instrumentation

`pivid-tx` carries a gated sampler thread
([`tx_sample.c`](../../apps/pivid-tx/src/tx_sample.c)) that measures the ring
from inside the daemon that owns the mapping. It is **dormant by default** — one
acquire-load branch in the `tx_push` hot path — and emits data only when
`PIVID_TX_SAMPLE` is set.

It has to live in-process: the driver enforces exclusive open on `/dev/radio0`
(`lr2021_open`'s `atomic_cmpxchg(&is_open,0,1)`), so a standalone sampler gets
`EBUSY` — verified on hardware — and per-record residency needs the in-process
enqueue timestamp regardless.

```sh
# systemd drop-in: /etc/systemd/system/pivid-tx.service.d/sample.conf
[Service]
Environment=PIVID_TX_SAMPLE=1
# optional: Environment=PIVID_TX_SAMPLE_CSV=/run/pivid/txsample.csv
```

Then `systemctl daemon-reload && systemctl restart pivid-tx`. Remove the drop-in
to go dormant.

**CSV output requires `PIVID_TX_SAMPLE=1`** — enforcement alone writes nothing,
so shipping `enforce` by default never grows the tmpfs CSV.

Columns: `kind,mono_ns,used,occ_pct,temp_mC,wake_skew_ns,res_seq,res_ns`

| `kind` | Meaning |
|--------|---------|
| `OCC` | 200 Hz occupancy sample: `used` bytes, `occ_pct` of the 32 KB ring, die temp (1 Hz), `wake_skew_ns` = actual − intended wake (the sampler's own jitter) |
| `RES` | One per record whose bytes have fully drained: `res_seq` (monotonic record number), `res_ns` (enqueue → drain) |
| `DROP` | Consumer was lapped; `res_seq` carries the cumulative dropped-sample count. Zero on a healthy capture |
| `AQM` | One per AU boundary (see below) |

`AQM` rows share the file and clock with the sampler:

```
AQM,<mono_ns>,<class>,<recipe>,<level>,<KEEP|DROP>,<signal_id>,<signal_pct>,<inst_pct>,<peak_pct>,<ewma_pct>,<est_res_ms>
```

`class`: 0=IDR 1=REF 2=DISP. `recipe`: 0=gdr 1=droppable 2=gop 3=fallback.
`level`: 0=NONE 1=DISP 2=REF. `signal_id`: 0=peak 1=inst 2=ewma 3=res.
Readers key on the `kind` column and tolerate the variable tail.

---

## Measured basis

The raw capture archives are not kept in the repo; the results below are the
distillation. Re-capture with `PIVID_TX_SAMPLE=1` and a bitrate staircase.

### Baseline — healthy link

TX Pi, live 480p30 `gdr` stream, ~600 kbps, FEC m2/k16, 70 495 rows.

| Metric | p50 | p90 | p95 | p99 | max |
|--------|-----|-----|-----|-----|-----|
| Ring occupancy (% of 32 KB) | 0 | 13 | 16 | 22 | 44 |
| Residency (ms) | 14.44 | 33.70 | 38.71 | 51.24 | 89.30 |

Sampler self-health: `wake_skew` mean 66 µs, max 1.6 ms, 0.2 % of ticks over
100 µs; **0 DROP rows**. Die temp ~53.7 °C.

### Axis decoupling

- **Bitrate / source rate → ring back-pressure.** This is the AQM axis. The
  source is the only knob that can outpace airtime, so it is the only one that
  fills the TX ring.
- **TX power → link margin.** FLRC airtime is fixed by the radio config, *not*
  by RF power, so lowering power degrades RX reception while leaving the TX-ring
  drain rate — and therefore residency — unchanged. **A power staircase cannot
  exercise the AQM trigger**; it is a separate RX-margin calibration.

### Bitrate staircase — the transfer function

TX Pi, `droppable`/VBR recipe, bitrate cap swept 600 k → 2.0 M, 30 s per step.
The VBR cap is the rate knob: on this scene the encoder has headroom well past
the drain, so produced rate climbs with the cap until the ring saturates.

The `gdr`/`gop` (CBR) recipes crash-loop on this rpicam fork, so the sweep uses
the working VBR recipe. The transfer function is a property of ring and drain
parameterised by *produced* rate, so the recipe is just a rate generator.

| Requested kbps | Produced kbps | `dropped_full` | occ % p50/p95/max | Residency ms p50/p95/p99/max | Regime |
|---|---|---|---|---|---|
| 600  | 651  | 0    | 0 / 16 / 39  | 14.4 / 37.3 / 45.3 / 78.9   | healthy |
| 800  | 852  | 0    | 4 / 29 / 55  | 18.6 / 63.8 / 83.4 / 107.6  | healthy |
| 1000 | 1041 | 0    | 8 / 32 / 61  | 22.2 / 68.1 / 87.9 / 118.6  | healthy |
| 1200 | 1226 | 20   | 30 / 65 / 97 | 64.3 / 129.1 / 155.6 / 193.4 | **knee** |
| 1500 | 1244 | 2346 | 89 / 98 / 99 | 176.7 / 192.6 / 195.9 / 200.5 | saturated |
| 2000 | 1242 | 4220 | 92 / 98 / 99 | 181.7 / 194.5 / 197.5 / 202.5 | saturated |

Three regimes:

- **Healthy** (≤1041 kbps, ρ<0.85): occupancy drains between frame bursts
  (p50 0–8 %); residency is jitter-dominated (p50 14–22 ms).
- **Knee** (~1226 kbps, ρ≈0.99): produced ≈ drain. Occupancy p50 jumps 8 → 30 %,
  residency p50 22 → 64 ms and p99 88 → 156 ms, a 7–10× slope increase.
  `dropped_full` first goes non-zero.
- **Saturation** (ρ>1): produced **plateaus at ~1244 kbps — the real TX-ring
  drain ceiling**. Occupancy pins at 90–99 %; residency pins at ~200 ms, which is
  `ring_capacity / drain` = 32 KB × 8 ÷ 1.244 Mbps = **211 ms** predicted against
  200–203 ms measured. That agreement validates the model.

### Deriving the trigger

For a 100 ms p99-residency budget, p99 crosses 100 ms between produced 1041
(p99 = 88 ms, occ p95 = 32 %) and 1226 (p99 = 156 ms, occ p95 = 65 %). The
relation `residency ≈ occupancy_bytes / drain` at the 1.244 Mbps ceiling puts
100 ms at **~15.5 KB ≈ 47 %**; staying clear of the knee's variance argues
lower — hence **40 % (~13 KB)**, which matches the a-priori estimate of
100 ms × 1.24 Mbps ≈ 12.5 KB ≈ 38 %.

**The key design constraint:** `dropped_full` only goes non-zero *after*
residency has already blown past 100 ms — 156 ms at the first drop. The kernel's
native ring-full tail-drop is therefore **too late**, and it shreds mid-frame.
The gate must fire proactively at ~40 % occupancy, before the ring fills.

### Signal selection

Offline join of `AQM` rows to the following `RES` rows on `mono_ns`, labelling an
AU congested if its records' residency later exceeded the 100 ms budget, pooled
over non-IDR AUs:

| Signal | Watermark | Recall (>100 ms caught) | Over-drop (healthy dropped) |
|--------|-----------|------------------------|-----------------------------|
| `peak` | 40 (single) | 0.967 | 0.098 |
| `peak` | 40/55 (two-level) | 0.940 | 0.084 |
| `inst` | 35 | 0.959 | 0.058 |
| `ewma` | 40 | 0.939 | 0.042 |

`peak` was chosen over `inst`/`ewma` because it degrades more gracefully at
higher thresholds (peak at 55: recall 0.855, versus `inst` 0.754) and is immune
to burst/trough noise. Its **one-frame lag is benign** — `peak[N] ≥ 40` catches
congestion at AU N (0.967), N+1 (0.981, mildly predictive) and N−1 (0.962) about
equally, because congestion is sustained across frames.

### Gate off versus gate on

Same sweep, `droppable`/VBR, FEC m3/k16. The "off" column was captured with the
gate computing and logging its decision but not suppressing, so both columns
carry identical traffic — that log-only mode has since been removed, and an
equivalent capture today would use `AQM=0`.

| Requested kbps | `dropped_full` off → on | Residency p99 ms off → on | occ % p95 off → on | AQM drop % |
|---|---|---|---|---|
| 600 (production) | 0 → 0 | 59 → 61 | 22 → 17 | 0.1 |
| 800 | 0 → 0 | 99 → 87 | — → 26 | 2.7 |
| 1000 | 0 → 0 | 106 → 92 | — → 30 | 3.3 |
| 1200 (knee) | 15 → **0** | 169 → **101** | — → 36 | 3.9 |
| 1500 | 2344 → **0** | 197 → 113 | — → 45 | 20 |
| 2000 | 6280 → **0** | 196 → 142 | 99 → 62 | 39 |

`dropped_full` reaches **0 at every step** — the ring never fills to the kernel
tail-drop. Residency flattens to roughly the budget at the knee, and at the
production 600 k rate the gate drops **0 frames**, leaving production untouched.

Deep (≥2×) oversubscription at 1500–2000 k stays above 100 ms; that regime
physically needs heavier dropping and is outside the operating envelope. IDR is
never dropped at any step; disposables shed first, reference-P only in deeper
congestion.

**Verified end-to-end at the viewer, not just in counters:** at the 1.2 M knee
with the gate on, the decoded video is clean and watchable — the software decoder
conceals each dropped frame in place, a clean skipped beat with no smear. With
the gate not suppressing at the same 1.2 M, so that `dropped_full` climbs and the
kernel tail-drops mid-frame, the picture shows visible block corruption.

## Re-tuning

The shipped watermarks were derived for this operating point: a 32 KB ring, a
~1.244 Mbps drain, 30 fps, and the `droppable`/VBR recipe. Change the FLRC
bitrate or coding rate, the ring size, or the frame rate and the right trigger
moves with them.

Re-deriving them means capturing `AQM` and `RES` rows with `PIVID_TX_SAMPLE=1`
across a bitrate staircase, then joining each `AQM` row to the `RES` rows that
follow it on `mono_ns` and checking, for each candidate signal and watermark,
whether the frames it would drop are the ones whose residency later exceeded the
budget — without over-dropping healthy frames. Set the result via
`PIVID_AQM_SIGNAL` and the `PIVID_AQM_T_*` overrides.

Note the gate enforces while you measure: there is no log-only mode. A
suppressed frame never reaches the ring, so its would-be residency is not
observable. To measure the unmitigated curve, set `AQM=0` for the capture.
