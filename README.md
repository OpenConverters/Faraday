# Faraday

**Automated EMC design review for PCB layouts — in your browser, your board never leaves your machine.**

Faraday loads a PCB layout (KiCad now; HyperLynx `.HYP` and IPC-2581 planned), screens the
whole board with computational geometry + closed-form transmission-line physics, and returns a
**ranked list of crosstalk and EMC risk findings** — coupled parallel runs (edge and broadside),
reference-plane split/void crossings, 3W-rule violations — each with the mechanism, the estimated
coupling in dB, a stated confidence tier, and a remediation hint.

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
./build/test_faraday        # Catch2
./build/faraday_cli board.kicad_pcb --stackup default-4layer -o report.json
```

## Stackup policy

Z₀ and coupling depend on the stackup. If the board file carries none, Faraday **refuses** rather
than silently assuming one — pass `--stackup default-2layer|default-4layer` (CLI) or confirm the
stackup card (GUI). Every report states the stackup it used.

## Physics references

Hammerstad–Jensen microstrip synthesis; Cohn symmetric stripline; Johnson & Graham
(*High-Speed Digital Design*) coupling estimate k = 1/(1+(s/h)²) with the saturated-NEXT bound.
Formulas are cited at the point of use and pinned by tests against published values.

## License

MIT.
