// Kelvin's browser engine (@kelvin/engine.js), for Node — what the crossref worker loads IN
// PLACE of that module, so the web app's own parts.js and Kelvin's own crossref.js run
// unchanged.
//
// The browser engine runs kelvin.js in a Web Worker and fetches shards and records over HTTP.
// Node has neither the worker nor the site, so this module answers the same calls the same
// way from disk: the same kelvin.js WASM, the shard bytes read instead of fetched, a record
// read by its byte span instead of a Range request. It is wired exactly like Kelvin's own
// mcp/xref.mjs — and keeps the one guard the browser engine has that Kelvin's worker does
// not: a shard is checked against manifest.json's buildId, because a shard from another
// build carries record offsets into a different NDJSON and fails far away when it does.
//
// Only the functions parts.js and crossref.js import exist here. Anything else the browser
// engine exports is not reachable from them, and a call to it fails at import rather than
// being stubbed.

import { readFileSync, existsSync, openSync, readSync, closeSync, fstatSync } from 'node:fs'
import { join } from 'node:path'
import { pathToFileURL } from 'node:url'

let M = null
let SHARDS = null
let MANIFEST = null

// Called once by the worker before anything imports parts.js.
export async function configure({ shards, wasm }) {
  const manifestPath = join(shards, 'manifest.json')
  if (!existsSync(manifestPath)) {
    throw new Error(`no manifest.json in ${shards} — a shard directory is the deploy triple ` +
      `(manifest.json + <family>.kidx + <family>.ndjson); point KELVIN_SHARD_DIR at one`)
  }
  MANIFEST = JSON.parse(readFileSync(manifestPath, 'utf8'))
  if (!MANIFEST?.families) throw new Error(`${manifestPath} is malformed (no families)`)
  if (!existsSync(wasm)) throw new Error(`Kelvin's engine is not at ${wasm}`)
  const mod = await import(pathToFileURL(wasm).href)
  M = await mod.default()
  SHARDS = shards
}

function engine() {
  if (!M) throw new Error('kelvin-node: configure() was never called')
  return M
}

function callJson(fn, ...args) {
  const out = engine()[fn](...args)
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(out.slice('Exception: '.length))
  }
  return JSON.parse(out)
}

function entry(family) {
  const e = MANIFEST?.families?.[family]
  if (!e) throw new Error(`Kelvin manifest has no entry for '${family}'`)
  return e
}

const loaded = new Map()   // family -> shard meta

export async function ensureShard(family) {
  if (loaded.has(family)) return loaded.get(family)
  const e = entry(family)
  const p = join(SHARDS, e.shard || `${family}.kidx`)
  if (!existsSync(p)) throw new Error(`no shard for '${family}' at ${p}`)
  const meta = callJson('load_shard', family, readFileSync(p))
  if (String(meta.buildId) !== String(e.buildId)) {
    throw new Error(`${family}: shard is build ${meta.buildId} but the manifest asks for ` +
      `${e.buildId} — its record offsets belong to a different catalogue`)
  }
  loaded.set(family, meta)
  return meta
}

export async function browse(family, query = {}) {
  await ensureShard(family)
  return callJson('browse', family, JSON.stringify(query))
}

export async function crossReference(category, original, candidates, options = {}) {
  const out = callJson('cross_reference_string', category, JSON.stringify(original),
    JSON.stringify(candidates), JSON.stringify(options))
  if (out?.error) throw new Error(`cross-reference failed: ${out.error}`)
  return out
}

export async function fetchRecord(family, srcOffset, srcLength) {
  if (typeof srcOffset !== 'number' || typeof srcLength !== 'number') {
    throw new Error('record fetch needs a byte span (srcOffset/srcLength)')
  }
  const e = entry(family)
  const p = join(SHARDS, e.ndjson || `${family}.ndjson`)
  if (!existsSync(p)) throw new Error(`no ${family}.ndjson beside the shard at ${p}`)
  const fd = openSync(p, 'r')
  try {
    // The same version guard the browser applies to a Range response.
    const size = fstatSync(fd).size
    if (size !== e.sourceSize) {
      throw new Error(`${family}: shard/catalog version mismatch (NDJSON ${size} B vs ` +
        `shard-indexed ${e.sourceSize} B)`)
    }
    const buf = Buffer.alloc(srcLength)
    readSync(fd, buf, 0, srcLength, srcOffset)
    return JSON.parse(buf.toString('utf8'))
  } finally {
    closeSync(fd)
  }
}
