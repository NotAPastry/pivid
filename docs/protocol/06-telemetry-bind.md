# 6. Telemetry Record & Bind Packet

Two small fixed-layout records round out the protocol: the **telemetry record**
(flight/link values relayed off the MSP path) and the **bind packet** (the
pre-config handshake that establishes the link sync word).

## Telemetry record (35 bytes, version 2)

`struct pivid_telemetry` is packed/unpacked by
`pivid_telemetry_pack` / `pivid_telemetry_unpack`
([protocol.c](../../common/protocol.c),
[protocol.h](../../common/protocol.h)). It is a flat
little-endian record; `unpack` rejects a wrong `version`.

> Wire history: `PKT_OSD_GRID` (TYPE=1) reuses the bit that was once
> `PKT_TELEMETRY`. The MSP DisplayPort OSD relay ([05-osd-grid.md](05-osd-grid.md))
> is now the on-air carrier for displayed values; this 35-byte record is the
> structured telemetry exchanged on the control/MSP relay path (see
> `telem_tx.c` / the RX MSP receive-health dashboard), not a raw RF video packet.

Layout (`TELEMETRY_BYTES = 35`, `TELEMETRY_VERSION = 2`):

| Offset | Size | Field | Type |
|--------|------|-------|------|
| 0 | 1 | `version` | u8 (= 2) |
| 1 | 1 | `fc_link_state` | u8 |
| 2 | 1 | `voltage_state` | u8 |
| 3 | 2 | `vbat_mv` | u16 LE |
| 5 | 2 | `mah_drawn` | u16 LE |
| 7 | 2 | `amperage_ca` | i16 LE |
| 9 | 2 | `msp_rssi` | u16 LE |
| 11 | 2 | `throttle_us` | u16 LE |
| 13 | 4 | `altitude_cm` | i32 LE |
| 17 | 2 | `roll_decideg` | i16 LE |
| 19 | 2 | `pitch_decideg` | i16 LE |
| 21 | 4 | `arming_disable_flags` | u32 LE |
| 25 | 4 | `flight_mode_flags` | u32 LE |
| 29 | 1 | `beeper_state` | u8 |
| 30 | 4 | `tx_video_packet_total` | u32 LE |
| 34 | 1 | `ctl_lq_pct` | u8 (0..100; `CTL_LQ_NA = 0xFF` ⇒ "--") |

`CTL_LQ_NA = 0xFF` is the "no data" sentinel for the control-link LQ percentage
(0 is a real reading = total loss), [protocol.h](../../common/protocol.h).

## Bind packet (8-byte payload, always FIXED)

Bind packets establish the link before normal config is exchanged, so they are
**self-describing FIXED-mode** regardless of the running link mode: the receiver
can always read the 3-byte `[flags][len16]` header and require `len == 8`
([protocol.c](../../common/protocol.c),
[protocol.h](../../common/protocol.h)).

```
[ flags(1) ][ len16 = 8 ][ session_id(u32 LE) ][ sync_word(u32 LE) ]
            └────────────── PROTOCOL_BIND_PAYLOAD_LEN = 8 ──────────┘
```

| Offset (in packet) | Size | Field | Type |
|--------------------|------|-------|------|
| 0 | 1 | `flags` | u8 |
| 1 | 2 | `len` | u16 LE (= `PROTOCOL_BIND_PAYLOAD_LEN` = 8) |
| 3 | 4 | `session_id` | u32 LE |
| 7 | 4 | `sync_word` | u32 LE |

`protocol_is_bind_packet` returns non-zero only when the length field equals
exactly 8, then reads `session_id` at byte 3 and `sync_word` at byte 7 (byte-wise
LE inlines — aarch64 alias/alignment safe). The `sync_word` is derived from the
binding phrase ([settings.c](../../common/settings.c),
`phrase_to_sync_word`) and is what keys the FLRC sync word in
[01-link-flrc.md](01-link-flrc.md).
