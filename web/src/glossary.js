// The glossary: every problem Faraday can find, in one place — what it is, why
// it matters, how it is detected, and what the confidence really means. This is
// documentation as data so the panel and the rule filters stay in sync with
// one list.

export const RULES = [
  {
    id: 'coupled-run',
    name: 'Coupled run',
    what: 'Two nets routed parallel and close for long enough to couple. Reported with a saturated near-end crosstalk (NEXT) estimate in dB.',
    physics: 'Johnson & Graham k = 0.25/(1+(d/h)²), length-saturated. Measured against a 2D field solve this closed form runs ~6.5 dB optimistic, uniformly — so the RANKING is reliable, the absolute figure is not.',
    fix: 'Increase the gap (3W), shorten the parallel run, or add a grounded guard with stitching vias. Or open the bench and solve the real cross-section.',
    confidence: 'screening-estimate (±6 dB stated)',
  },
  {
    id: '3w',
    name: '3W violation',
    what: 'Edge separation under three trace-widths on a coupled run — the classic layout rule of thumb, reported alongside its quantified partner.',
    physics: 'Pure geometry. At 3W the edge coupling has fallen enough for most digital work; inside it, crosstalk grows fast.',
    fix: 'Space the traces to at least 3× the width, centre to centre.',
    confidence: 'geometric-only',
  },
  {
    id: 'diff-pair',
    name: 'Differential pair (informational)',
    what: 'A tightly coupled run whose net names identify it as a differential pair. Coupling here is intentional, so it is reclassified to info rather than flagged.',
    physics: 'Recognized by name suffix (P/N, +/-, H/L, M/P).',
    fix: 'Nothing — keep gap and length symmetric along the whole run.',
    confidence: 'exact',
  },
  {
    id: 'diff-skew',
    name: 'Differential-pair skew',
    what: 'Intra-pair routed-length mismatch on a recognized pair, in mm and approximate ps.',
    physics: 'Skew converts differential signal into common mode at every edge, and common-mode current is what reaches the cable. Lengths are summed routed copper — a geometric fact.',
    fix: 'Length-match with serpentines AT the mismatch point, on the same layer as the mismatch.',
    confidence: 'exact',
  },
  {
    id: 'plane-crossing',
    name: 'Return-path break (plane crossing)',
    what: 'A signal crossing a void or split in its reference plane. The return current cannot follow and detours around the obstruction.',
    physics: 'The detour encloses loop area — the dominant mechanism behind both emission and coupling. The return-path layer shows the same defect spatially.',
    fix: 'Reroute around the void, close the split, or add a stitching capacitor across it.',
    confidence: 'exact (checked against the actual pour polygons)',
  },
  {
    id: 'sparse-reference',
    name: 'Sparse reference',
    what: 'Signals routed over a layer whose copper coverage is too thin to act as a solid return.',
    physics: 'A patchy pour forces returns through whatever copper exists, enlarging loops unpredictably.',
    fix: 'Solidify the pour under the routing, or move the routing over a real plane.',
    confidence: 'geometric-only',
  },
  {
    id: 'no-reference-plane',
    name: 'No reference plane',
    what: 'A routed layer with no plane above or below it at all — no defined return path for anything on it.',
    physics: 'Loops close over the whole board. On the return-path layer these traces carry the worst effective height.',
    fix: 'Add a plane layer, or route critical signals elsewhere.',
    confidence: 'exact',
  },
  {
    id: 'switch-node',
    name: 'Switch node',
    what: 'A converter’s switching net, found by connectivity (FET + inductor pattern), with the copper area it exposes.',
    physics: 'High dV/dt copper is an E-field source: every mm² couples displacement current into whatever lies near or under it.',
    fix: 'Minimise the exposed copper consistent with thermal needs; keep sensitive routing away and off adjacent layers.',
    confidence: 'heuristic (connectivity pattern)',
  },
  {
    id: 'commutation-loop',
    name: 'Commutation loop',
    what: 'The hot loop of a converter — input cap, switch pair, return — with its enclosed area measured off the copper.',
    physics: 'This loop carries the discontinuous switching current; area × dI/dt sets ringing and radiated emission. The emissions panel turns this area into a margin against CISPR/FCC.',
    fix: 'Move the input capacitor tight to the FET pair; put the return plane directly beneath the loop.',
    confidence: 'heuristic (hull of the identified members)',
  },
  {
    id: 'via-stub',
    name: 'Via stub',
    what: 'The unused barrel below the last used layer of a via — an open-circuit stub.',
    physics: 'A quarter-wave resonator: at f = c/(4·L·√εr) it transforms the open into a short. Reported with that frequency.',
    fix: 'Back-drill, use blind/buried vias, or keep stubs short relative to the edge bandwidth.',
    confidence: 'exact',
  },
  {
    id: 'dangling-stub',
    name: 'Dangling stub',
    what: 'A routed trace with an unterminated open end — test points, abandoned routes, unpopulated options.',
    physics: 'Same quarter-wave physics as the via stub, plus an antenna at its resonance. Automated scans have found debug test points among the strongest emitters on a board.',
    fix: 'Remove the copper, or terminate it.',
    confidence: 'exact',
  },
  {
    id: 'decoupling-distance',
    name: 'Decoupling reach',
    what: 'A decoupling capacitor placed far from the IC it serves.',
    physics: 'Distance is inductance (~0.8 nH/mm of escape), and inductance is what a decap fights. The PDN panel turns this into the actual impedance curve.',
    fix: 'Move the cap to the pin, on the same side if possible; shorten the via escape.',
    confidence: 'geometric-only',
  },
]

// The deep tools — not findings, but part of the same vocabulary.
export const TOOLS = [
  { id: 'bench', name: 'Bench (field solve)', what: '2D boundary-element extraction of a coupled pair + transient — millivolts on the victim vs its receiver threshold.' },
  { id: 'emissions', name: 'Emissions', what: 'Commutation-loop area against CISPR 32 / FCC limit lines, plus the cable common-mode current budget.' },
  { id: 'near-field', name: 'Near field', what: 'Quasi-static |H| at component scale: which sensitive parts sit in the switching field, in A/m — never dBµV/m.' },
  { id: 'return-path', name: 'Return path', what: 'Effective loop height of every trace — geometry only, no assumed currents.' },
  { id: 'pdn', name: 'PDN', what: 'Rail impedance from every decap as an R-L-C branch with its mounting inductance measured off the board.' },
]
