# Measured reference data — attribution

Numeric data digitised from third-party published figures, used to validate
Faraday against real measurements (ABT #810). **Not covered by Faraday's MIT
license** — each keeps its source's terms, stated below. Included as reference
measurements only; no source figure or artwork is reproduced here.

## tida01492_cispr25_fig47.csv

- **TIDA-01492** reference design, Texas Instruments — LM73605-Q1 front end at
  2.2 MHz, designed to CISPR 25 Class 5
- Source: design guide **TIDUDA4**, Figure 47, "150-kHz to 30-MHz Conducted
  Emissions — Peak and Average Detection" (<https://www.ti.com/lit/pdf/tiduda4>,
  fetched 2026-08-23; capture dated 17 Jul 2017)
- © Texas Instruments Incorporated
- Content: the peak and average detector traces, read out of the receiver
  screenshot by `scripts/digitize_ce_screenshot.py` — numbers only
- Why this board: the strongest public pairing of a machine-readable layout
  (gerbers, schematic, BOM) with a measured CISPR 25 conducted run. See
  `docs/validation-conducted.md` for the two checks that make the digitisation
  trustworthy, and for what remains blocked.
