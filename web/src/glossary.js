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
    what: 'A converter’s switching net, found by connectivity (FET + inductor pattern, with physics vetoes: a net with a capacitor straight to the return can never switch — the cap would short the switch every cycle), with the copper area it exposes.',
    physics: 'High dV/dt copper is an E-field source: every mm² couples displacement current into whatever lies near or under it.',
    fix: 'Minimise the exposed copper consistent with thermal needs; keep sensitive routing away and off adjacent layers.',
    confidence: 'heuristic (connectivity pattern), or user-declared for promoted candidates',
  },
  {
    id: 'switch-node-candidate',
    name: 'Candidate switch node',
    what: 'A net that looks like a MONOLITHIC converter — switcher IC + inductor, no discrete FET: a wound part and active silicon on a compact net, no capacitor to the return, and two distinct filtered rails (energy moves between them). Offered in the meta strip, never screened automatically.',
    physics: 'From layout data alone this shape is irreducibly ambiguous: a linear regulator followed by an LC filter presents the identical external netlist (measured on a real SDR board — feedback resistors, copper width and package size all fail to separate them). Faraday will not guess; it shows the evidence and lets you decide.',
    fix: 'If the net IS a switcher’s output, click it in the meta strip — it screens fully (commutation loop, near field, coupled-run boosts) and the exported report records the provenance as user-declared.',
    confidence: 'evidence shown, decision yours',
  },
  {
    id: 'commutation-loop',
    // (kept in sync with the C++ finding: derived meshes name their XOR
    // branches — "T7 + D22 + R29" — via Franz §4.4 current-switching analysis;
    // boards where device roles can't be inferred keep the geometric hull)
    name: 'Commutation loop',
    what: 'The hot loop of a converter, with its enclosed area measured off the copper. Where device roles are inferable from the netlist, the mesh is DERIVED by current-switching analysis (Franz §4.4) — the finding names its exact branches (e.g. "T7 + D22 + R29") and the shape (two-device, or winding + clamp). For a boost that mesh closes through the OUTPUT capacitor.',
    physics: 'Draw the current\'s circulation before and after the switch toggles; the branches carried in only ONE of the two form the critical mesh — the only copper carrying the current step. Area × dI/dt sets ringing and radiated emission; the emissions panel turns the area into a margin against CISPR/FCC.',
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
  {
    id: 'connector-ground-spread',
    name: 'Connector ground spread',
    what: 'Ground entries of off-board connectors scattered around the board instead of clustered — a series-ground structure toward the outside world.',
    physics: 'The ground potential difference between two cable roots drives the attached cables as a dipole; cable common-mode is what actually fails EMC tests. Franz (EMV 5th ed., §7.2–7.3) measured 23.5 dB between the scattered and the clustered layout of the same board. A continuous plane between the entries softens it (his Vermaschung), which is why the severity drops when one exists.',
    fix: 'Cluster the off-board connections on one edge so their grounds share one reference point (star structure). Where placement is fixed: lowest possible copper impedance between the entries, and a common-mode choke per cable.',
    confidence: 'heuristic',
  },
  {
    id: 'plane-cavity-mode',
    name: 'Plane cavity mode',
    what: 'The VCC/GND plane pair as a resonant 2-D cavity, with its first mode frequencies computed from the board size and dielectric.',
    physics: 'f_mn = c0/(2√εr)·√((m/a)²+(n/b)²) — Franz Gl. 5.3. Above the capacitor/plane parallel resonance the decaps stop acting and these modes set the supply impedance. Every mode peaks in the corners; the board centre is a null of the first three (17 dB measured on the 10-mode).',
    fix: 'Thinner plane spacing lowers the cavity impedance everywhere; lossy edge termination or ESR-controlled caps damp the modes; placing the switching cluster centrally stops the first three modes being driven.',
    confidence: 'heuristic',
  },
  {
    id: 'critical-mesh-ground',
    name: 'Critical mesh crosses ground domains',
    what: 'The commutation loop closes through a capacitor bridging two ground pours (GND↔AGND) — the switching current crosses a domain boundary.',
    physics: 'Franz §8.17.1, rule 1: the critical mesh must connect to the general ground at ONE point. When its return crosses a domain stitch, both domains carry the di/dt and everything referenced to either sees it. Found by running his Stromumschaltanalyse on the netlist derived from the layout.',
    fix: 'Return the switch source and the loop capacitor to the same pour; join the domains at a single star point away from the loop. If the stitch must carry the loop, place and size it as part of the hot loop.',
    confidence: 'heuristic',
  },
  {
    id: 'edge-radiation',
    name: 'Switch node at board edge',
    what: 'Switch-node copper running along the board edge, where the field is no longer confined between trace and plane.',
    physics: 'At the edge the plane\'s image current is truncated and the dv/dt fringes into free space — from the loudest copper on the board. A return-via fence between the copper and the edge contains most of it (severity drops when one is present).',
    fix: 'Pull the switch node inboard, keep the plane out to the edge beneath it, and fence the edge with stitching vias (pitch ≤ λ/10 at the highest aggressor harmonic).',
    confidence: 'geometric-only',
  },
  {
    id: 'pdn-antiresonance',
    name: 'PDN anti-resonance',
    what: 'A computed parallel-resonance peak in a rail\'s decoupling network — where paralleled capacitors (or the caps against the plane) fight instead of helping.',
    physics: 'Franz §5.5: between any two different series resonances a parallel resonance appears; equal-value caps in parallel produce none. §5.9.5: the caps\' total inductance against the plane capacitance forms the handover resonance that ENDS the capacitors\' authority. Computed from the same branch model as the PDN panel (measured mounting inductance; stated 15 mΩ ESR default; bulk >2.2 µF excluded — their real ESR damps).',
    fix: 'Damp, don\'t dodge: same-value caps instead of a decade ladder, ESR-controlled parts, or a small series R on one branch. Verify in the PDN panel.',
    confidence: 'screening-estimate',
  },
  {
    id: 'cap-via-stub',
    name: 'Decoupling via stub',
    what: 'A decoupling capacitor whose nearest same-net via into its plane is millimetres from the pad — the cap decouples through a trace stub.',
    physics: 'Above series resonance a capacitor IS its inductance, and the stub (~0.8 nH/mm) is in series with it. Franz §5.6: connection lengths in the decoupling branch must be as short as manufacturable; his via table shows a second via pair alone is worth ~19% of the branch inductance.',
    fix: 'A via pair directly beside each pad (checklist figure: within 0.3 mm), not at the end of a trace run.',
    confidence: 'geometric-only',
  },
]

// The deep tools — not findings, but part of the same vocabulary.
export const TOOLS = [
  { id: 'bench', name: 'Bench (field solve)', what: '2D boundary-element extraction of a coupled pair + transient — millivolts on the victim vs its receiver threshold.' },
  { id: 'emissions', name: 'Emissions', what: 'Commutation-loop area against CISPR 32 / FCC radiated limit lines, the cable common-mode current budget, and the CONDUCTED estimate judged per mode: which of common or differential mode dominates, at what frequency, and how many dB each filter stage must find.' },
  { id: 'near-field', name: 'Near field', what: 'Quasi-static |H| at component scale: which sensitive parts sit in the switching field, in A/m — never dBµV/m.' },
  { id: 'return-path', name: 'Return path', what: 'Effective loop height of every trace — geometry only, no assumed currents.' },
  { id: 'pdn', name: 'PDN', what: 'Rail impedance from every decap as an R-L-C branch with its mounting inductance measured off the board.' },
]

// ---------------------------------------------------------------------------
// Plain language — what GUIDED (basic) mode says instead
// ---------------------------------------------------------------------------
// The same findings, for someone who lays out boards but is not an EMC
// engineer: one sentence for what was found, one for what to do about it. No
// dB, no Greek, no standard numbers — those are all still there, one click
// away, in advanced mode. Nothing here softens a verdict; it only removes
// vocabulary. A rule with no entry falls back to its engineering text, which
// is why this map can never quietly hide a finding.
export const PLAIN = {
  'coupled-run': {
    says: 'These two tracks run side by side for long enough that whatever happens on one will show up on the other.',
    do: 'Move them apart — three track-widths is the usual minimum — shorten the parallel stretch, or run a grounded track with vias between them.',
  },
  '3w': {
    says: 'These two tracks are closer together than the usual safe spacing of three track-widths.',
    do: 'Space them at least three track-widths apart, measured centre to centre.',
  },
  'diff-pair': {
    says: 'This is a differential pair. The two tracks are meant to be coupled to each other, so this is information, not a fault.',
    do: 'Nothing to fix. Keep the gap between them, and their two lengths, the same the whole way along.',
  },
  'diff-skew': {
    says: 'One half of this pair is longer than the other, so the two halves of the signal no longer arrive together — and the difference leaks out onto the cable.',
    do: 'Add a small zig-zag to the shorter one, at the point where the mismatch happens.',
  },
  'plane-crossing': {
    says: 'This track crosses a gap in the copper underneath it. The return current cannot follow it across, so it takes a detour around the gap.',
    do: 'Route around the gap, close the gap, or bridge it with a small capacitor right where the track crosses.',
  },
  'sparse-reference': {
    says: 'There is not enough solid copper under these tracks for their return current to flow straight back.',
    do: 'Fill in the copper under the routing, or move these tracks over a layer that has a solid plane.',
  },
  'no-reference-plane': {
    says: 'This layer has no solid copper plane above or below it, so nothing routed on it has a defined way back.',
    do: 'Add a plane layer, or move the important signals onto a layer that has one.',
  },
  'switch-node': {
    says: 'This is the converter’s switching copper — the noisiest area on the board, and the source most other problems trace back to.',
    do: 'Keep it as small as the heat allows, and keep sensitive parts and tracks away from it — including on the layers directly underneath.',
  },
  'switch-node-candidate': {
    says: 'This net might be a switching converter, but from the layout alone it looks exactly like a linear regulator with a filter. Faraday will not guess between them.',
    do: 'If it is a switching converter, click it in the strip at the bottom of the window and the whole board gets checked against it.',
  },
  'commutation-loop': {
    says: 'This is the loop the current jumps into every time the converter switches. The area it encloses is what decides how loudly the board radiates.',
    do: 'Move the input capacitor hard against the switching devices, and keep solid ground copper directly underneath the loop.',
  },
  'via-stub': {
    says: 'Part of this via is unused. The leftover length behaves like a small resonator at high frequency.',
    do: 'Back-drill it, use a blind or buried via — or leave it, if nothing on the board is fast enough to care.',
  },
  'dangling-stub': {
    says: 'This track has a dead end that connects to nothing — a test point, or a route someone abandoned. Dead ends radiate.',
    do: 'Delete the copper, or terminate it.',
  },
  'decoupling-distance': {
    says: 'This decoupling capacitor sits too far from the chip it is meant to feed, so most of its benefit is lost in the distance.',
    do: 'Move it next to the pin, on the same side of the board if you can, with the shortest possible via escape.',
  },
  'connector-ground-spread': {
    says: 'The ground connections of the off-board connectors are scattered around the board, which turns the attached cables into antennas.',
    do: 'Bring the off-board connectors together on one edge so their grounds meet at a single point. Where that is fixed, add a common-mode choke on each cable.',
  },
  'plane-cavity-mode': {
    says: 'The power and ground planes behave like a drum that rings at particular frequencies. The corners of the board are the loudest places on it.',
    do: 'Move the plane pair closer together, keep the switching parts away from the corners, and damp the ringing with ESR-controlled capacitors.',
  },
  'critical-mesh-ground': {
    says: 'The switching current has to cross from one ground area into another, so both grounds — and everything referenced to them — carry the switching noise.',
    do: 'Return the switching parts to the same ground area, and join the two grounds at one single point, away from the switching loop.',
  },
  'edge-radiation': {
    says: 'The noisy switching copper runs along the edge of the board, where its field escapes into the air instead of staying between the track and the plane.',
    do: 'Move it inboard, keep the ground plane out to the edge underneath it, and add a row of stitching vias along the edge.',
  },
  'pdn-antiresonance': {
    says: 'The decoupling capacitors on this rail fight each other at one frequency instead of helping, and the supply impedance peaks there.',
    do: 'Use several capacitors of the SAME value rather than a mix of decades, or put a small resistor in one branch to damp it.',
  },
  'cap-via-stub': {
    says: 'This decoupling capacitor reaches its plane along a length of track instead of through a via at its own pad, which throws away much of what it was placed for.',
    do: 'Put a via pair right beside the capacitor’s pads.',
  },
}

// Risk word for guided mode — the severity label without the vocabulary.
export const PLAIN_SEVERITY = {
  high: 'Fix this',
  medium: 'Worth fixing',
  low: 'Minor',
  info: 'Information',
}

export const plainFor = rule => PLAIN[rule] || null
