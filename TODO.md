# TODO

Open work only, ranked by expected gain. Settled decisions live in
[docs/DECISIONS.md](docs/DECISIONS.md) and measured results in
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) — check both before starting
anything here, because several plausible directions are already closed.

Each item notes its gate: **research** (needs study before it can be built),
**bench** (testable on the desk), or **flight** (needs a real range outing).

---

### 1. Bit-rate "range gear"

**Gain:** a guaranteed +2.5 dB link margin (2.6 → 1.3 Mbps) when range is the
binding constraint — larger than any RF tweak available.
**Cost:** half the throughput, plus real protocol design.
**Gate:** research, then pre-flight.

The hard part is switching `bitrate_bw` on a one-way link with no ACK. Dual
sync-word versus a timed switch needs study before anything is built. Note that
the intermediate step is not worth it: 2.08 Mbps buys only +1 dB for a 20 %
throughput loss.

### 2. Re-test `rx_boost` 7 vs 4 at the range limit

**Gain:** ~0.5 dB sensitivity.
**Gate:** flight, for the sensitivity half.

Datasheet rev 2.1 quantifies the non-boost penalty directly (table 3-8
`LOSS_S_BOOST` = 0.5 dB HF, 1 dB LF) and the RX-current cost (`IDDDELTA2` =
+0.4 mA). The bench cannot settle it — boost only helps a sensitivity-limited
link, and a full 30 dB TX-power sweep only reached RSSI −62 dBm, roughly 37 dB
above the floor. Keep whichever value holds PER longer at range.

The **overload half is bench-able** and worth doing first: a strong-signal plus
co-located-interferer A/B at `boost=7` would clear or damn a default flip before
any range outing. See the `rx_boost` entry in
[docs/DECISIONS.md](docs/DECISIONS.md) for why 7 is not simply the default.

Field pre-work is done: `rx_boost` is portal-configurable (`RX_BOOST`, RX
Advanced page, modreload tier, offers 4–7), so no driver rebuild is needed in the
field. The sensitivity probe harness still needs to be written as a tracked
script under `scripts/`.

### 3. FLRC sync-word quality audit

**Gain:** unknown, possibly zero — only worth doing if false sync is ever
observed in the field.
**Gate:** research.

The sync word is 4 hash bytes with only a nonzero guard
([phrase_sync.c](common/phrase_sync.c)). Measure false-sync rate against
adversarial words (`0x01010101`, `0xAAAAAAAA`, low-transition patterns) and
decide whether a quality filter is warranted.

### 4. pivid-player full Windows port

**Gain:** platform reach only — no link or mission gain.
**Gate:** large effort; deferred.

All five socket files are POSIX-only: `telem_recv.c`, `control_client.c`,
`mdns_browse.c`, `video_decode.c`, `lan_source.c` (`poll.h` needs `WSAPoll`).
A single `net_compat` shim has to cover **all five** — doing one leaves the link
broken. Toolchain: MSYS2 with MinGW-w64.
