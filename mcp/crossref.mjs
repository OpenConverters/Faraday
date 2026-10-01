// The worker behind the MCP server's `crossref_board` tool: the cross-reference of every
// component on a board, done by the code the Faraday web app runs when a board is loaded.
//
// Nothing here identifies a part or ranks a substitute. Identification is web/src/parts.js —
// refdes-prefix family ordering, value parsing, footprint packages, part-number string
// matching, the board sweep — and ranking is Kelvin's runCrossRef (@kelvin/crossref.js) over
// Kelvin's C++ ranker. Both are imported here as they are; a second copy in Python would
// disagree with the board in the browser within a release, and a board that is "identified"
// in a chat and not in the app is worse than one that is never identified at all.
//
// What this file adds is the ORDER the app asks those questions in, which lives in Vue
// components rather than in parts.js:
//
//   App.vue      boardParts = components with pads that are parts (isPart), then
//                sweepBoard(parts) — the load-time identification pass.
//   PartPanel    for a part the sweep did not identify exactly: identify() in the likeliest
//                three families, then — a miss there not being an answer — every other
//                family; if still not identified, candidatesByValue() with the land.
//                For an identified part: the family's manufacturers, every one but the
//                original's own marked, then crossReference(sameType, maxResults).
//
// Wiring: `@kelvin/*` resolves to Kelvin's web sources exactly as the vite configs alias it,
// and Kelvin's browser engine (a Web Worker plus HTTP) is swapped for kelvin-node.mjs (the
// same WASM over files on disk). One JSON request per stdin line, one reply per stdout line:
//
//   {"id":1,"op":"crossref_board","components":[...],"pads":[...],
//    "targets":["..."]|[],"sameType":true,"maxResults":5,"listed":5}
//   {"id":1,"ok":true,"result":{...}} | {"id":1,"ok":false,"error":"..."}
//
//   node crossref.mjs --shards <dir>

import { createHash } from 'node:crypto'
import { existsSync, readFileSync } from 'node:fs'
import { registerHooks } from 'node:module'
import { createInterface } from 'node:readline'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const FARADAY = resolve(HERE, '..')

function arg(name) {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : null
}

const SHARDS = arg('shards')
if (!SHARDS) throw new Error('crossref.mjs needs --shards <dir> (the server passes KELVIN_SHARD_DIR)')

// Same resolution, and the same refusal, as web/vite.config.js and mcp/vite.config.js.
const KELVIN_SRC = process.env.KELVIN_WEB_SRC || resolve(FARADAY, '..', 'Kelvin', 'web', 'src')
if (!existsSync(join(KELVIN_SRC, 'crossref.js'))) {
  throw new Error(`Kelvin's web sources not found at ${KELVIN_SRC} — parts.js imports them. ` +
    `Check out OpenConverters/Kelvin beside Faraday, or set KELVIN_WEB_SRC.`)
}
const KELVIN_WASM = resolve(KELVIN_SRC, '..', 'public', 'kelvin.js')

const BROWSER_ENGINE = pathToFileURL(join(KELVIN_SRC, 'engine.js')).href
const NODE_ENGINE = pathToFileURL(join(HERE, 'kelvin-node.mjs')).href

// Every file this worker actually loads, recorded as it resolves — the fingerprint below is
// computed over exactly these, so it cannot drift from a hand-kept list.
const loadedFiles = new Set([fileURLToPath(import.meta.url)])
registerHooks({
  resolve(specifier, context, nextResolve) {
    const spec = specifier.startsWith('@kelvin/')
      ? pathToFileURL(join(KELVIN_SRC, specifier.slice('@kelvin/'.length))).href
      : specifier
    let out = nextResolve(spec, context)
    if (out.url === BROWSER_ENGINE) out = { ...out, url: NODE_ENGINE, shortCircuit: true }
    if (out.url.startsWith('file:')) loadedFiles.add(fileURLToPath(out.url))
    return out
  },
})

const engine = await import(NODE_ENGINE)
await engine.configure({ shards: resolve(SHARDS), wasm: KELVIN_WASM })
loadedFiles.add(KELVIN_WASM)
const P = await import(pathToFileURL(join(FARADAY, 'web', 'src', 'parts.js')).href)
const { XREF, originalMissingKeys } = await import('@kelvin/crossref.js')

// ── helpers ─────────────────────────────────────────────────────────────────

// A manufacturer named by a person ("Würth", "wurth elektronik") against the catalogue's own
// spelling of it ("Wurth Elektronik"). Exact (case- and accent-insensitive) wins; otherwise
// every catalogue name containing it. Nothing matching is reported, never guessed.
const fold = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '')
  .toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim()

function resolveTargets(wanted, names) {
  const used = [], unknown = []
  for (const w of wanted) {
    const fw = fold(w)
    const exact = names.filter((n) => fold(n) === fw)
    const hits = exact.length ? exact : names.filter((n) => fw && fold(n).includes(fw))
    if (hits.length) for (const h of hits) { if (!used.includes(h)) used.push(h) }
    else unknown.push(w)
  }
  return { used, unknown }
}

// A shard row, trimmed to what describes the part: where it sits in the catalogue file is
// bookkeeping a consumer must not read as a spec.
function brief(row) {
  if (!row) return null
  const { srcOffset, srcLength, lineno, line, ...rest } = row
  return rest
}

async function crossrefPart(family, row, req, mfrCache) {
  if (!XREF.some((f) => f.key === family)) {
    // Kelvin's crossref.js falls back to its FIRST model for a family it does not know, which
    // would rank a controller as if it were a magnetic. Kelvin's own MCP refuses; so does this.
    return { skipped: `Kelvin has no cross-reference model for the ${family} family ` +
      `(it ranks ${XREF.map((f) => f.key).join(', ')})` }
  }
  if (!mfrCache.has(family)) mfrCache.set(family, (await P.manufacturersOf(family)).map(([m]) => m))
  const names = mfrCache.get(family)
  let targets, unknown = []
  if (req.targets?.length) {
    ({ used: targets, unknown } = resolveTargets(req.targets, names))
    if (!targets.length) {
      return { skipped: `none of ${req.targets.join(', ')} makes ${family} parts in the ` +
        `catalogue`, unknownTargets: unknown }
    }
  } else {
    // PartPanel's default question: anyone but the original's own manufacturer.
    targets = names.filter((m) => m !== row.manufacturer)
    if (!targets.length) return { skipped: `the ${family} catalogue has no vendor other than ${row.manufacturer}` }
  }
  const x = await P.crossReference({ family, original: row, manufacturers: targets,
                                     sameType: req.sameType !== false,
                                     maxResults: Number(req.maxResults) })
  return {
    targets, targetsFromCatalogue: !req.targets?.length, unknownTargets: unknown,
    category: x.fam.category, caveat: x.fam.caveat ?? null,
    origSpec: x.origSpec, origVerified: x.origVerified, missing: x.missing,
    missingKeys: originalMissingKeys(x.fam, x.origSpec),
    poolTotal: x.poolTotal, poolScored: x.poolScored,
    // the same projection Kelvin's MCP hands back: the ranker's verdict plus the spec
    // vocabulary it compared, so a candidate and the original read in one vocabulary
    ranked: x.ranked.map((c) => {
      const r = x.rowByKey.get(c._key)
      const { _key: _k, ...specs } = r ? x.fam.spec(r) : {}
      const { _key, ...verdict } = c
      return { ...verdict, manufacturer: verdict.manufacturer ?? r?.manufacturer ?? null, specs }
    }),
  }
}

async function crossrefBoard(req) {
  const components = req.components ?? []
  const byRef = new Map()
  for (const p of req.pads ?? []) {
    if (!byRef.has(p.component)) byRef.set(p.component, [])
    byRef.get(p.component).push(p)
  }
  const listed = Number(req.listed)
  const diagnostics = []
  const seen = new Map()
  for (const c of components) seen.set(c.ref, (seen.get(c.ref) ?? 0) + 1)
  for (const [ref, n] of seen) {
    if (n > 1) diagnostics.push(`${ref} appears ${n} times on the board; the sweep answers ` +
      `per reference designator, so those placements share one answer`)
  }

  // App.vue's boardParts
  const parts = components.filter((c) => P.isPart(c, byRef.get(c.ref)))

  // App.vue's load-time sweep. A family that will not load is not "no such part" — the web
  // app says so in its note, and here it is an error: the alternative is a board on which
  // every capacitor reads as absent from the catalogue because one download failed.
  const shardErrors = [], partErrors = new Map()
  const sweep = await P.sweepBoard(parts, {
    onProgress: (p) => {
      if (p.phase === 'shardError') shardErrors.push(`${p.family}: ${p.message}`)
      if (p.phase === 'partError') partErrors.set(p.ref, p.message)
    },
  })
  if (shardErrors.length) {
    throw new Error(`the catalogue could not be opened, so its answers would be incomplete: ` +
      shardErrors.join('; '))
  }

  const mfrCache = new Map()
  const lines = []
  for (const c of components) {
    const pads = byRef.get(c.ref)
    const line = {
      ref: c.ref, value: c.value || null, footprint: c.footprint || null,
      partNumber: c.partNumber || null,
      package: P.packageOf(c.footprint)?.code ?? null,
      tried: [], families: [],
    }
    lines.push(line)
    if (!P.isPart(c, pads)) {
      line.match = 'not-a-part'
      line.why = pads?.length
        ? 'its value, part number or footprint names a mounting hole, fiducial, test point, ' +
          'logo, jumper or similar — nothing a catalogue sells'
        : 'it has no pads on the board'
      continue
    }
    const cands = P.mpnCandidates(c)
    const value = P.valueOf(c)
    const fams = P.familyOrder(c)
    line.tried = cands
    line.families = fams
    if (value) line.parsedValue = { kind: value.kind, si: value.si, unit: value.unit,
                                    ratedV: value.ratedV }
    if (partErrors.has(c.ref)) line.lookupError = partErrors.get(c.ref)

    const st = sweep.get(c.ref)
    if (st?.state === 'unlookupable') {
      line.match = 'unlookupable'
      line.why = st.why
      continue
    }
    let exact = st?.state === 'exact' ? st.hit : null
    let outside = false
    let near = []
    if (!exact && cands.length) {
      // PartPanel.start(): the likeliest families first, then — a miss there not being an
      // answer — every family the first pass did not reach (searchAllFamilies).
      const first = fams.slice(0, 3)
      const r1 = first.length ? await P.identify(c, first)
        : { tried: cands, searched: [], exact: null, near: [] }
      near = r1.near
      exact = r1.exact
      if (!exact) {
        const rest = P.ALL_FAMILIES.filter((f) => !r1.searched.includes(f))
        if (rest.length) {
          const r2 = await P.identify(c, rest)
          near = [...near, ...r2.near]
          if (r2.exact) { exact = r2.exact; outside = true }
        }
      }
    }
    if (exact) {
      line.match = 'exact'
      line.family = exact.family
      line.query = exact.query
      line.outsideSuggestedFamilies = outside
      line.original = brief(exact.row)
      try {
        line.xref = await crossrefPart(exact.family, exact.row, req, mfrCache)
      } catch (e) {
        line.xrefError = String(e.message || e)
      }
      continue
    }
    if (near.length) {
      line.match = 'substring'
      line.near = near.slice(0, listed).map((h) => ({ family: h.family, query: h.query,
                                                     row: brief(h.row) }))
      line.nearTotal = near.length
    }
    if (value) {
      // PartPanel passes the land, so a part whose case code says nothing is still checked
      // against the room the board gives it.
      const v = await P.candidatesByValue(value, P.packageOf(c.footprint), { land: pads })
      if (v) {
        line.byValue = {
          family: v.family, field: v.field, tol: v.tol, total: v.total, scanned: v.scanned,
          matched: v.rows.length, unknownCase: v.unknownCase.length, differs: v.differs.length,
          bySize: v.bySize, byCase: v.byCase, byLand: v.byLand,
          rows: v.rows.slice(0, listed).map(brief),
        }
        if (!line.match && v.rows.length) line.match = 'value-package'
      }
    }
    if (!line.match) {
      line.match = 'none'
      line.why = cands.length
        ? `no catalogue part carries ${cands.map((q) => `'${q}'`).join(' or ')}, in any family`
          + (value ? ', and none of its value fits this footprint' : '')
        : 'no catalogue part of this value fits this footprint'
    }
  }
  return { lines, diagnostics, parts: parts.length }
}

// ── the loop ────────────────────────────────────────────────────────────────
const reply = (o) => process.stdout.write(JSON.stringify(o) + '\n')

const files = [...loadedFiles].sort()
const h = createHash('sha256')
for (const f of files) h.update(readFileSync(f))
const fingerprint = h.digest('hex').slice(0, 16)

process.stderr.write(`faraday crossref worker ready (shards ${SHARDS}, source ${fingerprint})\n`)
reply({ ready: true, fingerprint, files })

const rl = createInterface({ input: process.stdin })
for await (const line of rl) {
  if (!line.trim()) continue
  let req
  try {
    req = JSON.parse(line)
  } catch (e) {
    reply({ id: null, ok: false, error: `bad request json: ${e.message}` })
    continue
  }
  try {
    if (req.op !== 'crossref_board') throw new Error(`unknown op '${req.op}'`)
    if (!(Number(req.maxResults) > 0) || !(Number(req.listed) > 0)) {
      throw new Error('crossref_board needs positive maxResults and listed')
    }
    reply({ id: req.id ?? null, ok: true, result: await crossrefBoard(req) })
  } catch (e) {
    reply({ id: req.id ?? null, ok: false, error: String(e.message || e) })
  }
}
