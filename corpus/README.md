# Corpus validation ledger (Tier 4)

The living record of ABT #422 (closed 2026-08-01; this file is its successor —
update it as boards land). The program: grow the open-source converter corpus,
track (a) the derive success rate of the critical-mesh analysis, (b)
human-reviewed member-set agreement, (c) fallback correctness — and pin every
reviewed board as a Catch2 regression (`test_real_boards.cpp`,
`[corpus-pin]`, SKIP when the corpus is not fetched). Characterization-test
discipline throughout: pins are verified against physical sense and never
silently re-pinned.

## Final stats (2026-08-01)

**Every converter board in the program derives.** 14 derived meshes across 8
physical boards, zero known wrong-member cases:

| Board | Node(s) | Derived mesh | Notes |
|---|---|---|---|
| PoE flyback (Altium ODB++) | NetD17_3 | T7 + D22 + R29 | RCD clamp — includes the D22 a hand-run prototype missed |
| | NetC52_2 | T4 + D21 + R16 | |
| LM5143-Q1EVM (TI gerbers) | ×2 | exact per SNVA-documented hot loops | two-phase buck |
| LM5177EVM (via KiCad 9) | buck + boost | both match SLVAFJ3 | |
| LM5123EVM (via KiCad 9) | boost | closes through the OUTPUT cap | Franz's own example of what pattern-matching gets wrong |
| mppt-2420-lc | SW_NODE | Q1 + Q4 + C4 (71 mm², 25 nH) | pinned |
| mppt-1210-hus | SW_NODE | Q1 + Q4 + C4 (64 mm², 26 nH) | pinned; LOAD_S pinned to keep REFUSING a divider-as-clamp |
| mppt-2420-hc | SW_NODE | Q1 + Q2 + C3 (buck) | TO-220 equal pads → conduction case d (pin 1 = JEDEC control terminal, 2 mm² pad floor) |
| VESC (KiCad + ODB++) | H1/H2/H3 | Q1+Q2+R53+C40 · Q3+Q4+C8 · Q5+Q6+R54+C37 | Kelvin shunts NAMED as mesh members; both formats agree byte-for-byte |

Plus 4 synthetic topology pins in Catch2: buck, boost-with-decoy,
flyback-RCD, Kelvin-shunt bridge — and the determinism pin (pad order +
exporter rounding invariance).

**Correct negatives:** pwm-2420-lus (PWM controller, SOLAR- only), moteus /
FPGA / interface boards, hackrf (pinned: 0 switch nodes, exactly 2
candidates — its dual-LDO + ferrite harness is externally isomorphic to a
fixed buck; see the main README).

**Wrong-member cases found and fixed during the program** (each caught by the
regression pins this ledger mandated — which is the whole argument for them):

- mppt-1210-hus LOAD_S derived a divider (R+R) as a clamp path → two-edge
  clamps must contain a diode; falls back honestly.
- PoE gate-drive SOT-23s with numbered pins hijacked the mesh → 2 mm² pad
  floor on conduction case d.
- mppt-2420-hc SUPPLY_INPUT (L2+Q4 supply-ORing rail) and PoE NetC45_1
  (aux-winding node smoothed by C45) screened as commutation loops → the
  shunt-cap veto: a net with a 2-pad capacitor straight to the return can
  never switch.
- KiCad vs ODB++ of the same VESC picked different caps (C8/277 mm² vs
  C40/221 mm²) and anchor FETs → meshes now scored by pad-hull area (the
  least-inductance loop; C40 was physically right), areas quantized to
  0.1 mm² before ref-name tie-breaks. Centroid-hull scoring was tried and
  REFUTED (mppt-2420-lc's collinear C2 degenerated to ~1 mm²).

## Format coverage — real products on all five formats

- **KiCad**: the corpus (this directory; `scripts/fetch_corpus.py`).
- **Gerber + IPC-D-356**: TI fab exports (TIDA-010979, LM5143-Q1EVM) —
  Altium exporter. Drawing-sheet title blocks are cropped and counted
  (LM5143 screened as 168×105 mm before; really 96×73 with 8.3 m of
  title-block strokes); IPC netting below 95 % of routed copper is reported,
  never silent.
- **ODB++**: the PoE flyback (real Altium product) + VESC through KiCad 9's
  own ODB++ writer — second real exporter, derives identically.
- **HyperLynx**: hyp2mat's Eagle-exported RF filters (hairpinfilter,
  notchfilter; github.com/koendv/hyp2mat) — own stackup honored.
- **IPC-2581**: BeagleBone Black RevB6 (Cadence Allegro, ipc2581.com
  consortium) — screens with an explicit stackup; the file carries no
  permittivity and Faraday refuses to invent one.

Altium native `.PcbDoc` stays refused by design (binary OLE). Altium boards
are covered by three validated routes: ODB++ export, `Gerber_NCdrills_IPC`
fab output, and the `.PcbDoc` → KiCad 9 importer.

## History (ledger entries)

- **2026-07-31 baseline** (9 boards + PoE): derived on mppt-2420-lc and PoE
  flyback; geometric fallback on vesc + mppt-2420-hc (D2PAK role ambiguity).
- **+ conduction cases b/c + SOT-23 refusal pins**: 11 reviewed-correct
  meshes across 7 boards; VESC 1/3 phases.
- **conduction case d + Kelvin-shunt bridge**: every converter board
  derives — 14 meshes across 8 boards; VESC 3/3 with shunts as members.
- **2026-08-01, the "100 %-happy" audit**: pour coverage became a scanline
  UNION (BBB LYR2 175 % → 92.7 %); importer plausibility notes are appended
  by the gate, never overwritten; gerber outlines are the smallest profile
  loop holding ≥95 % of pads+vias; T-junction/pour/mid-via physical-contact
  netting; the shunt-cap veto and monolithic-converter candidates
  (ABT #408/#409/#410); mesh determinism across formats.
