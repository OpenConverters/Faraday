import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import vue from '@vitejs/plugin-vue'

// The parts inspector asks Kelvin (kelvin.openconverters.com) about every
// component on the board: identify the part, read its catalogue record and
// datasheet, rank cross-references. That is Kelvin's own browser code — the
// worker-hosted WASM ranker, the family tables, the record reader — imported
// from the sibling checkout at BUILD time, never re-implemented here: a second
// copy of the cross-reference model would drift from the real one within a
// release, and a Faraday that ranks substitutes differently from Kelvin is
// worse than one that does not rank them at all.
//
// The engine (kelvin.js) and the data set (/kelvin/*: manifest, shards, the
// NDJSON the records are Range-read from) are served SAME-ORIGIN, the way
// Kirchhoff does it — the CSP allows no other host. In production nginx
// serves both off the box's shared /cache/kelvin + Kelvin's own dist (see
// ops/faraday.nginx); in development the dev server proxies them from the
// live Kelvin site, so a local board talks to the real catalogue.
const here = path.dirname(fileURLToPath(import.meta.url))
const kelvinSrc = process.env.KELVIN_WEB_SRC ||
  path.resolve(here, '../../Kelvin/web/src')
if (!existsSync(path.join(kelvinSrc, 'crossref.js'))) {
  throw new Error(
    `Kelvin's web sources not found at ${kelvinSrc} — the parts inspector ` +
    `imports them (crossref.js, engine.js, curves.js). Check out ` +
    `OpenConverters/Kelvin beside Faraday, or set KELVIN_WEB_SRC.`)
}
const kelvinOrigin = process.env.FARADAY_KELVIN_ORIGIN ||
  'https://kelvin.openconverters.com'
// The librarian: Heaviside's part-sourcing endpoint, reached same-origin under
// /heaviside/ for the same CSP reason as Kelvin. In production nginx maps that
// prefix to the librarian on the box; in development it is proxied to whatever
// Heaviside you point it at (a local `heaviside serve` on 8000, or the
// deployed one). Note this spends a real distributor API call per lookup, so
// the e2e suite stubs the route rather than pointing here.
const heavisideOrigin = process.env.FARADAY_HEAVISIDE_ORIGIN ||
  'https://heaviside.openconverters.com'

export default {
  plugins: [vue()],
  resolve: { alias: { '@kelvin': kelvinSrc } },
  server: {
    fs: { allow: [here, kelvinSrc] },
    proxy: {
      '/kelvin.js': { target: kelvinOrigin, changeOrigin: true },
      '/kelvin/': { target: kelvinOrigin, changeOrigin: true },
      '/heaviside/': {
        target: heavisideOrigin,
        changeOrigin: true,
        rewrite: (p) => p.replace(/^\/heaviside/, ''),
      },
    },
  },
}
