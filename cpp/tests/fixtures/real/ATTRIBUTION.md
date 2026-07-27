# Real-board test fixtures — attribution

These are unmodified third-party PCB layouts used as **test data** for the
importer and screening engine. They are NOT covered by Faraday's MIT license;
each keeps its own license, stated below. They are included solely to pin
Faraday's behaviour on real production boards (characterization tests in
`test_real_boards.cpp`).

## hackrf-one.kicad_pcb

- **HackRF One** software-defined radio, Great Scott Gadgets
- Source: <https://github.com/greatscottgadgets/hackrf>
  (`hardware/hackrf-one/hackrf-one.kicad_pcb`, branch `main`, fetched 2026-07-27)
- License: **GPL-2.0** (repo `COPYING`)
- Why this board: 4-layer production RF design, KiCad v6 format
  (`20211014`), full stackup with epsilon_r, 3817 track segments — exercises
  scale, plane classification, and v6 syntax.

## mppt-2420-hc.kicad_pcb

- **MPPT 2420 HC** solar charge controller (20 A / 80 V synchronous buck),
  Libre Solar Project
- Source: <https://github.com/LibreSolar/mppt-2420-hc>
  (`kicad/mppt-2420-hc.kicad_pcb`, branch `main`, fetched 2026-07-27)
- License: **CERN-OHL-W-2.0** (repo `LICENCE`)
- Why this board: power-electronics converter in KiCad **5** legacy format
  (`20171130`) with renamed copper layers (`Top`/`GND`/`3V3`/`Bottom`),
  `(module ...)` footprints, and zone-inherited filled polygons — exercises
  every legacy-format path plus the power-domain rule set.
