// The parts inspector's side of the bridge to Kelvin: what the BOARD says
// about a component (a reference, a value string, a footprint name, some
// pads) turned into the questions a parts catalogue can answer — which
// catalogue family to open, which strings might be a part number, what value
// and package to match when there is no part number at all — and the answers
// carried back as Kelvin's own rows, records and cross-reference verdicts.
//
// Nothing here ranks, scores or decides a substitute. That is Kelvin's C++
// ranker behind runCrossRef, reached through Kelvin's own browser code
// (@kelvin/*, the sibling checkout), so a verdict shown on a Faraday board is
// the verdict kelvin.openconverters.com would show for the same part.
//
// The one thing this file does decide is HOW SURE it is. A refdes prefix is
// a convention, not a contract (ANSI, DIN and IEC disagree on what T, Q and L
// mean, and real boards mix them), so prefixes only ORDER the families to
// search — they never exclude one. A part number is matched by string, and
// the caller is told whether the match was exact or a substring.
import { browse, ensureShard, fetchRecord } from '@kelvin/engine.js'
import { runCrossRef } from '@kelvin/crossref.js'
import { FAMILIES } from '@kelvin/families.js'

export const ALL_FAMILIES = FAMILIES.map(f => f.key)

// ---- the value string -------------------------------------------------------
// "100n", "4u7", "2k2", "3R3", "0.1uF", "2.2µF 16V X7R", "10uF/25V", "1M".
// The unit letter decides the kind when it is there; when it is not ("100n")
// the kind comes from the caller's hint, because 100n is 100 nF on a
// capacitor and 100 nH on an inductor and the string alone cannot tell.
const PREFIX = { p: 1e-12, n: 1e-9, u: 1e-6, m: 1e-3, k: 1e3, K: 1e3, M: 1e6, G: 1e9, '': 1 }
const KIND_UNIT = { C: 'F', R: 'Ω', L: 'H' }

export function parseValue(raw, kindHint = null) {
  if (!raw) return null
  const s = String(raw).replace(/[µμ]/g, 'u').replace(/[Ωω]|ohms?/gi, 'R')
  const fields = s.split(/[\s,/;]+/).filter(Boolean)
  if (!fields.length) return null
  const first = fields[0]
  let num = null, mult = null, unit = null
  // infix notation: 4u7, 2k2, 3R3, 1M5, 0R
  let m = first.match(/^(\d+)([pnumkKMGR])(\d*)$/)
  if (m) {
    num = Number(m[1] + (m[3] ? '.' + m[3] : ''))
    if (m[2] === 'R') { mult = 1; unit = 'R' } else mult = PREFIX[m[2]]
  } else {
    // suffix notation: 100n, 100nF, 0.1uF, 10k, 47R, 1M, 100 (bare: refused)
    m = first.match(/^(\d+(?:\.\d+)?)([pnumkKMG]?)([FHR]?)$/)
    if (!m) return null
    num = Number(m[1]); mult = PREFIX[m[2]]; unit = m[3] || null
    if (!m[2] && !m[3]) {
      // "100 nF": one value written with a space in it
      const two = fields[1]?.match(/^([pnumkKMG]?)([FHR])$/)
      if (two) { mult = PREFIX[two[1]]; unit = two[2] }
      else if (kindHint !== 'R') return null   // a bare number is a guess
      else mult = 1
    }
  }
  const kind = unit === 'F' ? 'C' : unit === 'H' ? 'L' : unit === 'R' ? 'R'
    : (mult >= 1e3 ? 'R' : kindHint)   // k/M prefixes only ever mean ohms here
  if (!kind) return null
  let ratedV = null
  for (const f of fields.slice(1)) {
    const v = f.match(/^(\d+(?:\.\d+)?)\s*V(?:DC|AC)?$/i)
    if (v) { ratedV = Number(v[1]); break }
  }
  return { kind, si: num * mult, unit: KIND_UNIT[kind], ratedV, raw: String(raw) }
}

// ---- the footprint name ----------------------------------------------------
// "Capacitor_SMD:C_0603_1608Metric" (KiCad), "Resistor_SMD_R_0603_1608Metric"
// (ODB++ from KiCad), "CAPC1608X08N" (IPC-7351, Altium), "SOT23-3",
// "Package_TO_SOT_THT:TO-220-3_Vertical". The chip codes are ambiguous on
// purpose — 0603 is an imperial size AND the metric name of an imperial
// 0201 — so a metric marker wins when it is present, and the imperial code is
// read only where it is spelled as one.
const METRIC_TO_IMPERIAL = {
  '0603': '0201', '1005': '0402', '1608': '0603', '2012': '0805', '3216': '1206',
  '3225': '1210', '4516': '1806', '4532': '1812', '5025': '2010', '5750': '2220',
  '6332': '2512',
}
const IMPERIAL = new Set(Object.values(METRIC_TO_IMPERIAL))
const PKG_RE = /(SOT-?\d+(?:-\d+)?|SOD-?\d+|TO-?\d+(?:-\d+)?|D2?PAK|DPAK|SMA|SMB|SMC|DO-?\d+[A-Z]*|SOIC-?\d+|SO-?\d+|TSSOP-?\d*|MSOP-?\d*|SSOP-?\d*|QFN-?\d*|DFN-?\d*|LQFP-?\d*|TQFP-?\d*|QFP-?\d*|BGA|WSON|PowerPAK|LFPAK|SIP-?\d*|DIP-?\d+)/i

export function footprintName(footprint) {
  if (!footprint) return ''
  const i = footprint.indexOf(':')
  return i >= 0 ? footprint.slice(i + 1) : footprint
}

// ---- the size the BOARD says the part is -----------------------------------
// A footprint name is not the only evidence of size, and on a part like an
// inductor it is often the worst: "L_12x12mm_H8mm" or a bare vendor series
// name tells a regex nothing. The PADS tell the truth — they are where the
// part physically sits — so the land pattern's own extent is measured and used
// when the name yields no standard code.
//
// A land pattern is LARGER than the body it carries (pads extend past the ends
// of a chip, and a wound component's pads sit under its footprint), so this is
// an upper bound on the body, not the body. It is used as a bound: a catalogue
// part whose own drawing is bigger than the land cannot sit on it.
export function landSize(pads) {
  if (!pads?.length) return null
  let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity
  for (const p of pads) {
    x1 = Math.min(x1, p.x - p.w / 2); y1 = Math.min(y1, p.y - p.h / 2)
    x2 = Math.max(x2, p.x + p.w / 2); y2 = Math.max(y2, p.y + p.h / 2)
  }
  if (!(x2 > x1) || !(y2 > y1)) return null
  const [lMm, wMm] = [x2 - x1, y2 - y1].sort((a, b) => b - a)
  return { lMm, wMm }
}

// How much bigger than its land a real part may be before the fit is a lie.
// Pads normally stick OUT past a chip body, so a part bigger than its land in
// either axis is the suspicious direction; a little slack covers a wound part
// whose body overhangs its pads and the courtyard conventions that differ by
// library.
const LAND_SLACK = 1.25

export function packageOf(footprint) {
  const name = footprintName(footprint)
  if (!name) return null
  let m = name.match(/(\d{4})Metric/i) || name.match(/^(?:CAP|RES|IND|LED|DIO)[A-Z]*(\d{4})X/i)
  if (m && METRIC_TO_IMPERIAL[m[1]])
    return { code: METRIC_TO_IMPERIAL[m[1]], kind: 'chip' }
  m = name.match(/(?:^|[^0-9])(\d{4})(?![0-9])/)
  if (m && IMPERIAL.has(m[1])) return { code: m[1], kind: 'chip' }
  m = name.match(PKG_RE)
  if (m) return { code: m[1].toUpperCase().replace(/[-\s]/g, ''), kind: 'pkg' }
  return null
}

// A catalogue row's case code, on the same footing: "0603 (1608 Metric)" and
// "0603" are both 0603; "TO-220AB", "TO220-3" and "TO-220" all say TO220.
export function rowPackage(row) {
  const c = row?.caseCode
  if (!c) return null
  let m = c.match(/(?:^|[^0-9])(\d{4})(?![0-9])/)
  if (m && IMPERIAL.has(m[1])) return { code: m[1], kind: 'chip' }
  m = c.match(PKG_RE)
  if (m) return { code: m[1].toUpperCase().replace(/[-\s]/g, ''), kind: 'pkg' }
  return { code: c.toUpperCase().replace(/[-\s]/g, ''), kind: 'other' }
}

// ---- the size a chip part actually is --------------------------------------
// Body length x width in mm for each imperial chip code (EIA / IEC 60384-21).
// These are the standard's nominal bodies, not one vendor's drawing.
const CHIP_MM = {
  '0201': [0.60, 0.30], '0402': [1.00, 0.50], '0603': [1.60, 0.80],
  '0805': [2.00, 1.25], '1206': [3.20, 1.60], '1210': [3.20, 2.50],
  '1806': [4.50, 1.60], '1812': [4.50, 3.20], '2010': [5.00, 2.50],
  '2220': [5.70, 5.00], '2512': [6.30, 3.20],
}
// Tolerance on each axis. Wide enough for real drawings (a 0603 body is
// 1.60 +/- 0.15) and narrow enough that neighbours stay distinct: the closest
// pair on the long axis is 0603 vs 0805 (1.60 vs 2.00), which +/-12% keeps
// apart on the SHORT axis (0.80 vs 1.25), and both axes must agree.
const CHIP_TOL = 0.12

// The standard chip size a row's measured body IS, or null when it is not a
// standard chip at all. Orientation-agnostic: a drawing may list the axes
// either way round, so both are sorted before comparing.
export function rowSize(row) {
  const d = dimsOf(row)
  if (!d) return null
  const [a, b] = [d.lMm, d.wMm]
  for (const [code, [nl, nw]] of Object.entries(CHIP_MM)) {
    if (Math.abs(a - nl) <= CHIP_TOL * nl && Math.abs(b - nw) <= CHIP_TOL * nw)
      return { code, kind: 'chip', lMm: a, wMm: b }
  }
  return { code: null, kind: 'other', lMm: a, wMm: b }
}

// Does this catalogue row fit the land pattern the board drew?
//
// For a CHIP part the measured body decides, not the case string, because the
// case string is genuinely ambiguous and vendors disagree: Murata ships a
// 1.0 x 0.5 mm capacitor whose case field reads "0603" — the METRIC code for
// what everyone else calls an imperial 0402. Read as imperial that part is
// offered as fitting a 1.6 mm land it is 0.6 mm too short for. Measured, it
// cannot be. So dimensions win where they exist, the case code answers where
// they do not, and `basis` says which one spoke.
//
// For a non-chip package (TO-220, SOT-23, a can) the code string stays the
// check: those are families of drawings with real variation, not one
// standardised rectangle, and a dimension window would reject the variants.
//
// Returns { verdict: 'match'|'differs'|'unknown', basis: 'size'|'case'|null }.
export function packageMatches(pkg, row, land = null) {
  if (pkg?.kind === 'chip') {
    const size = rowSize(row)
    if (size) {
      // a body that is no standard chip cannot sit on a standard chip land
      return { verdict: size.code === pkg.code ? 'match' : 'differs', basis: 'size' }
    }
  }
  const rp = pkg ? rowPackage(row) : null
  if (rp) {
    if (pkg.kind === 'chip')
      return { verdict: rp.kind === 'chip' && rp.code === pkg.code ? 'match' : 'differs', basis: 'case' }
    const fits = rp.code.startsWith(pkg.code) || pkg.code.startsWith(rp.code)
    return { verdict: fits ? 'match' : 'differs', basis: 'case' }
  }
  // Neither a standard code on the row nor one readable from the footprint
  // name — which is the ordinary case for inductors, transformers and every
  // vendor-specific outline. The board still knows how much room the part has,
  // and the catalogue row still carries the part's own drawing, so the two can
  // be compared directly instead of giving up.
  const l = landSize(land)
  const dims = dimsOf(row)
  if (l && dims) {
    const fits = dims.lMm <= l.lMm * LAND_SLACK && dims.wMm <= l.wMm * LAND_SLACK
    return { verdict: fits ? 'match' : 'differs', basis: 'land',
             land: l, part: dims }
  }
  return { verdict: 'unknown', basis: null }
}

// A catalogue row's own body, longest axis first, in mm.
export function dimsOf(row) {
  const l = row?.lengthM, w = row?.widthM
  if (!(l > 0) || !(w > 0)) return null
  const [lMm, wMm] = [l * 1000, w * 1000].sort((a, b) => b - a)
  return { lMm, wMm }
}

// ---- which families to open, in what order ---------------------------------
const NOT_A_PART = /mounting|hole|fiducial|logo|testpoint|test_point|tp_|symbol|marking|net[-_ ]?tie|jumper/i

const BY_PREFIX = [
  [/^C[A-Z]?\d/i, ['capacitor']],
  [/^R[A-Z]?\d/i, ['resistor']],
  [/^(RV|VR|MOV|VDR|ZNR)\d/i, ['varistor']],
  [/^(L|FB|FL|T|TR|TX)\d/i, ['magnetic']],
  [/^Q\d/i, ['mosfet', 'bjt', 'igbt']],
  [/^D\d/i, ['diode']],
  [/^(U|IC)\d/i, ['controller', 'analog']],
  [/^(Y|X|XTAL|OSC)\d/i, ['timing']],
  [/^(J|P|CN|CON|K|X)\d/i, ['connector']],
]
const BY_FOOTPRINT = [
  [/^(C_|CAPC|CAP|CP_)|Capacitor|Elko/i, ['capacitor']],
  [/^(R_|RESC|RES)|Resistor/i, ['resistor']],
  [/^(L_|IND)|Inductor|Choke|Transformer|Bead|Ferrite/i, ['magnetic']],
  [/Varistor|MOV/i, ['varistor']],
  [/Crystal|Oscillator|XTAL|Resonator/i, ['timing']],
  [/Conn|Header|Terminal|Socket|USB|RJ\d|JST|Molex|Phoenix|MKDS|Screw/i, ['connector']],
  [/SOD|\bSMA\b|\bSMB\b|\bSMC\b|DO-?\d|Diode|^D_/i, ['diode']],
  [/SOT|TO-?\d|D2?PAK|DFN|PowerPAK|LFPAK|SO-?8|SOIC-?8/i, ['mosfet', 'diode', 'bjt', 'igbt']],
  [/QFN|QFP|TSSOP|MSOP|SSOP|BGA|SOIC|SOP|DIP/i, ['controller', 'analog']],
]
const BY_KIND = { C: ['capacitor'], R: ['resistor'], L: ['magnetic'] }

export function isPart(comp, pads) {
  if (!pads?.length) return false
  // a mounting hole with a plated pad, a fiducial, a test point, a logo: on
  // the board, but nothing a catalogue could be asked about
  if (NOT_A_PART.test(comp.value || '') || NOT_A_PART.test(footprintName(comp.footprint)))
    return false
  return true
}

export function familyOrder(comp) {
  const out = []
  const add = fams => { for (const f of fams) if (!out.includes(f)) out.push(f) }
  const hint = refdesKind(comp.ref)
  const v = parseValue(comp.value, hint)
  if (v) add(BY_KIND[v.kind])
  const fn = footprintName(comp.footprint)
  for (const [re, fams] of BY_FOOTPRINT) if (re.test(fn)) add(fams)
  for (const [re, fams] of BY_PREFIX) if (re.test(comp.ref || '')) add(fams)
  return out
}

function refdesKind(ref) {
  if (/^C[A-Z]?\d/i.test(ref || '')) return 'C'
  if (/^R[A-Z]?\d/i.test(ref || '')) return 'R'
  if (/^(L|FB|FL)\d/i.test(ref || '')) return 'L'
  return null
}

export function valueOf(comp) {
  return parseValue(comp.value, refdesKind(comp.ref))
}

// ---- what might be a part number ------------------------------------------
// The value field first ("IPA045N10N3G", "BC846B"); and when the export wrote
// the part number in the footprint slot and no value at all (Altium's ODB++
// does), the footprint name. A footprint that is plainly a package
// ("SOT-23", "C_0603_1608Metric") is never offered as a part number.
export function mpnCandidates(comp) {
  const out = []
  const push = s => {
    const t = String(s || '').trim().split(/\s+/)[0]
    if (!t || t.length < 4) return
    if (!/\d/.test(t) || !/[A-Za-z]/.test(t)) return
    // a value-shaped token ("100n", "4u7", "2k2", "50V") is never a part number
    if (/^\d+(?:\.\d+)?[pnumkKMGR]?\d*[FHRV]?$/i.test(t)) return
    if (parseValue(t, refdesKind(comp.ref))) return
    if (NOT_A_PART.test(t)) return
    if (!out.includes(t)) out.push(t)
  }
  push(comp.value)
  if (!comp.value) {
    const fn = footprintName(comp.footprint)
    if (!packageOf(fn) && !/_/.test(fn)) push(fn)
  }
  return out
}

const canon = s => String(s || '').toUpperCase().replace(/[^A-Z0-9]/g, '')

// Search the given families, in order, for the candidate strings. Stops at
// the first EXACT hit (case- and punctuation-insensitive); otherwise returns
// every substring hit found on the way. `onFamily` reports each family as it
// is opened — a shard is a download the caller should be able to see.
export async function identify(comp, families, { onFamily } = {}) {
  const cands = mpnCandidates(comp)
  const result = { tried: cands, searched: [], exact: null, near: [] }
  if (!cands.length) return result
  for (const family of families) {
    onFamily?.(family)
    for (const q of cands) {
      const page = await browse(family, {
        filters: { mpn: q }, sort: { field: 'mpn', dir: 'asc' }, limit: 25 })
      for (const row of page.rows) {
        const hit = { family, row, query: q }
        if (canon(row.mpn) === canon(q)) {
          result.exact = hit
        } else if (!result.near.some(n => n.family === family && n.row.mpn === row.mpn &&
                                         n.row.manufacturer === row.manufacturer)) {
          result.near.push(hit)
        }
      }
      if (result.exact) break
    }
    result.searched.push(family)
    if (result.exact) break
  }
  return result
}

// A free-text part-number search across the given families (the manual
// "find the original" path).
export async function searchMpn(q, families, { onFamily, limit = 15 } = {}) {
  const hits = []
  for (const family of families) {
    onFamily?.(family)
    const page = await browse(family, {
      filters: { mpn: q }, sort: { field: 'mpn', dir: 'asc' }, limit })
    for (const row of page.rows) hits.push({ family, row })
  }
  return hits
}

// ---- no part number: match the value and the package ------------------------
// Windows are tighter than an E-series step so a neighbour value never
// qualifies (E96 resistors sit 2.3% apart; E24 capacitors ~10%).
const VALUE_FIELD = { C: { family: 'capacitor', field: 'capacitance', tol: 0.02 },
                      R: { family: 'resistor', field: 'resistance', tol: 0.005 },
                      L: { family: 'magnetic', field: 'inductance', tol: 0.02 } }
export const POOL = 800

export async function candidatesByValue(value, pkg, { onFamily, land = null } = {}) {
  const spec = VALUE_FIELD[value.kind]
  if (!spec) return null
  onFamily?.(spec.family)
  const filters = { [spec.field]: { min: value.si * (1 - spec.tol), max: value.si * (1 + spec.tol) } }
  if (value.kind === 'C' && value.ratedV) filters.v_rated = { min: value.ratedV }
  const page = await browse(spec.family, {
    filters, sort: { field: 'lineno', dir: 'asc' }, limit: POOL })
  const rows = [], unknownCase = [], differs = []
  let bySize = 0, byCase = 0, byLand = 0
  for (const r of page.rows) {
    const { verdict, basis } = packageMatches(pkg, r, land)
    if (verdict === 'match') {
      rows.push(r)
      if (basis === 'size') bySize++
      else if (basis === 'case') byCase++
      else if (basis === 'land') byLand++
    } else if (verdict === 'unknown') unknownCase.push(r)
    else differs.push(r)
  }
  return { family: spec.family, field: spec.field, tol: spec.tol,
           total: page.total, scanned: page.rows.length,
           rows, unknownCase, differs, bySize, byCase, byLand,
           land: landSize(land) }
}

// ---- the manufacturers a cross-reference can target -------------------------
export async function manufacturersOf(family) {
  const r = await browse(family, { withFacets: true, facetTop: 500, limit: 0 })
  return r.facets?.manufacturer?.values ?? []
}

// ---- the cross-reference itself: Kelvin's ranker, Kelvin's model ------------
export function crossReference({ family, original, manufacturers, sameType = true,
                                 maxResults = 12 }) {
  return runCrossRef({ family, original, manufacturers, sameType, maxResults })
}

// ---- the full record, one Range slice, cached for the session --------------
const _records = new Map()
export function recordOf(family, row) {
  const key = `${family}:${row.srcOffset}`
  if (!_records.has(key)) {
    _records.set(key, fetchRecord(family, row.srcOffset, row.srcLength).catch(e => {
      _records.delete(key)
      throw e
    }))
  }
  return _records.get(key)
}

// ---- the whole board at once: what can the catalogue actually resolve? -----
// The parts overlay draws every component. This answers a different question —
// which of them the catalogue can say anything about — and it is a question
// only the catalogue can answer, so it costs the family shards the board needs.
//
// Work is grouped BY FAMILY, not by part, so each shard is downloaded once and
// every component that might live in it is asked while it is warm. Parts the
// board describes too poorly to look up (no part number, no parseable value)
// are settled with no catalogue call at all.
export const SWEEP_STATES = ['exact', 'candidates', 'none', 'unlookupable']

export async function sweepBoard(parts, { onProgress, signal } = {}) {
  const out = new Map()
  const queue = []          // [{ ref, comp, families, value, pkg }]

  for (const comp of parts) {
    const value = valueOf(comp)
    const cands = mpnCandidates(comp)
    const families = familyOrder(comp)
    if (!cands.length && !value) {
      // nothing on the board to look up: not a catalogue miss, a board that
      // never said what the part is. Saying "not found" would blame the
      // catalogue for the export's silence.
      out.set(comp.ref, { state: 'unlookupable',
                          why: 'the board gives neither a part number nor a value' })
      continue
    }
    queue.push({ ref: comp.ref, comp, families, value, pkg: packageOf(comp.footprint), cands })
  }

  // every family any queued part might belong to, most-wanted first
  const wanted = new Map()
  for (const q of queue) {
    const fams = q.cands.length ? q.families.slice(0, 3)
      : (q.value ? [VALUE_FIELD[q.value.kind]?.family].filter(Boolean) : [])
    q.searchIn = fams
    for (const f of fams) wanted.set(f, (wanted.get(f) ?? 0) + 1)
  }
  const order = [...wanted.entries()].sort((a, b) => b[1] - a[1]).map(([f]) => f)

  let done = 0
  const total = queue.length
  for (const family of order) {
    if (signal?.aborted) return out
    onProgress?.({ phase: 'shard', family, done, total })
    try {
      await ensureShard(family)
    } catch (e) {
      // a family that will not load is reported, never silently skipped —
      // otherwise its parts would read as "not in the catalogue"
      onProgress?.({ phase: 'shardError', family, message: String(e.message || e) })
      continue
    }
    for (const q of queue) {
      if (signal?.aborted) return out
      if (out.get(q.ref)?.state === 'exact') continue
      if (!q.searchIn.includes(family)) continue
      try {
        if (q.cands.length) {
          const r = await identify(q.comp, [family])
          if (r.exact) { out.set(q.ref, { state: 'exact', hit: r.exact }); continue }
          if (r.near.length && !out.has(q.ref))
            out.set(q.ref, { state: 'candidates', count: r.near.length, by: 'mpn' })
        }
        if (q.value && !out.has(q.ref) && VALUE_FIELD[q.value.kind]?.family === family) {
          const v = await candidatesByValue(q.value, q.pkg)
          if (v?.rows.length)
            out.set(q.ref, { state: 'candidates', count: v.rows.length, by: 'value' })
        }
      } catch (e) {
        onProgress?.({ phase: 'partError', ref: q.ref, message: String(e.message || e) })
      }
    }
    done = queue.filter(q => out.has(q.ref)).length
    onProgress?.({ phase: 'family', family, done, total })
  }

  for (const q of queue)
    if (!out.has(q.ref))
      out.set(q.ref, { state: 'none',
                       why: q.cands.length
                         ? 'no catalogue part carries this number'
                         : 'no catalogue part of this value fits this footprint' })
  onProgress?.({ phase: 'done', done: total, total })
  return out
}

// ---- the end of the catalogue: ask Heaviside to source the part ------------
// When Kelvin has never heard of a part number, the catalogue is not the last
// word — Heaviside's librarian can look it up at the distributor, convert it
// to the same TAS envelope the catalogue serves, schema-validate it, and park
// it in staging for review. What comes back is therefore rendered by exactly
// the code that renders a catalogue record.
//
// Same-origin (/heaviside/…, proxied by nginx to the librarian on the box), so
// the page's connect-src 'self' CSP is untouched and the board still never
// leaves the machine — only the part NUMBER is sent, which is a public string
// printed on the part itself.
export const SOURCE_ENDPOINT = '/heaviside/librarian/lookup'

export async function sourcePart(mpn, category = null, { signal } = {}) {
  let res
  try {
    res = await fetch(SOURCE_ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mpn, category }),
      signal,
    })
  } catch (e) {
    // A transport failure is not "no such part": say which it was, or the
    // reader goes off to hand-enter a part that exists.
    throw new Error(`could not reach the librarian: ${e.message || e}`)
  }
  let body = null
  try { body = await res.json() } catch { /* handled below */ }
  if (!res.ok) {
    const detail = body?.detail || `HTTP ${res.status}`
    if (res.status === 404) {
      throw new Error(
        'the librarian is not reachable from this site — the part-sourcing ' +
        'route is not configured on this deployment')
    }
    // The limiter answers with a bare status and no JSON, so without this the
    // message is "HTTP 503" — which reads as the librarian being broken rather
    // than as you having asked it a lot in one minute.
    if (res.status === 429 || res.status === 503) {
      throw new Error(
        'too many lookups in one minute — each one spends a real distributor ' +
        'API call, so they are rate limited. Try again shortly.')
    }
    throw new Error(String(detail))
  }
  if (!body || typeof body !== 'object') throw new Error('the librarian returned no answer')
  return body
}

// A value string Faraday's own parser reads back ("100nF 50V"), for a part
// the export left nameless. Only the families whose rows carry a primary
// value; a MOSFET has no "value" to hand back.
export function valueStringFor(family, row) {
  const fmt = (x, unit) => {
    if (!(x > 0)) return null
    const steps = [[1e-12, 'p'], [1e-9, 'n'], [1e-6, 'u'], [1e-3, 'm'], [1, ''], [1e3, 'k'], [1e6, 'M']]
    let p = steps[0]
    for (const s of steps) if (x >= s[0] * 0.9999) p = s
    const n = x / p[0]
    const str = Number(n.toPrecision(4)).toString()
    return `${str}${p[1]}${unit}`
  }
  if (family === 'capacitor') {
    const c = fmt(row.capacitance, 'F')
    if (!c) return null
    return row.v_rated > 0 ? `${c} ${Number(row.v_rated.toPrecision(3))}V` : c
  }
  if (family === 'resistor') return fmt(row.resistance, 'R')
  if (family === 'magnetic') return fmt(row.inductance, 'H')
  return null
}
