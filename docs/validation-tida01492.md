# Validating the conducted pipeline against a measured board — TIDA-01492

ABT #810 asks for the thing that decides whether any of the conducted work can
be shown to an EMC engineer: a real converter, measured on a real LISN, run
through S1–S4, with the error reported as a stated band per decade rather than
a headline number.

This document is the first half of that, and it is honest about which half.

## Status

| | |
|---|---|
| Measured reference data | **in hand** — digitised, two independent checks passed |
| Faraday's CISPR 25 Class 5 limit table | **validated** against an accredited lab's own receiver |
| Board through S1–S4 | **blocked** — TI gates the gerbers behind a myTI login |
| Per-mode (CM/DM) comparison | **not possible from public data** — see below |

## Why this board

Searching for a public pairing of *a machine-readable board* with *a measured
conducted spectrum* turns up very little, and the reason is structural rather
than accidental:

* **No open dataset exists.** Nothing on Zenodo or IEEE DataPort pairs raw
  conducted-emission traces with a published layout. Every vendor result is a
  rendered figure — no CSV anywhere.
* **Every published plot is envelope-only.** A compliance capture is one trace
  per detector on one line. CM and DM are not separable from it after the fact;
  that needs two phase-coherent line captures or a hardware separator, at the
  bench. So #810's per-mode requirement cannot be met from public data by
  anyone, at any effort.
* **Design files are usually gated.** TI's gerbers need a myTI account; ADI's
  demo-board pages did not respond at all from here.

Against that, [TIDA-01492](https://www.ti.com/tool/TIDA-01492) is the strongest
public candidate found. It publishes schematic, layout, **gerbers**, BOM and
assembly drawing, and its design guide (TIDUDA4) carries a CISPR 25 conducted
run with peak and average detectors, 150 kHz–30 MHz and 30–108 MHz, Class 5
limits overlaid and the test setup photographed. LM73605-Q1 front end at
2.2 MHz, 3.5–36 V in.

## The measured data, and why it can be trusted

Figure 47 is a Rohde & Schwarz screenshot. `scripts/digitize_ce_screenshot.py`
reads the trace pixels back into numbers, which is normally where a validation
quietly stops being one — so the script refuses to return numbers it cannot
corroborate, and both corroborations come from the screenshot itself.

**The marker.** The instrument printed its own readout in the header: 13.42 dBµV
at 30.000000000 MHz on trace 1. Digitised, that point reads **13.27 dBµV** —
0.15 dB. That checks the whole axis mapping, end to end, against the instrument
rather than against my arithmetic. A failed marker check is an error exit, not
a warning.

**The limit lines.** A compliance capture has the standard's limits drawn on it
by the receiver, from the lab's own table. Recovering the red pixels gives an
independent implementation of CISPR 25 Class 5, produced on real equipment by
people who do this for a living:

| band | Faraday `cispr25_conducted(5, …)` | recovered from the capture |
|---|---|---|
| LW 0.15–0.30 MHz | peak 70 · avg 50 | (peak sits on the graticule) · **50.05** |
| MW 0.53–1.8 MHz | peak 54 · avg 34 | **54.04** · **34.09** |
| SW 5.9–6.2 MHz | peak 53 · avg 33 | **53.1** · **33.1** |
| CB 26–28 MHz | peak 44 · avg 24 | **44.1** · **24.1** |

Every band, both detectors, inside 0.1 dB. That confirms three things at once
that were previously only checked against my own reading of the standard: the
Class 5 baselines, the **non-uniform class step** (10 dB in LW, 8 in MW, 6 from
SW up), and the **detector offsets** (peak = QP + 13, average = QP − 7). It is
pinned as a test — `[emc][conducted][cispr25][measured]` in `test_emissions.cpp`
— so the table cannot drift away from the lab's again.

The band *edges* agree too: the lab's MW line runs 0.5263–1.7830 MHz against a
nominal 0.53–1.8, which is the instrument drawing to the pixel it had.

Digitised traces: `docs/data/tida01492_cispr25_fig47.csv`, peak and average,
~1000 points each, roughly ±0.5 dB from the pixel grid. Good enough for a
per-decade error band; not for pinning a single harmonic.

## What is still blocked, and what it would take

S1 needs the board. TI serves `tidce75.zip` only behind a myTI login, so the
gerber pack cannot be fetched from here. **One download with a TI account
unblocks the rest of this document.**

After that the remaining cost is real but known: gerbers carry copper, not a
netlist, so component values have to come off the BOM by hand. That is exactly
what `--parts-out` / `--values` was built for on the PoE board, so the path
exists.

Then: S1 deck export → S2 device models → S3 the EMI run → S4 the spectrum,
compared against the CSV above **per decade**, peak against peak and average
against average.

## What "good" would be

Worth fixing before the comparison rather than after, so the answer is not
graded against whatever it happens to produce.

* **Rohde & Schwarz, simulation vs measurement on the ADI DC3042A** (LTC3310,
  5 V → 1.2 V at 6 A, 2 MHz) — the closest published analogue of this exercise,
  and the only public one that separates the modes. Dual-port LISN into a
  scope, CM = Ch1+Ch2, DM = (Ch2−Ch1)/2. Reported deviation at the 2 MHz
  fundamental: **about 10 dB for LTpowerCAD II**, with and without filter,
  smaller for LTspice, attributed to unmodelled parasitics.
* **Published behavioural/terminal models**: under 5–6 dB to 30 MHz — but those
  are fed a *measured* noise source. Faraday derives its source from layout
  plus a device model, so it should expect worse and say so.

A vendor grading its own tool at 10 dB is the useful number here. A stated band
of ±10–15 dB per decade is not a weak result; it is in line with the state of
the art, and #810's own position that a negative result is publishable is well
supported by the literature.

## The per-mode half (#810's actual requirement)

Needs the bench, and the recipe is settled: a dual-port LISN into a scope
following the R&S method, or any board plus a Mark Nave separator (sold as the
TekBox LISN Mate, 30 kHz–110 MHz). Either gives what `hertz.separate_cm_dm`
already wants and cannot get from a magnitude-only scan — the scope route
carries phase because it captures in the time domain, the separator does it in
hardware.

The DC3042A is the board to do it on: R&S have already published the measured
CM/DM result for it, and it carries an input EMI filter switched in by feeding
`VIN EMI` instead of `VIN`, so filtered and unfiltered are the same hardware
with one lead moved — the pair that makes a filter-prediction claim mean
something rather than a single point.

## A free sanity check that needs no lab

WE's own ANP010 measures an unfiltered 2 MHz buck at 10 V in and 0.7 A input
rms on a CISPR 25 DC-LISN, peak detector, and publishes two hard numbers at the
fundamental: **128 dBµV at full load, 112 dBµV at 50 mA**. It is differential
mode only, and the note says outright that DM measurement is not defined in the
EMC standards — so it is not a validation. But if S1–S4 on a comparable buck
does not land near 128 dBµV at the fundamental, something is wrong, and finding
that out costs an afternoon rather than a lab booking.

## Until then

The panel's wording stands as it is: the conducted numbers are a modelled
prediction, not a compliance statement, and they must keep saying so.
