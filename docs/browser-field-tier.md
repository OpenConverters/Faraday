# The bench: a field solver and a circuit solver, in the browser

The screening tier ranks the whole board with closed-form physics. The bench answers
**one** question properly: for this pair of traces, how much noise does the victim
actually see, and does its receiver care?

It runs entirely in the page. No server, no upload, no install — the same privacy
property the screener has, extended to the deep tier.

| | screening tier | bench |
|---|---|---|
| scope | whole board, every pair | one cross-section |
| physics | Hammerstad / Cohn / Johnson closed forms | 2D boundary-element extraction + transient |
| answer | ranked dB estimate, ±6 dB | millivolts on the victim vs its receiver's margin |
| cost | ~0.5 s for a 4 MB board | ~2 ms extraction, ~1 ms transient, ~60 ms field map |

## Why boundary elements rather than finite elements

A FEM solve needs a box, and for microstrip the box is a lie: the fields leave through
the top. Every FEM answer carries a truncation error you can only bound by re-running on
a bigger box. That is what the earlier OMFEM tier did, and it is why it stayed a native
CLI — MFEM is far too heavy for a browser, and its mesh is the dominant cost.

A boundary-element method discretises only the **conductor surfaces** and the
**dielectric interfaces**. The free-space Green's function already satisfies the
radiation condition, so open boundaries are exact rather than approximated. The matrix is
small and dense (~150 unknowns) instead of large and sparse (~50 000), which is the
entire reason a slider can re-solve the physics while it is moving.

Two further consequences:

- **The reference plane costs nothing and is exact.** A perfect conductor at *y* = 0 is
  imposed by images, so the plane is infinite, unmeshed, and free of truncation error.
  This is legitimate with mixed dielectrics because we use the Dirichlet Green's function
  of the half-space and represent all material response as bound charge.
- **The field plot IS the solution.** With the panel charges known, the potential and
  field are available in closed form anywhere — no mesh, no interpolation, no
  post-processing projection.

## Why not ship ngspice

ngspice compiles to WebAssembly; [EEcircuit](https://eecircuit.com/) and
[ngspiceX](https://shishir-dey.github.io/ngspiceX/) both do exactly that, and Faraday
already drives the real thing natively through Kirchhoff's in-process libngspice.

But a general simulator re-parses a netlist, rebuilds its matrix and re-factors it on
every run, and a coupled-line ladder is a **linear** circuit at a **fixed** timestep: its
MNA matrix never changes. Factor once, and each timestep is one triangular solve instead
of a Newton iteration. That is the difference between "press run and wait" and "drag the
separation slider and watch the noise move."

Correctness is not traded for that speed. `spice_ladder_deck()` emits the identical
circuit for ngspice, and the solver is pinned against transmission-line theory directly
(below).

FastCap and FastHenry are not used. FastCap solves the same integral equation this does,
in 3D and with multipole acceleration neither of which helps a 2D cross-section;
FastHenry's job (frequency-dependent R and L) is not on the interactive path.

## What is checked, and against what

Nothing here is a regression baseline captured from a previous run. Every reference is
independently derivable.

| check | reference | result |
|---|---|---|
| `L·C = μ₀ε₀ε_r·I`, off-diagonals included | exact identity of quasi-TEM theory | **2.6e-16** |
| coupled stripline Z_even, Z_odd | Cohn's conformal map, exact | **0.03–0.05%** |
| square conductor over a plane | logarithmic capacity, r_eq = 0.5902·a | **0.1%** |
| microstrip Z₀ | Hammerstad + IPC-2141 thickness correction | **0.7–2.7%**, residual falls with t/w |
| forward crosstalk in a homogeneous medium | identically zero | **1.7% → 0.10%** of NEXT as sections go 8 → 64 |
| near-end crosstalk, saturated | Paul's k_b × launched wave | **0.1–1.7%** |
| DC settling | resistor divider | **0.05%** |
| interface truncation | self-convergence | **< 0.02%** beyond 10× height |

The `L·C` identity is the sharpest instrument in the box. L and C come from two separate
solves — one with the dielectric, one in vacuum — so nothing but a correct formulation
makes their product collapse to a scaled identity.

### The bug it caught

The panel integrals evaluate a subtended angle. Written the obvious way as
`atan2(w₂,v) − atan2(w₁,v)`, that expression is correct only while the field point stays
off the branch cut: for `v < 0` with the projection falling inside the panel, `w₁` and
`w₂` straddle zero, atan2 jumps from −π to +π, and the angle comes out 2π wrong.

Coplanar panels always have `v = 0` and `w₁w₂ > 0`, so a mesh of flat strips never trips
it — the zero-thickness stripline path was correct throughout. The moment a closed
conductor contributed perpendicular faces, the potential was wrong, the charge density
went **negative** in places, and the extracted capacitance converged cleanly to a value
13× too large. Refinement made it worse, which is the signature to watch for.

Every scalar figure still looked plausible. The identity did not: it read 1.6% instead of
1e-16. The fix is to evaluate the angle as one continuous quantity,
`atan2(v·a, v² + w₁w₂)`.

## Modelling limits, stated

- **Two conductors and one reference.** Three-way coupling is not solved; the screener
  ranks pairs, and the bench answers about a pair.
- **Zero-thickness strips in homogeneous media, finite thickness elsewhere.** A
  zero-thickness sheet straddling a dielectric boundary has a free charge that cannot be
  recovered from the total charge, so the solver refuses rather than guessing. Conversely
  a very thin closed contour cannot be panelled at any sane count, and that is refused
  too — with the reason.
- **Quasi-TEM.** No radiation, no surface waves, no dispersion beyond the modal split.
- **Lossless by default.** `R` is carried but zero unless supplied; skin effect is not on
  this path.
- **The transient is a lumped ladder**, not a distributed line. The section count is set
  from the edge rate, and the residual FEXT in a homogeneous medium is the honest readout
  of what that costs.

## The verdict

A dB figure is not an answer. The bench reports the peak the victim actually sees in
millivolts against the DC input margin of a named receiver family — the smaller of
V_IL(max) and V_DD − V_IH(min) — and, when that is too much, bisects the separation to
find the gap that would fix it. That last number is re-solved independently and the test
suite checks the promise survives being taken.
