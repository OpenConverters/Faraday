# Field-solver tier — RLGC from a 2D electrostatic solve

The screening tier ranks risk with closed forms. This tier answers "how much,
really" for a chosen pair, by solving the actual cross-section.

## How it works

One formulation does both storage terms. `OMFEM::ElectrostaticCartesian` solves
`div(eps0 eps_r grad V) = 0` on the cross-section and returns the **Maxwell
capacitance matrix** — N unit-potential solves, with each electrode's charge read
off as the reaction at its constrained DOFs (`Q_j = (A V)_j`), which is exact for
the discretisation. Solving a second time with every dielectric replaced by
vacuum gives `C0`, and for a quasi-TEM line

```
L = mu0 * eps0 * C0^-1          (Paul, Multiconductor Transmission Lines, ch. 5)
```

so the inductance needs no magnetic solve at all. `R(f)` and skin effect still
belong to `HarmonicEddy`; today `R` is reported as zero rather than guessed.

The cross-section **always includes the reference plane** — without the return
path in the picture the inductance is meaningless — and the mesher refuses if any
conductor is thinner than one cell, because an electrode that silently vanishes
would produce a capacitance matrix missing a conductor.

## Running it

`faraday_solve` is off by default so the ordinary build, and the WASM engine the
browser loads, stay free of MFEM:

```sh
cmake -S cpp -B build-solve -DFARADAY_OMFEM_ROOT=$HOME/OpenMagnetics/OMFEM
cmake --build build-solve --target faraday_solve

./build-solve/faraday_solve --w 0.3 --sep 0.5 --h 0.2 --t 0.035 --er 4.4 \
                            --len 40 --deck xtalk.sp --field v.vtk
```

Every parameter is required; none of them can be assumed.

```
cross-section: 3.200 x 1.470 mm, 274 x 126 cells
L [nH/m]      327.2   69.6        C [pF/m]    108.5  -10.8
               69.6  327.2                    -10.8  108.5
line 0: Z0 = 54.9 ohm, v = 0.560 c0, eps_eff = 3.19
backward coupling k_b = 0.0780  ->  NEXT -22.2 dB (field solve)
screening estimate                   NEXT -29.2 dB  (delta 7.1 dB)
```

`--deck` writes an N-section ngspice ladder with all-pairs `K` coupling (rather
than ngspice's convergence-flaky `CPL` element) for a time-domain NEXT/FEXT run.

## Sanity of the extraction

For the 0.3 mm / 0.2 mm / eps_r 4.4 microstrip above: Hammerstad predicts
eps_eff 3.27 and Z0 57.7 ohm against the solve's 3.19 and 54.9 ohm — the solve
sits slightly lower, which is what finite copper thickness does. `Z0 = sqrt(L/C)`
and `v = 1/sqrt(LC)` are internally consistent to machine precision, and a
vacuum-only solve reproduces `1/sqrt(L*C0) = c0` to 1e-9.

## Calibration of the screening tier — a real finding

Sweeping separation at fixed geometry (w = 0.3 mm, h = 0.2 mm, eps_r 4.4):

| sep/h | field solve | screening | delta |
|---|---|---|---|
| 2.0 | −17.4 dB | −26.0 dB | 8.6 dB |
| 2.5 | −22.2 dB | −29.2 dB | 7.0 dB |
| 3.5 | −28.0 dB | −34.5 dB | 6.5 dB |
| 5.0 | −33.7 dB | −40.3 dB | 6.6 dB |
| 7.5 | −40.6 dB | −47.2 dB | 6.6 dB |
| 10.0 | −46.0 dB | −52.1 dB | 6.1 dB |

The closed form is **systematically about 6.5 dB optimistic**, and the offset is
nearly constant — it grows only for the tightest spacing, where the quasi-static
form is weakest.

Two consequences, and they pull in opposite directions:

- **The ranking is trustworthy.** A near-constant offset preserves ordering, which
  is what the screening tier is for.
- **The absolute number is not, and it errs the dangerous way.** Under-predicting
  coupling is worse than over-predicting it.

So the finding text now states the measured bias and the likelier real figure
instead of a symmetric "±6 dB". The formula itself is left alone: it is a cited
literature form, and tuning its constant against one geometry family would be
overfitting dressed as a fix. The honest correction is to say what the error is
and to make the field solve available for anything that matters.
