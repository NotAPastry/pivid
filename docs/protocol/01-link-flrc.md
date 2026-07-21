# 1. FLRC Chip Container (link layer)

The bottom layer is the LR2021 radio's **FLRC** (Fast Long Range Communication)
packet. The app never builds this frame directly — it hands a built outer
protocol packet (≤511 B) to the kernel driver, which loads it into the chip FIFO;
the chip adds the preamble, sync word, and CRC on air. On RX, only **CRC-valid**
records are delivered up to the app.

## On-air configuration (production)

| Parameter | Value | Source |
|-----------|-------|--------|
| Modulation | FLRC | [lr2021_flrc_cfg.h](../../drivers/lr2021/lr2021_flrc_cfg.h) |
| Bitrate / BW | 2.6 Mbps (fastest) | `LR2021_FLRC_BITRATE_BW = 0x00` |
| Coding rate | **CR 3/4** (chip convolutional FEC + interleaving, ~8 dB coding gain) | `cfg->cr` |
| Max payload | **511 B** | container ceiling |
| Carrier | **2.1 GHz** default (S-band; on the 4 MHz FE-cal grid). Configurable via `RF_FREQUENCY`; must match on both ends | [lr2021_core.c](../../drivers/lr2021/lr2021_core.c) / [settings.c](../../common/settings.c) |
| Sync word | 32-bit, derived from the binding phrase (SHA-256, first 4 bytes big-endian, forced nonzero) | [phrase_sync.c](../../common/phrase_sync.c) |
| CRC | chip-appended; invalid frames dropped before the app | driver |

**Coding-rate decision is LOCKED at CR 3/4** — the chip FEC stays on. CR 1 would
buy ~25–33 % airtime but costs ~8 dB of coding gain; the link is loss-limited,
not bandwidth-limited, so that trade is the wrong direction. See
[DECISIONS.md](../DECISIONS.md).

## Chip framing: DYNAMIC, pinned

The chip transmits **variable-length** FLRC packets: only the real bytes go on
air and the chip emits a 9-bit on-air length header, which the receiver recovers
via `GetRxPktLength`. Short packets (the last fragment of every NAL) therefore
do not pay for 511 B of padding airtime. The mechanism is confirmed on hardware;
its cost is one extra `SetFlrcPacketParams` SPI per packet, roughly 30–50 µs (see
[MEASUREMENTS.md](../MEASUREMENTS.md)).

This is **hardwired** in the driver (`flrc_cfg.mode = 0` at probe,
[lr2021_core.c](../../drivers/lr2021/lr2021_core.c)). The legacy FIXED chip mode
(`pkt_format=1`, 511-B zero-padded) is **pinned off**: there is no `flrc_mode`
module parameter and no `FLRC_MODE` config knob. The low-level dual-mode encoder
(`lr2021_flrc_encode_params`) still understands both `pkt_format` values. The
fixed path survives only as a defensive fallback in `lr2021_init.c`: an
out-of-range `pld_len` clamps the whole config back to fixed-511/CR-3/4. That is
unreachable with the shipped default (`flrc_pld_len = 511`).

> ⚠️ **Live reload wedges the chip.** Changing `flrc_cr` (or otherwise re-keying
> FLRC framing) mid-stream wedges chip framing — only `modprobe -r` / `modprobe`
> clears it. This is why the config-agent has a dedicated "modreload" tier. (FEC
> `m` live-reload via SIGHUP is fine; the FLRC *framing* params are not.)

## Outer protocol framing stays FIXED — on purpose (the LE16 is load-bearing)

The **chip** framing is DYNAMIC, but the **app's outer protocol header**
(`proto_mode_t`) for video/FEC packets is `PROTO_MODE_FIXED`:
`[flags(1)][len(u16 LE)(2)][payload]`. This is **deliberate, not legacy** — the
2-byte LE16 length is **load-bearing for the RX driver's SPI frame-sync**.

The kernel reads packets out of the chip's RX FIFO over SPI and can pick up a
leading **stray byte** (a bcm2835 DMA-jitter artifact, ~1.1 % of packets). To
decide per packet whether the real data starts at offset 0 or 1, the driver
matches the LE16 length against the chip-reported `pkt_len − 3` — a 16-bit exact
check, so a false collision is ~1/65536
([lr2021_irq.c](../../drivers/lr2021/lr2021_irq.c)). This picks the true offset
per packet even where the cheaper 1-byte heuristic is ambiguous, which the
in-code measurement puts at ~99.6 % of packets. DYNAMIC framing has
no such field, so video would fall to a weak heuristic and misframe — proven
**57–88 % ambiguous** on the clean path by an exhaustive offline spike. So FIXED
is kept for video/FEC; the ~0.3 % airtime the 2 bytes cost is noise on a
loss-limited link. Full analysis in
[docs/MEASUREMENTS.md](../MEASUREMENTS.md) ("DYNAMIC video frame-sync").

## The mode-mismatch landmine — RESOLVED (was historically)

Historically the `FLRC_MODE` chip knob was unwired to the protocol mode and could
be flipped independently, so the protocol-FIXED-over-chip-DYNAMIC arrangement
"worked by coincidence" and any chip-side flip surfaced as **silent mass
`drops_len`** (and would *also* break the LE16 frame-sync — a fixed-511 chip makes
`pkt_len ≠ payload+3`).

This is **eliminated by construction**: the chip is **pinned DYNAMIC** (knob,
module param, and the dead `tx_set_flrc_config` / `rx_set_flrc_config` wrappers all
removed), so the chip framing can no longer drift from the protocol's
expectations. The protocol header staying FIXED is now the single, documented,
permanent arrangement — and the LE16 it carries is exactly what the frame-sync
needs.
