# PiVid HAT

KiCad project for the PiVid carrier board — a Raspberry Pi Zero µHAT that mounts
the LR2021 radio module and the status LEDs and bind button the daemons drive.

| | |
|---|---|
| Design | `pivid hat v2`, rev 1.0 |
| KiCad | 10.x (`.kicad_sch` / `.kicad_pcb`) |
| Form factor | Raspberry Pi Zero µHAT, 65 × 30 mm, 4 × M2.5 mounting holes |
| Stack-up | 4 layer — `F.Cu`, `In1.Cu` (Power), `In2.Cu` (GND), `B.Cu` |
| Placements | 21 footprints (2 ICs, 3 connectors, 3 LEDs, 3 resistors, 4 capacitors, 1 switch, 1 jumper, 4 mounting holes) |

The GPIO assignments this board implements are documented in
[../docs/hardware/gpio.txt](../docs/hardware/gpio.txt) — SPI0 plus NRESET on
GPIO17, BUSY on GPIO22 and DIO9 on GPIO27 for the radio, and GPIO5 / GPIO6 /
GPIO13 for the TX LED, bind LED and bind button.

## What is tracked

Design sources only:

```
pivid_hat.kicad_pro      project
pivid_hat.kicad_sch      schematic
pivid_hat.kicad_pcb      board layout
fp-lib-table             footprint library table (project-relative)
sym-lib-table            symbol library table (project-relative)
library/                 LoRa2021 symbol, footprint and STEP model
lib.pretty/              mounting holes + the 2x20 µHAT pin socket
```

Everything KiCad regenerates is ignored — see [.gitignore](.gitignore). That
includes gerbers and drill files: **produce fabrication output from the
`.kicad_pcb`** (File → Fabrication Outputs) rather than trusting a checked-in
copy, which silently goes stale the moment the layout changes.

## Attribution

The board outline, mounting-hole placement and the µHAT pin-socket footprint
derive from the **Raspberry Pi Zero (W) uHAT Template Board** by Ravikiran
Bukkasagara, released under the MIT License:

> Copyright (c) 2019 Ravikiran Bukkasagara <contact@ravikiranb.com>
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

See the [HAT design guidelines](https://github.com/raspberrypi/hats/blob/master/designguide.md)
for the mechanical and electrical requirements a HAT has to meet. Note this
board carries **no ID EEPROM** — the template omits it deliberately, since the
footprint and pull-up sizing are project-specific.
