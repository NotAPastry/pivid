# Settled decisions

Directions that were investigated, decided, and closed. Each entry says what was
tried, what the evidence was, and what the decision is. **Do not re-open one of
these without new evidence** — most were settled by hardware measurement after a
plausible code-reading hypothesis turned out to be wrong.

The supporting numbers live in [MEASUREMENTS.md](MEASUREMENTS.md).

---

## FEC and video quality

**`FEC_M_REF = 2`, not 1.** With `m_ref=1` a bursty erasure loses `m+1` symbols on
reference blocks, RS decode fails, and the H.264 decoder conceals the slice —
visible as blocky lines. Raising it to 2 drove `fec_unrec` from ~12/min to 0 for
about +4 % airtime. `m_ref=3` was not tried; 2 fixed the visible defect. Offline
PSNR sweeps were blind to this failure mode and had briefly justified dropping to
1 — a false economy paid for in artifacts.

**FEC is always on (`m >= 1`).** Production links are lossy; the config layer
rejects `m_ref = 0`.

**FEC column interleave is inert at the sliced production config.** `fec_tx` only
column-interleaves equal-`(k,n)` held blocks, but sliced video produces one small
block per NAL with varying `k`, so the interleaved path never fires — interleave
on and off produce byte-identical elementary streams. Harmless to leave enabled;
it engages only for single-slice large-NAL profiles. Making it effective would
need cross-NAL/class block pairing; shelved as low-ROI.

**fecsim: keep the defaults; the block/timing interleaver does not ship.** No knee
in the sweep. `slice_mb=120` ranks best under loss and is what production uses.

**"Motion artifacts" are encoder starvation, not RF.** Bisection showed VBR
encoder starvation (bitrate cap → QP spikes) plus a ~1–2 % SPI floor; Pi and Mac
decode byte- and pixel-identically. The lever is bitrate, not the radio.
The video budget is 1070 kbps and the encoder runs at 1000 kbps — it fits, but
the margin is thin. **If motion artifacts return, check `dropped_full` on the TX
ring before blaming RF.** VBR spikes over budget were the historical signature.

## FLRC and radio

**Keep CR 3/4** (chip FEC on). CR 3/4 → 1 saves 25–33 % airtime but costs 8 dB of
coding gain — the wrong trade on a loss-limited link. The CR-1 re-test was
cancelled.

**Keep `rx_boost = 4`.** Boost only helps a thermal-noise-limited link, and a
close-range bench cannot create one: a full 30 dB TX-power sweep only reached
RSSI −62 dBm, ~37 dB above the floor. Datasheet rev 2.1 pins the stakes —
non-boost costs 0.5 dB HF (`LOSS_S_BOOST`), boost costs +0.4 mA (`IDDDELTA2`).
Sensitivity tables are conditioned on boost = 7, but blocking, ACR and co-channel
rejection are tabulated non-boosted, so boosted-mode linearity is uncharacterized.
**Do not blind-default to 7 in a dirty-RF environment** — a strong-signal plus
co-located-interferer bench A/B must clear it first. The knob is portal-
configurable (`RX_BOOST`, RX Advanced page, modreload tier, offers 4–7), so no
driver rebuild is needed to try it in the field.

**Outer framing: DYNAMIC is pinned, and the LE16 length field is load-bearing.**
It is what lets the RX driver's per-packet SPI frame-sync disambiguate the leading
stray byte (an exact `pkt_len-3` match), absorbing ~1.1 % strays. **Do not
reopen** — full-LE16 and FLRC_MODE variants misframe 57–95 %. The stray itself is
bcm2835 DMA jitter. The chip is pinned DYNAMIC so the two cannot drift apart.

**The `frame_sync` single-byte offset is intentional.** Verify it with a packet
generator; do not "fix" it from code-reading. Misframe bugs here recur — measure,
do not hypothesize.

**A live `flrc_cr` change wedges the chip** (`k_len` climbs; only
`modprobe -r` / `modprobe` clears it). This is why the `modreload` apply tier
exists. FEC live-reload is fine.

**RX wedge recovery is chip-counter-gated.** Recovery is skipped only when the
chip is provably quiet: `GetFlrcRxStats` counters frozen, mode still RX, and RX
FIFO empty. Everything else recovers as before and logs a self-classifying
diagnosis line. Field override: `wedge_chip_gate=0`. Accepted residual: a
frozen-and-empty-and-in-RX genuine stall would be stranded — never observed, and
it would surface as a burst longer than 111 packets (all historical bursts ended
at the wedge cap). Do not re-open the gate design without such a burst.

**FLRC CRC width default = 16-bit; 24 and 32 are one modprobe.d flip away.** Cost
is measured and pinned (see MEASUREMENTS.md) — do not re-measure it. A mismatch
between ends is a dead link, so flip both ends in one operation. Flip gate:
flight-like `flrc_chip_crc_err > 0`, or a second `payload_mismatch` cluster; then
prefer 32-bit over 24 and recalibrate the capacity model.

**RF carrier default = 2100 MHz.** It snaps to the LR2021 4 MHz FE-cal grid
exactly (2100/4 = 525), so the no-arg `CalibFE` calibrates dead-centre. A
historical 2028 vs 2100 MHz A/B found no difference.

> ⚠️ **2100 MHz is below the external SZM-2166Y PA's rated 2.3–2.7 GHz band.**
> The carrier was previously 2300 MHz specifically to sit in that band, with an
> antenna matched for 2.3 GHz. Off-band operation means unspecified gain, input
> and output match, and linearity, and a 2.3 GHz antenna is mismatched at
> 2.1 GHz — which reflects power back into the amplifier. **If the SZM-2166Y is
> fitted, bench-verify output power and spectrum at 2100 MHz before
> transmitting**, or set `RF_FREQUENCY` back into 2.3–2.7 GHz (2300 MHz and
> 2500 MHz are both on the 4 MHz grid). Both ends must match or the link is
> dead.

**TX PA hardware is settled — do not re-chase.** External **SZM-2166Y** module
(Qorvo SZM-2166 die): ~36 dB gain, output P1dB ~35 dBm, rated 33 dBm / 2 W, band
2.3–2.7 GHz. **The module's DC input is rated to 30 V** — the 8–23 V figure in
circulation is the bare die's Vcc, not the module input. It runs from a 6S pack
(25.2 V at full charge), within rating, no BEC needed. Heatsink fitted; 2 W output
is confirmed adequate. Do not re-measure PA output or re-question the supply
voltage.

**TX power default −3 dBm.** That drives the SZM-2166Y to its rated ~33 dBm / 2 W,
~2 dB below P1dB and well under the amp's ~−1 dBm input compression. **Do not
raise `rf_tx_power_dbm` above −3** — input overdrive means splatter and possible
damage.

**PA ramp default 0x03 (16 µs); the safety margin comes from the fast BUSY-wait,
not from the ramp.** Ramp time doubles as RX FIFO drain headroom at 511 B
saturation, and it is paid roughly twice per packet (up and down) — budget 2×Δ,
not Δ. Spectrum and ACP are unchanged ±1 dB down to 16 µs but were **not**
verified below it; do not default under 0x03 without re-running the RSSI sweep.
If the SPI wait path ever slows, re-prove drain by watching `bad_header`,
fragments and `rx_walk_drops` — **not** CRC. Recalibrate `link_capacity.c`
`OV_PER_PKT_US` against a fresh TXPERF `period` after any hot-path change.

**`rx_fifo_overflow` is gated to real signal.** It was firing on a benign
511 > 256 B drain that lost zero bytes. Do not retune the RX-high threshold.

**RX RSSI drifts with temperature** (~−30 dBm cold to −50…−66 dBm warm, and
recovers). Benign at ~50 dB margin unless `crc_drops` track it.

**The "FE-cal bit 9 every Rx" hypothesis is refuted on hardware.** The chip does
not raise `RXFREQ_NO_FE_CAL_ERR` at 2.03/2.1 GHz, and round-to-nearest slot 508 is
accepted for 2030 MHz. A positive control (skipping CalibFE) proved the detector
works. No code change was needed. Do not re-chase it.

**PIO FIFO reads are rejected.** `fifo_read_pio=1` is ~3× worse than DMA under
production load; DMA stays the default. The knob is kept only as a documented
dead end.

## Encoder and player

**CBR recipes need Baseline profile.** The `gdr` / `gdr_idr` / `gop` recipes fail
(`ret -3`) on Main or High profile with CBR — a firmware incompatibility. The
profile is therefore recipe-dependent, in `camera_argv.c`.

**TX idle-flush is unsafe.** `nal_parser_finish` emits unconditionally mid-slice,
which corrupts the picture — this is not an RF problem. The frame-trailer NAL is
the safe clean-boundary flush, and it is what shipped.

**Player idle-flush is a 250 ms true-stall fallback — never lower it.** Anything
below ~100 ms sits *inside* the RX FEC block-hold: a `k=16` block fills across
several small still-scene frames, so tail slices legitimately arrive ~100 ms late.
A low threshold fires mid-frame 1–3 times a second and was the primary cause of
still-scene bottom-half corruption. Latency is owned by trailer-triggered AU
emit — the buffer is empty whenever a flush could legitimately fire. The
"lower it to ~15 ms" idea is refuted; do not re-run that A/B.

**The player defaults to software decode.** VideoToolbox tears down and rebuilds
its session on every damaged picture, which goes black on a lossy link. Pass
`--hw` to opt in on clean streams; there is no `--sw` switch, just omit `--hw`.

**The RX player must pin `SDL_RENDER_DRIVER=opengles2`** (set in
`target/etc/systemd/system/pivid-player.service`). Auto-selection can land on a desktop-GL backend
whose fixed-function calls (`glEnable(GL_TEXTURE_2D)`, `glTexImage2D` with
`GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV`) are invalid on the Pi's GLES-only
Mesa/vc4 — every texture upload fails and SDL presents an empty buffer, i.e. a
**black screen**. The diagnosis is nasty because everything else looks perfect:
frames decode, `chunks_dropped=0`, `nals_partial=0`, CRTC `active=1`, page-flips
run, and the player logs nothing ([render.c](../apps/pivid-player/src/render.c)
silently renders black when `ensure_video()` fails). Only `MESA_DEBUG=1` reveals
it. This surfaced on a fresh SD image while the same binary worked on another Pi,
so **do not debug the player code — check the render backend first.** Same class
of root cause as a missing EGL/Mesa runtime.

**GDR cannot fast late-join** (the firmware does refresh XOR IDR), so `gdr_idr`
uses periodic IDR instead. The dashboard hides `PIVID_INTRA` for `gdr_idr`.

## Platform

**GPU usage % is not obtainable on this Pi.** The VideoCore firmware is closed and
the codec block is not V3D. Do not re-investigate.

**A `config_schema` row auto-renders but does not auto-apply.** Every new config
key also needs a `Settings` backing field (about six sites) or it is a silent
no-op.

**DietPi masks logind**, so `systemctl reboot` fails with "File exists" — use
`systemctl --force reboot`.

**Nothing in the rx → caster → player chain may be ordered after
`network.target`.** That path is loopback-only, but `network.target` here is
gated by `ifup@wlan0` running wpa_supplicant and dhclient *synchronously* with a
5-minute timeout. Ordering after it trades the HDMI picture for WiFi association:
measured 60.2 s with WiFi up, and about 5 minutes with no AP in range. Nothing in
the chain needs an interface at start — ingest binds 127.0.0.1, ctrl binds
`INADDR_ANY` (wildcard binds cover interfaces created later), the fan-out socket
is deliberately unbound so the kernel picks source and egress per `sendto()`, and
mDNS uses `AVAHI_IF_UNSPEC` with `AVAHI_CLIENT_NO_FAIL` so avahi-daemon does the
interface tracking (it demonstrably starts loopback-only and adds wlan0 later on
every boot). Note that `After=network.target` never implied connectivity anyway —
that is `network-online.target` — so when ifup times out, `network.target`
activates regardless and a no-IP start was always reachable. Guarded by
`tests/agent/test_config_units.sh`.

**mDNS multicast may be blocked by the access point.** On the AP used during
development, browsing `_pivid._udp` and `_workstation._tcp` from a Mac returns
nothing and `ping pivid-rx.local` fails, so a *browse*-based check proves nothing
on such a network. A **unicast** mDNS query bypasses it and interrogates the Pi's
avahi directly:

```sh
dig -p 5353 @<rx-ip> _pivid._udp.local PTR
```

Also note `avahi-browse` is not installed on the Pis, and `avahi-daemon` there
warns `No NSS support for mDNS`, so `.local` does not resolve *from* a Pi. Likely
AP client isolation. Verify autodiscovery before relying on it in the field.

---

## Engineering constraints

Rules that hold across the codebase, each learned the hard way.

- **C for aarch64:** no `uint8_t*` → `uint32_t*` casts (alignment UB); use
  `size_t` for sizes; dispatch mmap on `vm_pgoff`.
- **Use `_GNU_SOURCE`, not strict `_POSIX_C_SOURCE`.** The latter hides
  `__USE_MISC` socket options such as `SO_RXQ_OVFL`, and only an on-Pi run
  exposes the gap.
- **LR2021 SPI response widths: the datasheet tables are authoritative.** Never
  assume one byte — `GetRxPktLength` is 2 B MSB-first. In multi-field byte cells
  the stacked fields read **top = MSB** (hardware-proven via the table 18-4 CR
  high nibble and the table 7-2 `rx_path` bit 0). The AN1200.104 `SetPaConfig`
  examples contradict the datasheet; do not use them.
- **Kernel SPI callbacks belong in `delayed_work`, not `timer_list`.** Timer
  callbacks run in softirq, and `lr2021_spi_cmd` plus `cmd_mutex` sleep — that is
  a BUG splat.
- **Always verify the decoded picture end-to-end, not just Pi counters.** The
  decoded video has repeatedly caught corruption that counter sweeps (NAL-count
  comparisons) missed.

## Development environment

- **The Pis have no native compiler.** DietPi ships no gcc or aarch64-gcc.
  Cross-build on the host or in Docker (`make …` → `bin/`) and copy the ARM ELF.
- **Before a Docker `make tests`,** clear stale binaries with `git clean -f
  tests/` — **not** `rm -f tests/**/test_*`, which deletes tracked sources and
  `.sh` files. Remove stale macOS Mach-O test binaries first as well.
- **Never `pkill -f <pattern>` over ssh** — the pattern matches the ssh command's
  own argv and kills your session. Kill by exact saved PID.
- **Prefer `.local` FQDNs** (`pivid-tx.local`, `pivid-rx.local`) over raw IPs,
  which rotate.
- **`deploy.sh` preserves an existing `pivid.conf`**, so default changes only
  reach fresh installs or a Reset. `webui.h` is generated from `index.html` by
  `scripts/embed_asset.sh`. An agent-only deploy is a copy plus a restart of
  `pivid-config-agent`.
