<script setup>
// The part inspector: what a PCB explorer shows when you click a component —
// reference, value, footprint, pins and nets, and the review's findings on
// those nets — and then what only this one can: the part's catalogue record
// and datasheet from Kelvin, and ranked cross-references from Kelvin's own
// ranker. Everything catalogue-side is Kelvin's code (see parts.js); this
// panel decides what to ask and says how sure the answer is.
import { ref, computed, watch, inject } from 'vue'
import { shardEvents } from '@kelvin/engine.js'
import { familyNode, extractCurves, specRows, unitFor } from '@kelvin/curves.js'
import { familyByKey } from '@kelvin/families.js'
import { famFor, keyOf } from '@kelvin/crossref.js'
import { si } from '@kelvin/units.js'
import CurveChart from '@kelvin/components/CurveChart.vue'
import { ALL_FAMILIES, familyOrder, mpnCandidates, valueOf, packageOf, footprintName,
         identify, searchMpn, candidatesByValue, manufacturersOf, crossReference,
         recordOf, valueStringFor, packageMatches, rowSize, sourcePart } from '../parts.js'

const props = defineProps({
  report: { type: Object, required: true },
  refdes: { type: String, required: true },
  findings: { type: Array, required: true },
})
const emit = defineEmits(['close', 'adopt', 'goto'])
const basic = inject('basic', ref(true))

// ---- the board's half ------------------------------------------------------
const board = computed(() => props.report.board)
const comp = computed(() => board.value.components.find(c => c.ref === props.refdes) ?? null)
const pads = computed(() => board.value.pads.filter(p => p.component === props.refdes))
const netName = id => board.value.nets.find(n => n.id === id)?.name || (id >= 0 ? `net ${id}` : '—')
const pins = computed(() => pads.value.map(p => ({
  pin: p.pin || '?', net: p.net >= 0 ? netName(p.net) : 'unconnected', netId: p.net,
  where: p.th ? 'through-hole' : (board.value.copperNames[p.cu] ?? `layer ${p.cu}`),
})))
const side = computed(() => {
  const last = board.value.copperNames.length - 1
  let top = 0, bottom = 0, th = 0
  for (const p of pads.value) { if (p.th) th++; else if (p.cu === 0) top++; else if (p.cu === last) bottom++ }
  return th && !top && !bottom ? 'through-hole' : bottom > top ? 'bottom side' : 'top side'
})
const nets = computed(() => new Set(pads.value.map(p => p.net).filter(n => n >= 0)))
const related = computed(() => {
  const re = new RegExp(`\\b${props.refdes.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`)
  return props.findings.filter(f =>
    nets.value.has(f.netA) || nets.value.has(f.netB) ||
    re.test(f.title || '') || re.test(f.detail || '')).slice(0, 12)
})

// ---- the catalogue's half --------------------------------------------------
const value = computed(() => comp.value ? valueOf(comp.value) : null)
const pkg = computed(() => comp.value ? packageOf(comp.value.footprint) : null)
const families = computed(() => comp.value ? familyOrder(comp.value) : [])
const candidates = computed(() => comp.value ? mpnCandidates(comp.value) : [])

const status = ref('')          // one line: what the panel is doing right now
const error = ref('')
const busy = ref(false)
const ident = ref(null)         // identify() result
const valueHits = ref(null)     // candidatesByValue() result
const original = ref(null)      // { family, row } — the part the record and cross-ref are about
const viewing = ref(null)       // { family, row } whose record is on screen (original by default)
const record = ref(null)
const recordErr = ref('')
const query = ref('')
const searchHits = ref(null)
const mfrs = ref([])            // [[name, count]...]
const marked = ref(new Set())
const xref = ref(null)
const xrefErr = ref('')
const xrefBusy = ref(false)
const sameType = ref(true)
const searchedAll = ref(false)
// Sourcing: the catalogue is not the last word. Heaviside's librarian can look
// an unknown part number up at the distributor and stage it for the catalogue.
const sourcing = ref(false)
const sourced = ref(null)      // the librarian's answer, hit or honest miss
const sourceErr = ref('')

const familyLabel = k => familyByKey(k)?.label?.toLowerCase() ?? k
const onShard = e => {
  const { family, phase } = e.detail
  if (phase === 'loading') status.value = `opening the ${familyLabel(family)} catalogue — a one-time download, cached by the browser…`
  else if (phase === 'loaded' && status.value.startsWith('opening')) status.value = ''
}
shardEvents.addEventListener('shard', onShard)

const remaining = computed(() =>
  ALL_FAMILIES.filter(f => !(ident.value?.searched ?? []).includes(f)))

function reset() {
  status.value = ''; error.value = ''; busy.value = false
  ident.value = null; valueHits.value = null; original.value = null; viewing.value = null
  record.value = null; recordErr.value = ''; query.value = candidates.value[0] ?? ''
  searchHits.value = null; mfrs.value = []; marked.value = new Set()
  xref.value = null; xrefErr.value = ''; xrefBusy.value = false; searchedAll.value = false
  sourcing.value = false; sourced.value = null; sourceErr.value = ''
}

async function start() {
  reset()
  if (!comp.value) return
  busy.value = true
  try {
    if (candidates.value.length) {
      // the likeliest families only — a shard is a download, and the rest are
      // one click away with the cost stated
      const first = families.value.length ? families.value.slice(0, 3) : []
      if (first.length) {
        const r = await identify(comp.value, first)
        ident.value = r
        if (r.exact) await choose(r.exact)
      } else {
        ident.value = { tried: candidates.value, searched: [], exact: null, near: [] }
      }
    }
    if (!original.value && value.value) {
      status.value = `matching ${si(value.value.si, value.value.unit)}${pkg.value ? ' in ' + pkg.value.code : ''}…`
      valueHits.value = await candidatesByValue(value.value, pkg.value)
      status.value = ''
    }
  } catch (e) {
    error.value = String(e.message || e)
  } finally {
    busy.value = false
    if (status.value.startsWith('matching')) status.value = ''
  }
}

async function searchMore(all = false) {
  if (!comp.value) return
  busy.value = true; error.value = ''
  try {
    const fams = all ? remaining.value : remaining.value.slice(0, 3)
    const r = await identify(comp.value, fams)
    const prev = ident.value ?? { tried: r.tried, searched: [], exact: null, near: [] }
    ident.value = { tried: r.tried, searched: [...prev.searched, ...r.searched],
                    exact: r.exact ?? prev.exact, near: [...prev.near, ...r.near] }
    if (all) searchedAll.value = true
    if (r.exact) await choose(r.exact)
  } catch (e) { error.value = String(e.message || e) } finally { busy.value = false }
}

async function runSearch() {
  const q = query.value.trim()
  if (q.length < 2) return
  busy.value = true; error.value = ''; searchHits.value = null
  try {
    const fams = families.value.length ? families.value.slice(0, 3) : ALL_FAMILIES.slice(0, 4)
    searchHits.value = await searchMpn(q, fams)
  } catch (e) { error.value = String(e.message || e) } finally { busy.value = false }
}

async function choose(hit) {
  original.value = hit
  viewing.value = hit
  xref.value = null; xrefErr.value = ''
  await Promise.all([loadRecord(hit), loadMfrs(hit)])
  await runXref()
}

async function loadRecord(hit) {
  record.value = null; recordErr.value = ''
  try { record.value = await recordOf(hit.family, hit.row) }
  catch (e) { recordErr.value = String(e.message || e) }
}

async function loadMfrs(hit) {
  try {
    mfrs.value = await manufacturersOf(hit.family)
    // the default question: anyone but the original's own manufacturer
    marked.value = new Set(mfrs.value.map(([m]) => m).filter(m => m !== hit.row.manufacturer))
  } catch (e) { xrefErr.value = String(e.message || e) }
}

function toggleMfr(m) {
  const s = new Set(marked.value)
  s.has(m) ? s.delete(m) : s.add(m)
  marked.value = s
}
function markAllOthers() {
  marked.value = new Set(mfrs.value.map(([m]) => m).filter(m => m !== original.value?.row.manufacturer))
}

async function runXref() {
  if (!original.value || !marked.value.size) return
  xrefBusy.value = true; xrefErr.value = ''; xref.value = null
  try {
    xref.value = await crossReference({
      family: original.value.family, original: original.value.row,
      manufacturers: [...marked.value], sameType: sameType.value, maxResults: 12 })
  } catch (e) { xrefErr.value = String(e.message || e) } finally { xrefBusy.value = false }
}

function view(hit) { viewing.value = hit; loadRecord(hit) }

// ---- sourcing an unknown part through Heaviside -----------------------------
// Only the part NUMBER leaves the browser. It is a public string printed on
// the component; the layout stays here, as it does for everything else.
async function askLibrarian(mpn) {
  sourcing.value = true; sourceErr.value = ''; sourced.value = null
  try {
    // The board's family guess travels as a HINT and the librarian may
    // overrule it from the distributor's own taxonomy — which is right: a
    // refdes and a footprint are weaker evidence than a product family.
    const hint = SOURCE_HINT[families.value[0]] ?? null
    const out = await sourcePart(mpn, hint)
    sourced.value = out
    if (out.found && out.component) {
      // What comes back is the catalogue's own envelope, so the record pane
      // renders it with the code it already has. It is NOT in the catalogue
      // yet, so it never becomes the cross-reference's original: ranking
      // against a part the ranker cannot see would be a different answer
      // from the one Kelvin gives.
      record.value = out.component
      recordErr.value = ''
    }
  } catch (e) {
    sourceErr.value = String(e.message || e)
  } finally {
    sourcing.value = false
  }
}

// Faraday's family keys -> the librarian's category vocabulary. Only the ones
// that mean the same thing on both sides; anything else travels as no hint at
// all rather than as a wrong one.
const SOURCE_HINT = {
  mosfet: 'mosfet', diode: 'diode', capacitor: 'capacitor', resistor: 'resistor',
  igbt: 'igbt', magnetic: 'magnetic', connector: 'connector', timing: 'timeBase',
}

// ---- record rendering (the same reading Kelvin's drawer gives) -------------
const info = computed(() => familyNode(record.value)?.manufacturerInfo ?? null)
const curves = computed(() => record.value ? extractCurves(record.value) : [])
const specGroups = computed(() => {
  const g = new Map()
  for (const row of (record.value ? specRows(record.value) : [])) {
    if (!g.has(row.group)) g.set(row.group, [])
    g.get(row.group).push(row)
  }
  return [...g.entries()]
})
const headline = computed(() => {
  const v = viewing.value
  const fam = v ? familyByKey(v.family) : null
  if (!v || !fam) return []
  return fam.columns.filter(c => v.row[c.f] != null).map(c => ({
    label: c.label,
    value: c.str || c.bool ? String(v.row[c.f]) : si(v.row[c.f] * (c.scale ?? 1), c.unit),
  }))
})
function fmtSpec(row) {
  if (typeof row.value === 'number') {
    const ok = Math.abs(row.value) >= 1e-15 && Math.abs(row.value) < 1e15
    const pretty = ok ? si(row.value, unitFor(row.key), 4) : String(row.value)
    return row.dim ? `${pretty} (nom)` : pretty
  }
  return String(row.value)
}

// ---- cross-ref rendering -----------------------------------------------------
const xfam = computed(() => original.value ? famFor(original.value.family, original.value.row) : null)
const rowOf = c => xref.value?.rowByKey.get(c._key ?? c.mpn)
const verdictOf = (c, key) => c.params?.find(p => p.name === key)?.verdict ?? 'unverified'
function cell(r, p) {
  const v = r?.[p.row]
  if (p.str) return v || '—'
  return si(v != null && p.scale ? v * p.scale : v, p.unit)
}
const GRADE = { drop_in: 'drop-in', minor_review: 'minor review', major_review: 'major review',
                redesign: 'redesign', no_substitute: 'no substitute' }
const GRADE_TITLE = {
  drop_in: 'fits the original’s footprint with no parameter regressions',
  minor_review: 'fits, but with warnings worth checking',
  major_review: 'fits, but a parameter regressed materially — re-qualify before building',
  redesign: 'does not fit the original’s board space, or mount/family/process differs',
  no_substitute: 'a hard gate failed — this is not a substitute',
}
const DIR = { upgrade: '▲ upgrade', downgrade: '▼ downgrade', mixed: '◆ mixed', unknown: '? unknown' }
const FIT = { same: 'fits', smaller: 'smaller', larger: 'larger', different_land_pattern: 'other pattern',
              unknown: '—' }
const mfrTop = computed(() => mfrs.value.slice(0, 18))

const adoptable = computed(() => {
  if (!comp.value || comp.value.value || !original.value) return null
  return valueStringFor(original.value.family, original.value.row)
})

const pkgOf = r => packageMatches(pkg.value, r).verdict
const pkgBasis = r => packageMatches(pkg.value, r).basis

watch(() => props.refdes, start, { immediate: true })
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="part" data-testid="part-panel" role="dialog" :aria-label="`Part ${refdes}`">
      <header class="phead">
        <h2>{{ refdes }}</h2>
        <span class="val" data-testid="part-value">{{ comp?.value || 'no value in the export' }}</span>
        <span class="fp">{{ footprintName(comp?.footprint) || 'no footprint name' }} · {{ side }} ·
          {{ pads.length }} pin(s)</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close part">✕</button>
      </header>

      <div class="body">
        <!-- ================= the board ================= -->
        <div class="col">
          <h3>On the board</h3>
          <table class="pins" data-testid="part-pins">
            <thead><tr><th>pin</th><th>net</th><th>where</th></tr></thead>
            <tbody>
              <tr v-for="(p, i) in pins.slice(0, 64)" :key="i">
                <td class="mono">{{ p.pin }}</td>
                <td class="mono" :class="{ nc: p.netId < 0 }">{{ p.net }}</td>
                <td class="dim">{{ p.where }}</td>
              </tr>
            </tbody>
          </table>
          <p v-if="pins.length > 64" class="dim">… and {{ pins.length - 64 }} more pins</p>
          <p class="dim mono">at {{ comp?.x.toFixed(2) }}, {{ comp?.y.toFixed(2) }} mm · rot {{ comp?.rot }}°</p>

          <h3>Findings on its nets</h3>
          <ul v-if="related.length" class="rel" data-testid="part-findings">
            <li v-for="f in related" :key="f.id">
              <button class="lnk" @click="emit('goto', f.id)">
                <b :class="f.severityLabel">{{ f.severityLabel }}</b> {{ f.title }}
              </button>
            </li>
          </ul>
          <p v-else class="dim">none — no finding of this review touches a net this part is on.</p>
        </div>

        <!-- ================= the catalogue ================= -->
        <div class="col wide">
          <h3>In the catalogue <span class="dim">— Kelvin, the OpenConverters parts librarian</span></h3>
          <p v-if="status" class="status" data-testid="part-status">{{ status }}</p>
          <p v-if="error" class="err" data-testid="part-error">{{ error }}</p>

          <!-- how the part was, or was not, identified -->
          <div class="ident" data-testid="part-ident">
            <template v-if="original">
              <p><b class="mono">{{ original.row.mpn }}</b> · {{ original.row.manufacturer }} ·
                {{ familyLabel(original.family) }}
                <span v-if="ident?.exact && ident.exact.row === original.row" class="tag ok">exact part-number match</span>
                <span v-else class="tag">chosen from the matches below</span>
              </p>
            </template>
            <template v-else-if="ident">
              <p v-if="ident.tried.length">
                <span v-if="ident.searched.length">
                  <b class="mono">{{ ident.tried.join(' / ') }}</b> is not in the
                  {{ ident.searched.map(familyLabel).join(', ') }} catalogue{{ ident.searched.length > 1 ? 's' : '' }}
                  as an exact part number.</span>
                <span v-else>the board gives no hint which catalogue family
                  <b class="mono">{{ ident.tried.join(' / ') }}</b> belongs to.</span>
              </p>
              <p v-if="ident.near.length" class="dim">{{ ident.near.length }} part number(s) contain it:</p>
              <ul v-if="ident.near.length" class="hits">
                <li v-for="h in ident.near.slice(0, 12)" :key="h.family + keyOf(h.row)">
                  <button class="lnk mono" @click="choose(h)">{{ h.row.mpn }}</button>
                  <span class="dim"> {{ h.row.manufacturer }} · {{ familyLabel(h.family) }}</span>
                </li>
              </ul>
              <p v-if="remaining.length && !searchedAll" class="more">
                <button class="chip" data-testid="part-search-more" :disabled="busy"
                        @click="searchMore(true)">
                  search the other {{ remaining.length }} families</button>
                <span class="dim"> — each is a catalogue download the browser keeps</span>
              </p>
            </template>
            <p v-else-if="!value && !busy" class="dim">
              no part number and no value on this component — nothing to look up.
              Type one below if you know it.</p>
          </div>

          <!-- no part number: the value and package, matched -->
          <div v-if="valueHits && !original" class="byvalue" data-testid="part-by-value">
            <p>
              <b>{{ valueHits.rows.length }}</b> catalogue part(s) match
              <b class="mono">{{ si(value.si, value.unit) }}</b>
              <template v-if="value.ratedV"> ≥ {{ value.ratedV }} V</template>
              <template v-if="pkg"> in <b class="mono">{{ pkg.code }}</b></template>
              <span class="dim"> (±{{ (valueHits.tol * 100).toFixed(1) }}% of the value;
                {{ valueHits.scanned }} of {{ valueHits.total }} value matches scanned<template
                v-if="valueHits.bySize">, {{ valueHits.bySize }} checked against the part's own
                measured body and {{ valueHits.byCase }} against its case code</template><template
                v-if="valueHits.unknownCase.length">, {{ valueHits.unknownCase.length }} state
                neither a size nor a case code and are not shown</template><template
                v-if="valueHits.differs.length">, {{ valueHits.differs.length }} are a different
                size</template>).</span>
              Pick one to read its record and cross-reference from it — the board does not say which part it is.
            </p>
            <ul class="hits">
              <li v-for="r in valueHits.rows.slice(0, 25)" :key="keyOf(r)">
                <button class="lnk mono" @click="choose({ family: valueHits.family, row: r })">{{ r.mpn }}</button>
                <span class="dim"> {{ r.manufacturer }}<template v-if="r.v_rated"> · {{ si(r.v_rated, 'V') }}</template><template
                  v-if="r.technology"> · {{ r.technology }}</template><template
                  v-if="r.dielectric_code"> · {{ r.dielectric_code }}</template><template
                  v-if="r.tolerance"> · ±{{ (r.tolerance * 100).toFixed(1) }}%</template><template
                  v-if="r.power_rating"> · {{ si(r.power_rating, 'W') }}</template></span>
              </li>
            </ul>
            <p v-if="valueHits.rows.length > 25" class="dim">
              first 25 of {{ valueHits.rows.length }} shown — narrow it with the part-number search below</p>
            <p v-if="valueHits.unknownCase.length" class="dim">
              {{ valueHits.unknownCase.length }} value match(es) publish neither a body size nor a case
              code, so nothing could be checked against the footprint:
              <button class="lnk" @click="valueHits.rows = valueHits.unknownCase">show them anyway</button>
            </p>
          </div>

          <!-- the manual way in -->
          <form class="search" @submit.prevent="runSearch">
            <input v-model="query" data-testid="part-query" spellcheck="false"
                   placeholder="look up a part number…" />
            <button class="chip" type="submit" :disabled="busy || query.trim().length < 2">search</button>
          </form>
          <ul v-if="searchHits" class="hits" data-testid="part-search-hits">
            <li v-if="!searchHits.length" class="dim">no part number contains “{{ query }}” in the
              {{ (families.length ? families.slice(0, 3) : ALL_FAMILIES.slice(0, 4)).map(familyLabel).join(', ') }} catalogues</li>
            <li v-for="h in searchHits.slice(0, 15)" :key="h.family + keyOf(h.row)">
              <button class="lnk mono" @click="choose(h)">{{ h.row.mpn }}</button>
              <span class="dim"> {{ h.row.manufacturer }} · {{ familyLabel(h.family) }}</span>
            </li>
          </ul>

          <!-- ================= sourcing an unknown part ================= -->
          <div v-if="!original && candidates.length" class="source" data-testid="part-source">
            <p v-if="!sourced">
              <button class="chip real" data-testid="part-source-btn" :disabled="sourcing"
                      @click="askLibrarian(candidates[0])">
                {{ sourcing ? 'asking the librarian…' : `look ${candidates[0]} up at the distributor` }}</button>
              <span class="dim"> — Heaviside's librarian searches the distributor, converts the
                result to a catalogue record, schema-validates it, and stages it for the
                catalogue. Only the part number is sent.</span>
            </p>
            <p v-if="sourceErr" class="err" data-testid="part-source-error">{{ sourceErr }}</p>
            <template v-if="sourced">
              <p v-if="sourced.found" data-testid="part-sourced">
                <b class="mono">{{ sourced.mpn }}</b> found at the distributor and read as a
                <b>{{ sourced.category }}</b> record.
                <span class="tag ok">schema-valid</span>
                <span class="dim">{{ sourced.storedReason }}</span>
                It is <b>not in the catalogue yet</b>, so it cannot be cross-referenced here —
                that needs the index rebuilt.
              </p>
              <p v-else data-testid="part-sourced-miss">
                The librarian could not source <b class="mono">{{ sourced.mpn }}</b>:
                {{ sourced.reason }}
              </p>
            </template>
          </div>

          <!-- ================= the record ================= -->
          <div v-if="sourced?.found && !original" class="record" data-testid="part-record">
            <div class="rhead">
              <h4 class="mono">{{ sourced.mpn }}</h4>
              <span class="dim">{{ info?.name ?? 'sourced from the distributor' }}</span>
              <span v-if="info?.status" class="tag" :class="{ ok: info.status === 'production' }">{{ info.status }}</span>
              <div class="sp" />
              <a v-if="info?.datasheetUrl" :href="info.datasheetUrl" target="_blank" rel="noopener"
                 class="chip ds" data-testid="part-datasheet">manufacturer datasheet ↗</a>
            </div>
            <div v-if="curves.length" class="curves">
              <CurveChart v-for="c in curves" :key="c.key + c.title" :curve="c" :height="180" />
            </div>
            <details class="specs" open>
              <summary>spec sheet · {{ specGroups.reduce((n, [, r]) => n + r.length, 0) }} entries</summary>
              <section v-for="[group, rows] in specGroups" :key="group">
                <p class="glabel">{{ group }}</p>
                <table class="spec"><tbody>
                  <tr v-for="row in rows" :key="group + row.key">
                    <td class="dim">{{ row.key }}</td><td class="mono r">{{ fmtSpec(row) }}</td>
                  </tr>
                </tbody></table>
              </section>
            </details>
          </div>

          <div v-if="viewing" class="record" data-testid="part-record">
            <div class="rhead">
              <h4 class="mono">{{ viewing.row.mpn }}</h4>
              <span class="dim">{{ viewing.row.manufacturer }}</span>
              <span v-if="info?.status" class="tag" :class="{ ok: info.status === 'production' }">{{ info.status }}</span>
              <span v-if="viewing.row.caseCode || rowSize(viewing.row)" class="tag"
                    :class="{ ok: pkgOf(viewing.row) === 'match', warn: pkgOf(viewing.row) === 'differs' }"
                    :title="pkg
                      ? `the board's footprint reads as ${pkg.code}; this was checked by ${pkgBasis(viewing.row) === 'size' ? 'the part\'s measured body, which outranks its case string' : 'the case code, the only size this record carries'}`
                      : 'no package could be read from the footprint name'">
                {{ pkgBasis(viewing.row) === 'size'
                   ? `${rowSize(viewing.row).lMm.toFixed(2)} × ${rowSize(viewing.row).wMm.toFixed(2)} mm`
                   : `case ${viewing.row.caseCode}` }}</span>
              <span v-if="viewing !== original" class="dim">
                — <button class="lnk" @click="view(original)">back to {{ original.row.mpn }}</button></span>
              <div class="sp" />
              <a v-if="info?.datasheetUrl" :href="info.datasheetUrl" target="_blank" rel="noopener"
                 class="chip ds" data-testid="part-datasheet">manufacturer datasheet ↗</a>
            </div>
            <div v-if="headline.length" class="kvs">
              <div v-for="h in headline" :key="h.label" class="kv">
                <span class="kl">{{ h.label }}</span><span class="kv-v mono">{{ h.value }}</span>
              </div>
            </div>
            <p v-if="adoptable && viewing === original" class="adopt">
              <button class="chip real" data-testid="part-adopt"
                      @click="emit('adopt', { refdes, value: adoptable })">
                use <b class="mono">{{ adoptable }}</b> as this part's value on the board</button>
              <span class="dim"> — the export carried none; the PDN, input-branch and Y-cap models need it</span>
            </p>
            <p v-if="recordErr" class="err">{{ recordErr }}</p>
            <p v-else-if="!record" class="dim">reading the record… (one Range slice of the catalogue)</p>
            <template v-else>
              <div v-if="curves.length" class="curves">
                <CurveChart v-for="c in curves" :key="c.key + c.title" :curve="c" :height="180" />
              </div>
              <details class="specs">
                <summary>spec sheet · {{ specGroups.reduce((n, [, r]) => n + r.length, 0) }} entries</summary>
                <section v-for="[group, rows] in specGroups" :key="group">
                  <p class="glabel">{{ group }}</p>
                  <table class="spec"><tbody>
                    <tr v-for="row in rows" :key="group + row.key">
                      <td class="dim">{{ row.key }}</td><td class="mono r">{{ fmtSpec(row) }}</td>
                    </tr>
                  </tbody></table>
                </section>
              </details>
            </template>
          </div>

          <!-- ================= cross-references ================= -->
          <div v-if="original" class="xref" data-testid="part-xref">
            <h3>Cross-references <span class="dim">— substitutes for {{ original.row.mpn }}, ranked by Kelvin</span></h3>
            <div class="mfrs">
              <button class="chip" :class="{ on: marked.size === mfrs.length - 1 || marked.size === mfrs.length }"
                      @click="markAllOthers(); runXref()">any other manufacturer</button>
              <button v-for="[m, n] in mfrTop" :key="m" class="chip mfr" :class="{ on: marked.has(m) }"
                      :disabled="m === original.row.manufacturer" :title="`${n} ${familyLabel(original.family)} parts`"
                      @click="toggleMfr(m); runXref()">{{ m }}</button>
              <label v-if="xfam?.sameFacet" class="same">
                <input type="checkbox" v-model="sameType" @change="runXref" />
                same {{ xfam.sameFacet.label }} only</label>
            </div>
            <p v-if="xrefBusy" class="dim" data-testid="part-xref-busy">ranking…</p>
            <p v-if="xrefErr" class="err" data-testid="part-xref-error">{{ xrefErr }}</p>
            <template v-if="xref">
              <p class="dim">
                {{ xref.poolScored }} candidate(s) scored<template v-if="xref.poolTotal > xref.poolScored">
                  of {{ xref.poolTotal }} in the window (pool capped)</template>.
                <template v-if="!xref.origVerified">The original's own
                  {{ xref.missing.join(', ') }} is not in the catalogue, so no candidate can be more than
                  “partial” — the comparison is one-sided.</template>
              </p>
              <div class="tblwrap">
                <table v-if="xref.ranked.length" class="xt" data-testid="part-xref-table">
                  <thead><tr>
                    <th>#</th><th>part</th><th>manufacturer</th><th>verdict</th>
                    <th v-for="p in xfam.params" :key="p.key" class="r">{{ p.label }}
                      <span class="orig mono">{{ cell(original.row, p) }}</span></th>
                    <th>fit</th><th v-if="!basic" class="r" title="total penalty — lower is better">penalty</th>
                  </tr></thead>
                  <tbody>
                    <template v-for="(c, i) in xref.ranked" :key="c._key ?? c.mpn">
                      <tr :class="`st-${c.status}`">
                        <td class="mono dim">{{ i + 1 }}</td>
                        <td><button class="lnk mono" @click="view({ family: original.family, row: rowOf(c) })">
                          {{ rowOf(c)?.mpn ?? c.mpn }}</button></td>
                        <td>{{ rowOf(c)?.manufacturer ?? '' }}</td>
                        <td><span class="tag" :class="`g-${c.grade}`" :title="GRADE_TITLE[c.grade] ?? ''">
                          {{ GRADE[c.grade] ?? c.status }}</span>
                          <span v-if="c.direction && c.direction !== 'equivalent'" class="dir dim">{{ DIR[c.direction] }}</span></td>
                        <td v-for="p in xfam.params" :key="p.key" class="r mono" :class="`v-${verdictOf(c, p.key)}`"
                            :title="`${p.label}: ${verdictOf(c, p.key)}`">{{ cell(rowOf(c), p) }}</td>
                        <td :class="`fp-${c.footprint ?? 'unknown'}`">{{ FIT[c.footprint] ?? '—' }}</td>
                        <td v-if="!basic" class="r mono">{{ c.status === 'no_substitute' ? '—' : c.penalty.toFixed(2) }}</td>
                      </tr>
                      <tr v-if="c.notes?.length" class="notes">
                        <td></td><td :colspan="xfam.params.length + 5" class="dim">{{ c.notes.join(' · ') }}</td>
                      </tr>
                    </template>
                  </tbody>
                </table>
                <p v-else class="dim">no candidate from the marked manufacturers falls inside the ranker's window.</p>
              </div>
              <p class="dim legend">
                <span class="v-pass">■</span> pass · <span class="v-warn">■</span> warn ·
                <span class="v-fail">■</span> fail · <span class="v-unverified">■</span> not documented.
                Physical fit, mount type, AEC-Q grade and lifecycle are checked too and appear in the row note
                when they bite. A verdict is a catalogue comparison, not a qualification — read both datasheets.
              </p>
            </template>
          </div>

          <p class="caveat dim">
            Identification is by part-number string against the catalogue; the catalogue is not the
            whole world, and a board value like “100n” names a value, not a part. The board never leaves
            this machine — the catalogue is read here, in the browser, one record at a time.
          </p>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scrim {
  position: fixed; inset: 0; z-index: 50;
  background: rgba(8, 12, 10, 0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px;
}
.part {
  width: min(1180px, 100%); max-height: 100%;
  display: flex; flex-direction: column;
  background: var(--resin); border: 1px solid var(--resin-edge);
  border-radius: 8px; overflow: auto;
  /* Kelvin's chart reads its palette from these — mapped onto Faraday's */
  --bg-deep: #101613; --grat-strong: rgba(157, 180, 173, 0.16); --ink: var(--silk);
  --ink-dim: var(--tin); --k: var(--copper); --line-soft: var(--resin-edge); --s1: #6f9fc4;
}
.phead {
  display: flex; align-items: baseline; gap: 14px; padding: 12px 18px;
  border-bottom: 1px solid var(--resin-edge); position: sticky; top: 0;
  background: var(--resin); z-index: 2;
}
.phead h2 { font-family: var(--display); font-size: 22px; letter-spacing: 0.06em; color: var(--copper); }
.val { font-family: var(--mono); font-size: 14px; color: var(--silk); }
.fp { font-family: var(--mono); font-size: 11.5px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 16px; padding: 2px 6px; }
.x:hover { color: var(--silk); }

.body { display: grid; grid-template-columns: 300px 1fr; gap: 0; min-height: 0; }
@media (max-width: 900px) { .body { grid-template-columns: 1fr; } }
.col { padding: 14px 18px; display: flex; flex-direction: column; gap: 10px; }
.col:not(.wide) { border-right: 1px solid var(--resin-edge); }
h3 { font-family: var(--display); font-size: 13px; letter-spacing: 0.1em; text-transform: uppercase; color: var(--silk); }
h4 { font-size: 15px; color: var(--silk); }
.dim { color: var(--tin); font-size: 12px; }
.mono { font-family: var(--mono); }
.r { text-align: right; }
.err { color: var(--heat-high); font-family: var(--mono); font-size: 12px; }
.status { color: var(--heat-med); font-size: 12px; }
.tag {
  font-family: var(--mono); font-size: 10.5px; padding: 1px 7px; border-radius: 999px;
  border: 1px solid var(--resin-edge); color: var(--tin); margin-left: 6px;
}
.tag.ok { border-color: var(--heat-low); color: var(--heat-low); }
.tag.warn { border-color: var(--heat-med); color: var(--heat-med); }

.pins { border-collapse: collapse; font-size: 12px; width: 100%; }
.pins th { text-align: left; color: var(--tin); font-weight: normal; font-size: 10.5px;
           letter-spacing: 0.08em; text-transform: uppercase; padding: 2px 6px; }
.pins td { padding: 2px 6px; border-top: 1px solid var(--resin-edge); }
.pins .nc { color: var(--tin); font-style: italic; }
.rel { list-style: none; display: flex; flex-direction: column; gap: 4px; }
.rel b { font-family: var(--mono); font-size: 10.5px; text-transform: uppercase; margin-right: 6px; }
.rel b.high { color: var(--heat-high); } .rel b.medium { color: var(--heat-med); }
.rel b.low { color: var(--heat-low); } .rel b.info { color: var(--tin); }
.lnk { text-align: left; color: var(--silk); padding: 0; }
.lnk:hover { color: var(--copper); text-decoration: underline; }

.chip {
  font-family: var(--mono); font-size: 11px; padding: 3px 10px; border-radius: 999px;
  border: 1px solid var(--tin); color: var(--tin); background: rgba(16, 22, 19, 0.6);
}
.chip:hover:not(:disabled) { border-color: var(--silk); color: var(--silk); }
.chip.on { border-color: var(--copper); color: var(--copper); }
.chip.real, .chip.ds { border-color: var(--copper); color: var(--copper); text-decoration: none; }
.chip:disabled { opacity: 0.4; cursor: not-allowed; }
.hits { list-style: none; display: flex; flex-direction: column; gap: 2px; font-size: 12.5px; }
.hits li { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
.search { display: flex; gap: 8px; }
.search input {
  flex: 1; max-width: 340px; font: 12.5px var(--mono); padding: 5px 9px;
  background: var(--bare-fr4); color: var(--silk); border: 1px solid var(--resin-edge); border-radius: 5px;
}

.record { border: 1px solid var(--resin-edge); border-radius: 6px; padding: 10px 12px;
          display: flex; flex-direction: column; gap: 8px; }
.rhead { display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; }
.kvs { display: flex; flex-wrap: wrap; gap: 8px; }
.kv { background: var(--bare-fr4); border: 1px solid var(--resin-edge); border-radius: 4px;
      padding: 4px 10px; display: flex; flex-direction: column; }
.kl { font-size: 9px; letter-spacing: 0.1em; text-transform: uppercase; color: var(--tin); }
.kv-v { font-size: 13px; color: var(--silk); }
.curves { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 10px; }
.specs summary { cursor: pointer; color: var(--tin); font-size: 12px; }
.glabel { font-size: 10px; letter-spacing: 0.1em; text-transform: uppercase; color: var(--tin); margin: 8px 0 2px; }
.spec { width: 100%; border-collapse: collapse; font-size: 12px; }
.spec td { padding: 2px 6px; border-top: 1px solid var(--resin-edge); }

.mfrs { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.same { font-size: 12px; color: var(--tin); display: flex; gap: 6px; align-items: center; margin-left: 6px; }
.tblwrap { overflow-x: auto; }
.xt { border-collapse: collapse; font-size: 12px; width: 100%; }
.xt th { text-align: left; color: var(--tin); font-weight: normal; font-size: 10.5px;
         letter-spacing: 0.06em; text-transform: uppercase; padding: 4px 6px; white-space: nowrap; }
.xt th.r { text-align: right; }
.xt th .orig { display: block; color: var(--copper); font-size: 11px; text-transform: none; letter-spacing: 0; }
.xt td { padding: 4px 6px; border-top: 1px solid var(--resin-edge); white-space: nowrap; }
.xt tr.notes td { border-top: none; padding-top: 0; white-space: normal; font-size: 11.5px; }
.xt tr.st-no_substitute td { opacity: 0.55; }
.g-drop_in { border-color: var(--heat-low); color: var(--heat-low); }
.g-minor_review { border-color: var(--heat-med); color: var(--heat-med); }
.g-major_review { border-color: #e07a3f; color: #e07a3f; }
.g-redesign, .g-no_substitute { border-color: var(--heat-high); color: var(--heat-high); }
.dir { font-size: 10.5px; margin-left: 4px; }
.v-pass { color: var(--heat-low); } .v-warn { color: var(--heat-med); }
.v-fail { color: var(--heat-high); } .v-unverified { color: var(--tin); }
.fp-same { color: var(--heat-low); } .fp-different_land_pattern, .fp-larger { color: var(--heat-med); }
.legend { font-size: 11px; }
.caveat { font-size: 11px; border-top: 1px solid var(--resin-edge); padding-top: 8px; }
.adopt { font-size: 12px; }
.source {
  border: 1px dashed var(--resin-edge); border-radius: 6px; padding: 10px 12px;
  display: flex; flex-direction: column; gap: 6px; font-size: 12.5px;
}
.more { font-size: 12px; }
</style>
