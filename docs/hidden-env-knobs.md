# Hidden environment knobs

These are intentionally **not** shown in any `--help`. They are debug,
instrumentation, or advanced-tuning switches, read directly with `getenv()` (or,
for the AQM watermarks, via the `env_pct()` helper). They bypass the `Settings`
schema on purpose, so they can be flipped in the field without a rebuild or a
config-schema row.

Ordinary runtime configuration that is not listed here lives in `pivid.conf`
(the daemons, read via `--config`) or in the player's command-line switches.

This list is exhaustive as of the current tree — it is every `getenv()` in
`apps/` and `common/`.

## pivid-tx

| Env var | Purpose | Default |
|---|---|---|
| `PIVID_TX_DROP_PCT` | DEBUG: clean pre-transmit on-air loss injection (1–100) | off (0) |
| `PIVID_TX_SAMPLE` | Enable CSV instrumentation sampling. Also gates AQM CSV rows | off |
| `PIVID_TX_SAMPLE_CSV` | CSV output path | `/run/pivid/txsample.csv` |
| `PIVID_AQM_SIGNAL` | AQM tuning: congestion signal (`peak`/`inst`/`ewma`/`res`) | `peak` |
| `PIVID_AQM_T_DISP_HIGH` / `_LOW` | AQM tuning: disposable-drop watermarks (% of ring) | 40 / 25 |
| `PIVID_AQM_T_REF_HIGH` / `_LOW` | AQM tuning: reference-drop watermarks (% of ring) | 55 / 45 |
| `PIVID_RPICAM_BIN` | Test override for the `rpicam-vid` binary path | `rpicam-vid` on `PATH` |

## pivid-rx

| Env var | Purpose | Default |
|---|---|---|
| `PIVID_RX_DROP_PCT` | DEBUG: post-CRC loss injection for FEC tests (1–100) | off (0) |

## pivid-player

| Env var | Purpose | Default |
|---|---|---|
| `PIVID_PLAYER_IDLE_FLUSH_MS` | True-stall flush threshold. `0` disables; clamped to 500 max | 250 |
| `PIVID_RENDER_FPS` | Render/poll loop rate. Finer than the video rate only lowers present latency | 60 |
| `PIVID_PRESENT_BUFFER` | Presentation jitter buffer, in frames (clamped to 6). Costs ~N×33 ms latency; absorbs SW-decode jitter on a Zero 2 W | 0 (off) |
| `PIVID_DECODE_THREADS` | SW decode thread count. Capping lowers memory-bus contention with the SPI-DMA radio read on the RX Pi | unset = libavcodec auto (per-core) |
| `PIVID_DECODE_THREAD_TYPE` | Set to `frame` to A/B frame threading against the default slice threading | slice |

> **Do not lower `PIVID_PLAYER_IDLE_FLUSH_MS`.** Anything below ~100 ms sits
> inside the RX FEC block-hold and fires mid-frame, which was the primary cause
> of still-scene bottom-half corruption. See
> [DECISIONS.md](DECISIONS.md#encoder-and-player).

## Not env knobs (common confusions)

- **AQM mode** is the `pivid.conf` key **`AQM`** (`0` / `enforce`), carried
  through `Settings` — not an environment variable, and note the key has no
  `PIVID_` prefix. See [aqm/README.md](aqm/README.md#configuration).
- **AQM recipe** is always derived from `ENCODER_RECIPE` in C
  (`aqm_recipe_from_encoder`); there is no separate recipe knob.
- **Player hardware decode** is the `--hw` command-line switch. Software decode
  is the default and there is no `--sw` switch — just omit `--hw`.
- **Config file path** is the `--config <path>` switch on every daemon.

## Removed (no longer read)

- `PIVID_AQM_RECIPE` — the recipe now always derives from the encoder recipe,
  eliminating the mismatch footgun.
- `PIVID_CONF` — replaced by `--config <path>`.
- `PIVID_FEC_M_ANCHOR` / `PIVID_FEC_M_REF` / `PIVID_FEC_K` /
  `PIVID_SPS_REINJECT_MS` — now `pivid.conf` keys read through `Settings`.
- `PIVID_PLAYER_HW` / `PIVID_PLAYER_SW` — replaced by the `--hw` switch.
