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
import { browse, fetchRecord } from '@kelvin/engine.js'
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

export function packageMatches(pkg, row) {
  if (!pkg) return 'unknown'
  const rp = rowPackage(row)
  if (!rp) return 'unknown'
  if (pkg.kind === 'chip') return rp.code === pkg.code ? 'match' : 'differs'
  return rp.code.startsWith(pkg.code) || pkg.code.startsWith(rp.code) ? 'match' : 'differs'
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

export async function candidatesByValue(value, pkg, { onFamily } = {}) {
  const spec = VALUE_FIELD[value.kind]
  if (!spec) return null
  onFamily?.(spec.family)
  const filters = { [spec.field]: { min: value.si * (1 - spec.tol), max: value.si * (1 + spec.tol) } }
  if (value.kind === 'C' && value.ratedV) filters.v_rated = { min: value.ratedV }
  const page = await browse(spec.family, {
    filters, sort: { field: 'lineno', dir: 'asc' }, limit: POOL })
  const rows = [], unknownCase = [], differs = []
  for (const r of page.rows) {
    const m = packageMatches(pkg, r)
    if (m === 'match') rows.push(r)
    else if (m === 'unknown') unknownCase.push(r)
    else differs.push(r)
  }
  return { family: spec.family, field: spec.field, tol: spec.tol,
           total: page.total, scanned: page.rows.length,
           rows, unknownCase, differs }
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
