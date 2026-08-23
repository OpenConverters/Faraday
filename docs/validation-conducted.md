# Validating the conducted pipeline against a measured board

ABT #810 asks for the thing that decides whether any of the conducted work can
be shown to an EMC engineer: a real converter, measured on a real LISN, run
through S1–S4, with the error reported as a stated band per decade rather than
a headline number.

This document is the first half of that, and it is honest about which half.

## Status

| | |
|---|---|
| Measured reference data (TIDA-01492) | **in hand** — digitised, two independent checks passed |
| Faraday's CISPR 25 Class 5 limit table | **validated** against an accredited lab's own receiver |
| A real board's full design pack | **in hand** — ADI DC3042A, ungated, and it is the CM/DM one |
| Pre-X2 Gerber sets | **importable** — `--layer-map`, stated not guessed |
| Board through S1–S4 | **blocked on nets** — see "the netlist bridge" |
| Per-mode (CM/DM) comparison | needs the bench, but the board is now settled |

Two boards are in play and they are blocked on different things, which is the
useful part: TIDA-01492 has the public measurement and a gated layout, and the
DC3042A has the public layout and a published CM/DM measurement.

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

## The second board: ADI DC3042A — layout in hand

The R&S comparison above is not just a benchmark, it names a board, and that
board's **full design pack is public and ungated**:
`analog.com/media/en/evaluation-documentation/evaluation-design-files/dc3042a.zip`
— 4.2 MB containing the Gerber set, the PADS PowerPCB layout database
(`.ASC` + `.PCB`), the OrCAD schematic (`.DSN` + PDF), the demo manual and the
BOM. LTC3310, 5 V → 1.2 V, 2 MHz, four layers.

It is the better candidate of the two, for a reason that has nothing to do with
convenience: R&S have already published a **CM/DM-separated** measurement of
it, and it carries an input EMI filter switched in by feeding `VIN EMI` instead
of `VIN` — so filtered and unfiltered are the same hardware with one lead
moved. That is the pair that makes a filter prediction mean something rather
than a single point.

### What it took to get the copper in — and what it did not

The Gerbers are PADS VX.2.6 RS-274X: **no X2 attributes, no Protel extensions**
(they are `L1.pho` … `L4.pho`). Faraday refused them, correctly — the layer
identity lives in a file name that means nothing outside PADS, and `L1` is a
copper layer here and a legend layer in someone else's export.

Guessing from the name would have been the wrong fix. `--layer-map
L1=1,L2=2,L3=3,L4=4` is the right one: the caller **states** the mapping, the
importer honours a stated fact over an inferred one, and the refusal message
now names the way out. That is a general capability, not a workaround for this
board — every pre-X2 vendor pack becomes importable on the same terms.
Pinned by `[gerber][layermap]`.

### The netlist bridge — where this stops today

With the layers stated, the import gets one step further and then refuses again,
also correctly: no `%TO.N` attributes and no IPC-D-356 member, so there are no
nets. Without nets most of the analysis is meaningless and all of the conducted
pipeline is.

The connectivity **does** exist in the pack. The PADS `*ROUTE*` section is a
complete description — signal name, the pin pair it runs between
(`E15.1` → `U1.1`), then a polyline of (x, y, layer, width, flags) with vias
marked. Faraday already reads IPC-D-356 and pairs it with a plain Gerber set,
so the bridge is a converter, not a new importer.

What stopped it here is the **coordinate unit**, and it is worth being precise
about why rather than picking one:

* the header constants fit `38100 counts = 1 mil` cleanly — `TEXTSIZE 3810000`
  = 100 mil, `USERGRID 190500` = 5 mil, `DOTGRID` = 100 mil, `REAL WIDTH 38100`
  = 1 mil, all classic PADS defaults;
* but the via drills do not confirm it. Under that scale `MICROVIA 457200` = 12
  mil and matches drill tool `T2C.012` exactly, while `STANDARDVIA` = 15 mil and
  `TENTED/VIA/BOTTOM` = 10 mil match no tool in the file (the list has .011 and
  .012). Under a nanometre scale nothing matches at all.
* The gap is probably drilled-vs-finished diameter, but "probably" is not a
  scale factor. A wrong one puts every parasitic out by a constant nobody would
  see, because the board would still look like a board.

So it was refused rather than assumed, and then **settled by measurement**
(`scripts/pads_to_ipc356.py`, ABT #858). Two independent bodies of evidence,
neither of them a default:

1. **The trace widths.** Every PADS segment width must equal some Gerber
   aperture diameter, because they are the same traces. Each pairing proposes
   a scale; the true one is proposed by many widths at once and the rest are
   coincidences. Here **6 of 6 widths agree** on 2.624672e-08 inch per count —
   which is 1/38,100,000 inch, i.e. the `38100 = 1 mil` the header constants
   suggested.
2. **The drills, which had no part in choosing it.** Every PADS via must land
   on a hole in the Excellon file. It does: **median residual 0.0000 mil over
   611 vias, maximum 0.0001** — and the fit also recovered a **CAM origin
   offset of exactly +1.000 inch in both axes**, which no amount of arithmetic
   on header constants would ever have revealed. That offset is why "check the
   scale" and "check the alignment" are the same operation.

A residual above the bound is a refusal, not a warning, because a wrong scale
is invisible: the board still looks like a board.

### It imports

    faraday_cli <gerber-dir> --stackup default-4layer \
        --layer-map L1=1,L2=2,L3=3,L4=4

with the generated `.ipc` alongside, gives real nets — `GND`, `SW`, `VFB` —
and **IPC-356 netting reaching 95% of routed copper** (90,729 of 95,669 mm).
The DC3042A is inside Faraday.

Two things surfaced on the way in, both fixed:

* The IPC-D-356 reader **silently dropped every record** of a netlist whose
  records carry no pad-size field and write the coordinate sign as a leading
  blank — the Y field was delimited by "the next space", which for a
  blank-signed value is the sign itself. The set then reported as "no IPC-D-356
  netlist", a wrong diagnosis pointing at the wrong file.
* `--layer-map` states which layer a file *is*, not which face it *faces*.
  Calling them all inner named a 4-layer board `In0..In3`, with no Top and no
  Bottom for an outer-layer or reference-plane rule to find. Position in the
  sorted stack decides the face.

### What S1 still needs

The deck export gets as far as finding the switch node (`--switch-net SW`) and
then stops honestly: no commutation loop. Two gaps remain, both properties of
this export rather than of Faraday:

* **Pin inventory.** The `*ROUTE*` section names the pins at the ENDS of routed
  runs, and that is all this converter can seed. A pin that reaches its net
  through a pour has no run and therefore no seed. The BASIC PADS export has no
  `*NET*` section, so the rest has to be reconstructed geometrically from
  `*PART*` + `*PARTDECAL*` pin positions against the copper.
* **Pour identity.** PADS *paints* its planes — `HATCHGRID` in the header, and
  not one `G36` region in any of the four Gerbers. The copper is all there
  (each inner layer reads ~22,000 mm of "routed" track), but with no region
  primitive the plane test sees 0% coverage, so the board has no reference
  plane and every return-path and coupling rule degrades to geometry-only. That
  affects any painted-pour vendor pack, not just this one.

Both are ABT #861. Component **values** are a third input, and they are
available: the demo manual carries the parts list.

## Until then

The panel's wording stands as it is: the conducted numbers are a modelled
prediction, not a compliance statement, and they must keep saying so.
