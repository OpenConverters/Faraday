# Faraday

**Automated EMC design review for PCB layouts — in your browser, your board never leaves your machine.**

Faraday loads a PCB layout, screens the whole board with computational geometry + closed-form
transmission-line physics, and returns a **ranked list of crosstalk and EMC risk findings** —
each with the mechanism, the number, a stated confidence tier, and a remediation hint.

## Formats

| Format | Reaches | Notes |
|---|---|---|
| **KiCad** `.kicad_pcb` | KiCad 5–9 | legacy layer names, `(module)`, zone-inherited fills |
| **HyperLynx** `.hyp` | Altium, PADS, Expedition, Eagle | carries its own stackup with permittivity |
| **IPC-2581** `.xml` | rev B/C exporters | no other open-source reader exists |

Format is detected from the file's **contents**, not its name.

## Rules

`coupled-run` (edge, broadside, and to copper-pour boundaries) · `diff-pair` (intentional
coupling, never a defect) · `3w` · `plane-crossing` and `sparse-reference` (return-path breaks,
rolled up when systemic) · `via-stub` and `dangling-stub` (λ/4 resonators) ·
`decoupling-distance` · and for power converters, `switch-node` and **`commutation-loop`** —
the input-cap → switch-pair → return loop whose enclosed area dominates converter emissions.

Faraday is an automated **design review**, not compliance prediction. Screening-tier numbers are
first-order estimates for *ranking* risk; a field-solver tier (OMFEM 2D cross-section RLGC +
in-process ngspice via Kirchhoff) refines selected pairs.

## Layout

- `cpp/include/faraday/` — header-only C++20 core (s-expr parser, board IR, KiCad importer,
  transmission-line closed forms, screening engine)
- `cpp/tests/` — Catch2 tests (run the binary directly, never ctest)
- `cpp/tools/` — `faraday_cli`: `.kicad_pcb` → `report.json` + console summary
- `web/` — Vite + Vue GUI: drop a board, rendered layout with severity overlays, hover tooltips,
  two-way-linked findings list. The engine runs in WASM — nothing is uploaded.
- `scripts/build_wasm.sh` — emsdk build of the WASM engine into `web/public/`

## Build (native)

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_faraday        # Catch2 (run the binary directly, never ctest)
./build/faraday_cli board.kicad_pcb -o report.json
./build/faraday_cli board.hyp
./build/faraday_cli board.xml --stackup default-4layer
```

## The bench — field solve and transient, in the browser

Any coupled-run finding offers **Solve this cross-section**. That opens a 2D
boundary-element extraction of the actual geometry and a transient of the coupled pair,
both running in the page:

- the **field** of the real cross-section, computed in closed form from the panel charges
  (no mesh, and the reference plane is exact by images rather than truncated);
- **RLGC** — Z₀, Z even/odd, ε_eff, delay, mutual L and C, backward coupling;
- the **victim's noise waveform**, NEXT and FEXT, against its receiver's threshold;
- a **verdict in millivolts** against the DC input margin of a named logic family, and
  the separation that would bring it back under budget.

Extraction plus transient is a few milliseconds, so the sliders — separation, edge rate,
coupled length, swing — re-solve the physics as they move. See
[docs/browser-field-tier.md](docs/browser-field-tier.md) for the formulation, the
validation table, and the modelling limits.

## Radiated emissions — does it pass?

On a converter, `commutation-loop` findings offer **Predict radiated emissions →**. The
enclosed loop area comes off the copper — the one input every other estimator makes you
measure by hand — and the panel puts the resulting spectrum against the **CISPR 32 /
EN 55032** or **FCC Part 15** limit line, with a margin in dB.

Sliders for switched current, switching frequency, edge rate and duty; the plateau above
the edge knee is set by loop area, current and edge rate alone, so halving any one of
them buys exactly 6 dB.

The same panel carries the **common-mode budget**: how much common-mode current an
attached cable of a given length may carry and still pass. Faraday cannot predict your
actual common-mode current — it comes from ground-plane impedance and return-path
detours, not from geometry — but the inverse needs no unknowns, and it lands in
*microamps*, which is why an ordinary current probe never sees the mechanism that fails
most products.

The loop figure is an **estimate**, and the panel says so where it cannot be missed: differential-mode
loop radiation only, no common-mode current on attached cables (which dominates most real
failures), no enclosure, no board resonances. A clean result means this loop is not your
problem — not that the product passes.

## The radiation layer

The **radiation** chip on the board view re-colours every trace by how much of the
board's differential-mode emission it accounts for. Each segment plus its return forms a
loop of `length × height-to-reference`; over an intact plane that is square micrometres,
and where the reference is missing it becomes the trace length times the whole board. So
the map answers *which twenty traces are eighty percent of this*, which is the question
that changes a layout.

It is an **attribution, not a field simulation** — a ranking of your own copper, summed
incoherently, with an assumed current that scales the whole thing linearly. It is
deliberately not coloured in absolute field units, because coloured that way it would
look like a full-wave solve and it is not one.

## The component near-field map

The **near field** chip opens a different regime from the radiation layer. At component
scale below ~1 GHz, `k·r ≪ 1` and the fields *are* the magnetostatic dipole fields:
decay is **1/r³ — 18 dB per doubling**, not 6, and E and H are independent (wave
impedance spans five orders of magnitude at 5 mm, so the far-field `+51.5 dB` conversion
is wrong by ±40–55 dB there).

It shows |H| in A/m at a stated probe height, and for each sensitive component the
voltage induced in its own loop against the threshold for its class — a precision
current-sense amp is judged at 15 µV, a 12-bit ADC at 806 µV.

**This is why some components get shielded**, and the answer has two halves that are
routinely conflated. A thin conductive can is excellent against a *voltage-driven*
E-field source (reflection dominates; the bond to ground is the limit, not the metal)
and **nearly useless against a low-frequency magnetic near field** — single-digit dB
below ~10 MHz regardless of material. A magnetically shielded inductor is not shielded
by a can at all; it has a closed magnetic path.

It carries no dBµV/m, no limit line and no pass/fail: there is no reliable near-field to
far-field transform. See [docs/near-field-map-design.md](docs/near-field-map-design.md).

## Stackup policy

Z₀ and coupling depend on the stackup. If the board file carries none, Faraday **refuses** rather
than silently assuming one — pass `--stackup default-2layer|default-4layer` (CLI) or confirm the
stackup card (GUI). Every report states the stackup it used.

## Physics references

Hammerstad–Jensen microstrip synthesis; Cohn symmetric stripline; Johnson & Graham
(*High-Speed Digital Design*) coupling estimate k = 1/(1+(s/h)²) with the saturated-NEXT bound.
The field tier follows Nabors & White's FastCap formulation (IEEE TCAD 1991) in 2D, and
Paul's *Multiconductor Transmission Lines* for L = μ₀ε₀C₀⁻¹ and the backward-coupling
coefficient. Formulas are cited at the point of use and pinned by tests against published
values — including the exact identity L·C = μ₀ε₀ε_r·I, which holds to 2.6e-16.

## License

MIT.
