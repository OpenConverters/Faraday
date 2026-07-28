# The component near-field map: what it must be

Design spec, synthesised from six verified research dives (116 claims refuted and
corrected by adversarial review). Nothing here is implemented yet.

## 1. Why this is not the map we already have

Faraday's existing radiation layer is a **far-field** attribution: it answers "which
copper contributes to the 3 m chamber measurement". The question "why is *that*
component shielded, and what does it do to its neighbours" lives in a completely
different regime, and the two must never be merged.

The boundary is `r = λ/2π`:

| f | λ/2π |
|---|---|
| 1 MHz | 47.7 m |
| 30 MHz | 1.59 m |
| 100 MHz | 477 mm |
| 1 GHz | 47.7 mm |

At component scale (0.1–50 mm) below about 1 GHz, `k·r ≪ 1`. **The fields are literally
the magnetostatic and electrostatic dipole fields** — the radiation terms are negligible.
At 5 mm and 30 MHz, `k·r = 0.0031`, and the 1/x³ quasi-static term exceeds the 1/x
radiation term by ~100 dB.

Three consequences that invert far-field intuition:

- **Decay is 1/r³, not 1/r** — 18.06 dB per doubling of distance, not 6.02. Doubling a
  keep-out buys 18 dB. This is the single most useful fact for a designer.
- **E and H are independent.** Wave impedance is set by the *source*, not the medium.
  At 5 mm / 30 MHz: `Z_w,E = 120 kΩ`, `Z_w,M = 1.18 Ω` — five orders of magnitude apart.
  The far-field identity `E[dBµV/m] = H[dBµA/m] + 51.5 dB` is wrong by **±40 to ±55 dB**
  in the 1–10 mm window. Converting one field to the other is a hard error.
- **Source type decides everything**, including which shield works.

## 2. Why some components get shielded — the actual answer

Two different mechanisms, routinely conflated:

**Voltage-driven (high-Z, E-dominant):** switch/phase-node copper, crystal OUT pin, gate
drives. Field `E ∝ 1/r³`, wave impedance ≫ 377 Ω. A thin conductive shield works
*extremely* well here — reflection loss dominates and the metal is never the limit; the
bond to reference is. Measured: a grounded integrated copper E-shield on a composite
inductor gives **up to 20 dB at 1 cm — but only if soldered to ground.**

**Current-driven (low-Z, H-dominant):** commutation loops, power paths, inductor
windings. Field `H ∝ 1/r³`, wave impedance ≪ 377 Ω. **A conductive can barely helps.**
Below ~10 MHz a small thin-wall can gives single-digit to low-tens dB against a magnetic
near field *regardless of material* — reflection loss is poor at low impedance and the
wall is a fraction of a skin depth. What works is permeability, distance, or reducing
the loop.

So the honest answer to "why is this component shielded": a shield can over an RF
section is doing E-field and plane-wave work. A *magnetically* shielded inductor is not
shielded by a can at all — it has a closed magnetic path so the flux never leaves.
Measured shielded-vs-unshielded (Würth ANP047c, corrected by review): **~25–31 dB at the
switching fundamental and low harmonics, falling to ~14–18 dB at 8–10 MHz.** Any flat
scalar "shielded = −15 dB" is wrong in both directions.

And the crucial scoping result: an inductor's stray field is overwhelmingly a **local
coupling** problem, not a compliance one. Reaching a 40 dBµV/m limit at 3 m from an
inductor dipole needs ~20 mA at 150 MHz through the effective area — whereas **7.96 µA
of common-mode current on a 1 m cable** does it at 30 MHz. Cables beat board loops by
~84 dB at equal current. A can over an inductor does nothing for that.

## 3. What the map must compute

**Two co-registered layers, never blended, with independent colour scales.**

H-layer, from current-driven sources:
```
m = N·I·A                                  magnetic moment [A·m²]
H_axial      = m / (2π r³)                 quasi-static, r ≫ source size
H_equatorial = m / (4π r³)
|H| = m·√(1 + 3cos²θ) / (4π r³)
```

E-layer, from voltage-driven sources: dual expressions from the electric dipole, driven
by `i_disp = C_stray · dV/dt`.

Units: **A/m or dBµA/m** for H, **V/m** for E, at a **stated emulated height**. Never
dBµV/m, never a limit line, never a margin.

### The validity gate — this is not optional

The point-dipole model is **invalid** within ~2–3 source dimensions. Worked example from
the review: at 5 mm from a 100 mm² loop the dipole model is not marginal, it is wrong —
the exact equivalent-circular-loop value is **37.2 A/m (151.4 dBµA/m)**, and the first
valid dipole point is **~28 mm** (5·a_eff). Between roughly 5 and 20 mm from a typical
power inductor the real decay is closer to **1/r² (~12 dB/doubling)**, not 1/r³.

Per the house no-fallbacks rule: compute `r/a_eff` and **refuse to print a number** inside
the invalid radius rather than extrapolating. The shortest contour a dipole model may
print for a typical inductor is ~10 mm, with a wide uncertainty band.

## 4. Source inventory, ranked

Near-field strength at 1–5 mm on a switching board:

1. **Commutation ("hot") loop — H.** Sets the whole 50–300 MHz broadband floor. Ringing
   at `f = 1/(2π√(L_loop·C_oss))`, typically ~120 MHz. Faraday already extracts its area.
2. **Switch/phase-node copper — E.** 2.4 V/ns measured for silicon (12 V in 5 ns); GaN
   20–150 V/ns. The metric is coupling area **to non-return conductors**, not total
   polygon area over its own return plane.
3. **Power inductor — H at fsw, E at the ring frequency.**
4. **Input decoupling capacitor** — a *member* of the commutation loop, not a separate
   object. Moving input caps 5 mm further raised L_loop 4.6 → 6.6 nH.
5. **IC power pins and the IC-to-decap loop.**
6. **Stubs and test points** — an automated scan found debug testpoints among the
   strongest H-field spots on a whole board. Faraday already has a `via-stub` rule.

Reference-plane geometry belongs **inside** the source model: a void under the hot loop
is a ~20 dB amplifier at 100–300 MHz. A 36 mm² loop over solid plane beat a 4.5 mm² loop
by 10 dB — loop area alone does not predict emission.

## 5. The victim side

```
V_N[µV] = 6.283 · f[MHz] · B[µT] · A[mm²] · cos θ
```

Layout gives **A exactly** and **cos θ exactly** — those are the two terms a
geometry-only tool owns. B needs an assumed current.

Worked: at B = 14.1 µT (10 mm from an unshielded drum at 500 kHz), a 4 mm² victim loop
picks up 177 µV; 32 mm² picks up 1.4 mV. At the 130 MHz hot-loop ring the same 4 mm²
loop at 10 mm sees **46 mV**.

Use the **ringing frequency, not fsw** — `V ∝ f·B`, so 130 MHz beats 500 kHz by two
orders of magnitude.

Threshold ladder: precision CSA offset ±15 µV; 12-bit/3.3 V LSB 806 µV; 10-bit 3.22 mV.
Peak-volts thresholds (logic margin, comparator hysteresis, FB/COMP ramp) and DC-accuracy
thresholds (Vos, LSB) must be **separate ladders** — they cannot be ranked against each
other.

Capacitive: broadside overlap is **0.19 pF/mm² at 0.2 mm** dielectric. "Victim trace
overlaps switch-node copper on any layer" deserves its own high-severity finding with
`ΔV = ΔV_SW · C₁₂/(C₁₂+C_victim)`.

**cos θ is the cheapest countermeasure and is invisible to every distance-based rule.**
Rotating a victim loop edge-on nulls the coupling; perpendicular inductor axes null
inductor-to-inductor coupling entirely. A distance-only rule flags false positives on
perpendicular pairs.

## 6. What the map must NOT claim

- **No compliance verdict, no dBµV/m, no limit line, no pass/fail.** There is no
  reliable near-field → far-field transform. Two independent standards-level sources say
  so. Correlating a near-field scan with a TEM cell required a dimensionally improper
  E+H summation and the authors still warned the two "will never" agree.
- **No red/amber/green ramp that reads as a limit.** Red means "strongest thing on *this*
  board at *this* frequency".
- **No manufacturer keep-out radius.** No primary vendor publishes one; Coilcraft
  explicitly declines. A drawn ring must be labelled as the tool's own.
- **No structure finer than the physics supports.** At emulated height z, detail below
  ~2.7·z is evanescent-suppressed.
- Must **enumerate what it does not model in the UI**: cable common-mode (worth up to
  100 dB and the usual cause of real failures), enclosure/seam radiation, harness.

The defensible outputs are **rank order, relative dB between revisions, and
"strongest on this board"** — plus the Hubing/Clemson precedent of computing a
*maximum possible* contribution and saying so.

## 7. Validation invariants

The tool's discipline is that every number checks against something independently
derivable. This regime supplies unusually strong invariants:

| check | expected | why it is sharp |
|---|---|---|
| `Z_w,E(r) · Z_w,M(r) = η₀²` | exact, **all r**, machine precision | duality; catches any error in either dipole |
| `Z_w` at `k·r = 1` | `η₀/√2 = 266.39 Ω` and `η₀√2` exactly | closed-form algebraic identity |
| both cross 377 Ω | at `k·r = 1/√2` exactly | consequence of the duality |
| `k·r → 0` limit | `m√(1+3cos²θ)/(4πr³)` | quasi-static limit of the exact dipole |
| decay slopes | 18.06 dB/doubling (1/r³), 12.04 (1/r²), 6.02 (1/r) | universal |
| η₀ | derive as `μ₀·c`, never hardcode | makes the duality assert exact by construction |
| reciprocity | same `m` gives emission and susceptibility ranking | but see caveat below |

Reciprocity caveat from the review: it guarantees identical TX/RX *patterns* and input
impedance — **not** that a weak radiator is a weak victim. Efficiency and load matching
break the symmetry, in opposite directions.

Use Balanis 4-8a–c / 5-27a–c for the exact dipole fields. LearnEMC's published forms have
three sign defects from a missing minus in their Faraday step.

## 8. Implementation order

1. **H-layer from the commutation loop and switch node**, with the `r/a_eff` validity
   gate and the refusal behaviour. Reuses loop areas Faraday already extracts.
2. **Victim overlay**: loop area and cos θ per sensitive net, against the threshold
   ladder. This is where layout-only data is strongest — A and cos θ are exact.
3. **E-layer** from switch-node coupling area to non-return conductors.
4. **Inductor as a source**, requiring user-supplied current and an enumerated
   construction type (unshielded drum / semi-shielded / shielded ferrite / moulded
   composite) with a **frequency-dependent** benefit curve, never a scalar.
5. **Shielding**, as a conditional and source-type dependent modifier — and honest that
   a can is near-useless against a low-frequency magnetic near field.

Constants to fix in code: `η₀ = μ₀·c = 376.730313461771 Ω`, `η₀² = 141925.729081 Ω²`.
Free-space / OATS constant pairs are `1.317e-14 / 6.283e-7` and `2.63e-14 / 1.26e-6` —
**never mix them**; any absolute figure inherits a ±6 dB ambiguity from that choice alone.
