# MSP DisplayPort OSD

pivid renders the Betaflight OSD on the desktop player (and ships the grid to it)
instead of burning pixels into the video on the receiver. The flight controller
draws its OSD over MSP DisplayPort; pivid-tx reads that grid from the FC, ships it
keyframed over the radio link, and the player composites it client-side. The RX
injects the video-link RSSI / LQ into two reserved cells.


## 1. Betaflight FC setup

On the UART that is wired to the pivid-tx MSP UART (the device in
`PIVID_MSP_UART_DEVICE`, default `/dev/ttyAMA0`):

- In **Ports**, on that UART, at the matching baud (`PIVID_MSP_BAUD`, default
  115200):
  - enable **Configuration/MSP**, **and**
  - set the **Peripherals** dropdown to **VTX (MSP)**.

  > **Required, not optional.** Betaflight only pushes MSP DisplayPort to a UART
  > whose function mask has **both** `FUNCTION_MSP` *and* `FUNCTION_VTX_MSP`
  > (`config.c` binds `displayPortMspSetSerial` only for that pair). With just
  > Configuration/MSP the FC still answers MSP requests (so the port *looks*
  > alive) but pushes **zero** DisplayPort frames — `msp_polls_ok` and
  > `osd_frags_rx` stay at 0. CLI equivalent: add `131072` to the UART's serial
  > function mask, e.g. `serial 1 131073 115200 57600 0 115200`.

- In the CLI:

  ```
  set osd_displayport_device = MSP
  set displayport_msp_use_device_blink = ON
  save
  ```

  `displayport_msp_use_device_blink = ON` is **recommended**: it makes BF send a
  persistent blink *attribute* and lets the display blink locally, instead of BF
  toggling the element's content itself. pivid's keyframe rate
  (`PIVID_OSD_KEYFRAME_MS`, ~5 Hz) is below BF's content-blink rate, so with it
  OFF (the default) blinking elements (e.g. a low-battery voltage) alias into a
  sporadic flicker; with it ON the player blinks them smoothly client-side.

After `save` the FC pushes DisplayPort frames over that UART; pivid-tx applies them
into a shadow grid, snapshots each completed frame on `DRAW_SCREEN`, and transmits
keyframes at `PIVID_OSD_KEYFRAME_MS` cadence (default 200 ms).

## 2. Reserved cells (video-link RSSI / LQ)

The RX **hard-masks** two grid cells and writes the video-link RSSI and LQ into
them itself:

- `PIVID_OSD_VID_RSSI_CELL` (default `1,1` -> row 1, col 1)
- `PIVID_OSD_VID_LQ_CELL`   (default `2,1` -> row 2, col 1, stacked below RSSI)

Anything Betaflight draws in those cells is **silently overwritten**. Move BF OSD
elements out of those cells (or relocate the reserved cells to empty cells via the
two conf keys above; the player renders whatever font glyph sits at indices
`OSD_SYM_RSSI` / `OSD_SYM_LQ`). Both keys are **live**: changing them in the
dashboard SIGHUPs `pivid-rx`, which relocates the injected cells on the next OSD
frame — no RX restart.

## 3. Desktop player font

The player renders the grid by blitting each cell's glyph from an **embedded
msp-osd SD font**, downscaled to the cell size and cached (rasterised once per
`(glyph, cell-size)`; see `osd_msp_font.c` + `osd_grid_render.c` +
`osd_glyph_cache.c`). No FreeType — Cairo loads the embedded PNG from memory.

The bundled font is **SNEAKY_FPV BTFL_DJI** (`assets/font_btfl_dji.png`), the
msp-osd SD format: **36x54 px/glyph, 256 glyphs stacked vertically** (glyph `i` at
`y = i*54`), page 0 (white). Glyph index == Betaflight font index — printable ASCII
at its ASCII code, `OSD_SYM_RSSI`=0x01, `OSD_SYM_LQ`=0x7B, `SYM_AH_BAR9_x`=0x80..0x88
— so the **font is the glyph map** (no index→glyph table, no curation). At 30x16 (SD
canvas) the 36x54 glyphs are *downscaled* to the video, so they stay crisp.

The font is **embedded in the binary** (no runtime font file → the OSD always
renders; no font path/option — single font for now). Injected RSSI/LQ render with the
font's own glyphs at 0x01/0x7B.

**License:** BTFL_DJI is free for personal FPV use only (see
`assets/SNEAKY_FPV_LICENSE.txt`) — do **not** redistribute it in a commercial
product. To change the look or ship publicly, replace `assets/font_btfl_dji.png`
with any msp-osd SD font (e.g. the GPL `material-osd`) and re-run CMake to re-embed.
