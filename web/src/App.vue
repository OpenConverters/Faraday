<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import BoardView from './components/BoardView.vue'
import FindingsList from './components/FindingsList.vue'
import BenchPanel from './components/BenchPanel.vue'
import EmissionsPanel from './components/EmissionsPanel.vue'
import NearFieldPanel from './components/NearFieldPanel.vue'
import PdnPanel from './components/PdnPanel.vue'
import ImpedancePanel from './components/ImpedancePanel.vue'
import GlossaryPanel from './components/GlossaryPanel.vue'
import StackupPanel from './components/StackupPanel.vue'
import { unzip } from './zip.js'

const engine = ref(null)
const boardText = ref('')
// A Gerber board is a SET of files ({name, text}); non-empty means the set
// path (analyzeSet) instead of the single-file one.
const boardFiles = ref([])
// when the engine names the copper count ("choose default-6layer"), the
// stackup card offers exactly that button
const stackupSuggest = ref('')
const fileName = ref('')
const report = ref(null)
const error = ref('')
const needStackup = ref(false)
const stackupChoice = ref('')   // '' = from board file
const selectedId = ref('')
const dragOver = ref(false)

// The engine is a 650 kB WASM download and compile. On a fast local connection
// it is ready before anyone can pick a file; on a real one it is not, and a
// board dropped in that window used to hit `if (!engine) return` and vanish —
// no analysis, no error, no retry, forever. Holding the promise and awaiting it
// means a board is never silently dropped, whichever wins the race.
const enginePromise = window.createFaraday()
const engineLoading = ref(true)
// #load=<url>: fetch a board by URL. This is how the KiCad plugin hands the
// open board across (it serves the file from localhost with CORS), and how
// the demo button works — the fetch happens in the browser, so the privacy
// property is unchanged: nothing is uploaded anywhere.
async function loadFromHash() {
  const m = location.hash.match(/^#load=(.+)$/)
  if (!m) return
  const url = decodeURIComponent(m[1])
  try {
    const res = await fetch(url)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    const txt = await res.text()
    fileName.value = url.split('/').pop() || 'board'
    boardText.value = txt
    boardFiles.value = []
    stackupChoice.value = ''
    customStackup.value = null
    await analyze()
  } catch (e) {
    error.value = `could not load ${url}: ${e.message || e}`
  }
}

onMounted(async () => {
  try {
    engine.value = await enginePromise
  } catch (e) {
    error.value = 'the analysis engine failed to load: ' + String(e)
  } finally {
    engineLoading.value = false
  }
  await loadFromHash()
  window.addEventListener('hashchange', loadFromHash)
})

async function analyze() {
  if (!boardText.value && !boardFiles.value.length) return
  if (!engine.value) {
    // waiting on the engine is a state, not a failure — say so and continue
    engineLoading.value = true
    try { engine.value = await enginePromise } catch (e) {
      error.value = 'the analysis engine failed to load: ' + String(e)
      engineLoading.value = false
      return
    }
    engineLoading.value = false
  }
  error.value = ''
  needStackup.value = false
  const out = boardFiles.value.length
    ? JSON.parse(engine.value.analyzeSet(JSON.stringify(
        { files: boardFiles.value, stackup: stackupSpec() })))
    : JSON.parse(engine.value.analyze(boardText.value, stackupSpec()))
  if (out.error) {
    report.value = null
    if (out.error.includes('no stackup')) {
      // explicit choice required — Faraday never assumes a stackup silently
      needStackup.value = true
      stackupSuggest.value = (out.error.match(/default-\d+layer/) || [''])[0]
      hasSavedStackup.value = !!savedStackup()
    } else {
      error.value = out.error
    }
    return
  }
  report.value = out
  selectedId.value = ''
}

async function onFiles(list) {
  const picked = Array.from(list || []).filter(Boolean)
  if (!picked.length) return
  error.value = ''
  try {
    let files = []
    for (const f of picked) {
      if (/\.zip$/i.test(f.name)) {
        files.push(...await unzip(await f.arrayBuffer()))
      } else {
        files.push({ name: f.name, text: await f.text() })
      }
    }
    if (files.length === 1) {
      // the single-file formats keep their fast path
      fileName.value = files[0].name
      boardText.value = files[0].text
      boardFiles.value = []
    } else {
      fileName.value = picked.length === 1
        ? picked[0].name : `${files.length} files (Gerber set)`
      boardText.value = ''
      boardFiles.value = files
    }
  } catch (e) {
    error.value = String(e.message || e)
    return
  }
  stackupChoice.value = ''
  customStackup.value = null
  clearBaseline()
  fixResult.value = ''
  await analyze()
}

function onDrop(e) {
  dragOver.value = false
  onFiles(e.dataTransfer?.files)
}

function chooseStackup(name) {
  if (name === '__custom__') { stackupOpen.value = true; return }
  customStackup.value = null
  stackupChoice.value = name
  analyze()
}

const meta = computed(() => report.value?.meta ?? null)
const findings = computed(() => report.value?.findings ?? [])

// Rule filter: dense boards render 200 overlapping overlays, so let the reader
// mute whole rules. Muting hides them from BOTH the list and the board.
const hiddenRules = ref(new Set())
const ruleCounts = computed(() => {
  const m = new Map()
  for (const f of findings.value) m.set(f.rule, (m.get(f.rule) ?? 0) + 1)
  return [...m.entries()].sort((a, b) => b[1] - a[1])
})
const visibleFindings = computed(() =>
  findings.value.filter(f => !hiddenRules.value.has(f.rule) &&
                             !hiddenIds.value.has(f.id) &&
                             (!onlyChanges.value || diffMap.value[f.id])))
// The bench: the deep tier, opened on one finding. Only findings that carry a
// cross-section can be solved — a run with no reference plane has no
// cross-section, and the list says so rather than offering a dead button.
const benchId = ref('')
const benchFinding = computed(() =>
  findings.value.find(f => f.id === benchId.value && f.solve) ?? null)
const netName = id =>
  report.value?.board.nets.find(n => n.id === id)?.name || (id >= 0 ? `net ${id}` : '')
const layerName = cu => report.value?.board.copperNames?.[cu] ?? `layer ${cu}`

// Radiated emissions: offered on findings that carry an enclosed loop area,
// which today means commutation loops. The loop area is the input nobody else
// can supply, and it comes straight off the copper.
// The return-path layer: effective loop height per segment, geometry only.
// It replaced the "radiation attribution" once a measurement showed that
// ranking with the assumed current was 97% a restatement of the switch-node
// rule — the current is gone, and what remains is what the layout proves.
const returnPath = ref(null)
const rpError = ref('')
function runReturnPath() {
  try {
    const out = JSON.parse(engine.value.returnPath(JSON.stringify({})))
    if (out.error) { rpError.value = out.error; return }
    rpError.value = ''
    returnPath.value = out
  } catch (e) { rpError.value = String(e) }
}
function toggleReturnPath() {
  if (returnPath.value) { returnPath.value = null; return }
  runReturnPath()
}
watch(report, () => { returnPath.value = null; rpError.value = '' })

// The component near-field map: a different regime from the far-field
// attribution, so it is a separate panel with its own units and its own caveat.
const nearField = ref(null)
const nfError = ref('')
const nfDetail = ref(false)
// The excitation the overlay is computed at. Held here rather than in the
// panel because the board overlay needs it too.
const nfParams = ref({ ringCurrentA: 2, ringMhz: 130, probeHeightMm: 3, victimAreaMm2: 4 })
// Shield cans the user has drawn on the near-field layer. A can attenuates
// coupling only when it separates aggressor from victim, and the SE comes from
// its own material, wall and contact pitch at the ring frequency.
const shields = ref([])
const shieldSpec = ref({ material: 'tinsteel', wallMm: 0.2, seamPitchMm: 5, muR: 0 })
const drawingShield = ref(false)
function addShield(rect) {
  shields.value = [...shields.value, rect]
  drawingShield.value = false
  runNearField()
}
function clearShields() { shields.value = []; runNearField() }
watch(shieldSpec, () => { if (nearField.value) runNearField() }, { deep: true })
function runNearField() {
  try {
    const out = JSON.parse(engine.value.nearField(JSON.stringify({
      ...nfParams.value,
      shields: shields.value.map(s => ({ ...s, ...shieldSpec.value })),
    })))
    if (out.error) { nfError.value = out.error; nearField.value = null; return }
    nfError.value = ''
    nearField.value = out
  } catch (e) { nfError.value = String(e) }
}
function toggleNearField() {
  if (nearField.value) {
    nearField.value = null; nfError.value = ''; drawingShield.value = false
    return
  }
  runNearField()
}
function setNfParams(p) { nfParams.value = { ...nfParams.value, ...p }; runNearField() }
watch(report, () => { nearField.value = null; nfError.value = ''; nfDetail.value = false })

// The near-field map is built around switching aggressors, so on a board with
// none it has nothing to say. Better to disable the chip with the reason than
// to let it error on click.
const hasSwitchNode = computed(() => (meta.value?.switchNodes?.length ?? 0) > 0)
const ruleCountMap = computed(() => Object.fromEntries(ruleCounts.value))

const pdnOpen = ref(false)
const glossaryOpen = ref(false)
const stackupOpen = ref(false)
// A user-entered stackup (layers array). When set it wins over stackupChoice
// and is persisted per board file name, because a stackup belongs to a board.
const customStackup = ref(null)
function stackupKey() { return 'faraday-stackup:' + fileName.value }
function savedStackup() {
  try { return JSON.parse(localStorage.getItem(stackupKey()) || 'null') }
  catch { return null }
}
function applyStackup(layers) {
  customStackup.value = layers
  stackupOpen.value = false
  try { localStorage.setItem(stackupKey(), JSON.stringify(layers)) } catch {}
  analyze()
}
function useSavedStackup() {
  const l = savedStackup()
  if (l) { customStackup.value = l; analyze() }
}
const stackupSpec = () => customStackup.value
  ? JSON.stringify({ layers: customStackup.value })
  : stackupChoice.value
const hasSavedStackup = ref(false)

// ---- revision diff ("did this change make EMC worse?") ----
// baseline = an exported report JSON, or another revision's board file
// (analysed on the spot with the SAME stackup spec)
const baselineReport = ref(null)
const baselineName = ref('')
const diff = ref(null)
const onlyChanges = ref(false)
const baselineInput = ref(null)
// stitching-fix result line shown under the return-path bar
const fixResult = ref('')
function fixStitching() {
  fixResult.value = ''
  try {
    const out = JSON.parse(engine.value.fixStitching(boardText.value, stackupSpec()))
    if (out.error) { fixResult.value = out.error; return }
    if (out.text) {
      const base = fileName.value.replace(/\.kicad_pcb$/i, '')
      const blob = new Blob([out.text], { type: 'text/plain' })
      const a = document.createElement('a')
      a.href = URL.createObjectURL(blob)
      a.download = `${base}-stitched.kicad_pcb`
      a.click()
      URL.revokeObjectURL(a.href)
      fixResult.value = `${out.vias.length} stitching via(s) added — review the`
        + ` downloaded file in KiCad; your original is untouched`
    } else {
      fixResult.value = out.notes[0] || 'nothing to stitch'
    }
  } catch (e) { fixResult.value = String(e.message || e) }
}
function computeDiff() {
  diff.value = baselineReport.value && report.value
    ? JSON.parse(engine.value.diffReports(
        JSON.stringify(baselineReport.value), JSON.stringify(report.value)))
    : null
  if (diff.value?.error) { error.value = diff.value.error; diff.value = null }
}
async function onBaselineFile(file) {
  if (!file || !report.value) return
  try {
    const text = await file.text()
    if (/^\s*\{/.test(text)) {
      const j = JSON.parse(text)
      if (!j.findings || !j.meta)
        throw new Error('not a Faraday report (missing findings/meta)')
      baselineReport.value = j
    } else {
      // another revision's board: analyse it, then re-analyse the current
      // board so the engine's held board (overlays, PDN) stays the CURRENT one
      const out = JSON.parse(engine.value.analyze(text, stackupSpec()))
      if (out.error) throw new Error(out.error)
      baselineReport.value = out
      await analyze()
    }
    baselineName.value = file.name
    computeDiff()
  } catch (e) {
    error.value = 'baseline: ' + String(e.message || e)
  }
}
function clearBaseline() {
  baselineReport.value = null; baselineName.value = ''
  diff.value = null; onlyChanges.value = false
}
watch(report, computeDiff)
const diffMap = computed(() => {
  const m = {}
  if (diff.value) {
    for (const e of diff.value.added) m[e.id] = 'new'
    for (const e of diff.value.worsened) m[e.id] = 'worse'
  }
  return m
})
// Individually dismissed findings. Per board, not persisted: a dismissal is a
// review decision for THIS review.
const hiddenIds = ref(new Set())
function hideFinding(id) {
  const s2 = new Set(hiddenIds.value)
  s2.add(id)
  hiddenIds.value = s2
  if (selectedId.value === id) selectedId.value = ''
}
function unhideAll() { hiddenIds.value = new Set() }
function loadDemo() { window.location.hash = '#load=/demo.kicad_pcb' }
watch(report, () => { hiddenIds.value = new Set() })
const calcOpen = ref(false)
watch(report, () => { pdnOpen.value = false })

// Report export: a single self-contained HTML file built from the report the
// engine already produced. A design review you cannot hand to a colleague is
// half a review.
function exportReport() {
  const rep = report.value
  if (!rep) return
  const esc = t => String(t ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;')
  const rows = rep.findings.map(f => `<tr class="${esc(f.severityLabel)}">
    <td>${esc(f.id)}</td><td>${esc(f.rule)}</td><td>${esc(f.severityLabel)}</td>
    <td>${esc(f.title)}</td><td>${esc(f.detail)}<br><b>Fix:</b> ${esc(f.remediation)}</td></tr>`).join('')
  const planes = (rep.meta.planes ?? []).map(p =>
    `${esc(p.layer)}: ${p.isPlane ? 'plane' : 'signal'}`).join(' · ')
  const html = `<!doctype html><html><head><meta charset="utf-8">
<title>Faraday EMC review — ${esc(fileName.value)}</title>
<style>
 body{font:14px/1.5 system-ui;margin:2rem auto;max-width:70rem;color:#1c2422;padding:0 1rem}
 h1{font-size:1.4rem} .meta{color:#5a6d67;font-size:.85rem;margin-bottom:1.5rem}
 table{border-collapse:collapse;width:100%;font-size:.85rem}
 th,td{border:1px solid #d5ddd9;padding:.4rem .6rem;text-align:left;vertical-align:top}
 tr.high td:first-child{border-left:4px solid #d33}
 tr.medium td:first-child{border-left:4px solid #d90}
 .caveat{background:#f4f1ea;border:1px solid #d5ddd9;padding:.8rem 1rem;margin:1.2rem 0;font-size:.85rem}
</style></head><body>
<h1>Faraday EMC design review — ${esc(fileName.value)}</h1>
<p class="meta">Generated ${new Date().toISOString().slice(0, 10)} ·
 format ${esc(rep.format)} · stackup ${esc(rep.meta.stackupSource)} · ${planes} ·
 ${rep.findings.length} findings</p>
<div class="caveat"><b>Screening estimates, not compliance predictions.</b>
 Faraday ranks EMC risk from layout geometry with stated physics and stated
 error bars. Findings marked screening-estimate carry roughly ±6 dB; a clean
 report is not a chamber pass. Analysis ran locally in the browser — the
 layout never left the machine.</div>
<table><thead><tr><th>id</th><th>rule</th><th>severity</th><th>finding</th>
<th>detail</th></tr></thead><tbody>${rows}</tbody></table>
</body></html>`
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([html], { type: 'text/html' }))
  a.download = (fileName.value || 'board').replace(/\.[^.]+$/, '') + '-faraday-review.html'
  a.click()
  URL.revokeObjectURL(a.href)
}

const emitId = ref('')
const emitFinding = computed(() =>
  findings.value.find(f => f.id === emitId.value && f.emit) ?? null)

function toggleRule(rule) {
  const s = new Set(hiddenRules.value)
  s.has(rule) ? s.delete(rule) : s.add(rule)
  hiddenRules.value = s
  if (selectedId.value && !visibleFindings.value.some(f => f.id === selectedId.value))
    selectedId.value = ''
}
</script>

<template>
  <div class="shell" @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
       @drop.prevent="onDrop">
    <header class="topbar">
      <h1 class="brand">FARADAY</h1>
      <span class="tagline">EMC design review — runs in your browser, nothing is uploaded</span>
      <div class="spacer" />
      <label class="filebtn">
        <input data-testid="file-input" type="file" multiple
               accept=".kicad_pcb,.hyp,.HYP,.xml,.zip,.gbr,.gtl,.gbl,.g1,.g2,.g3,.g4,.gm1,.gko,.drl,.xln,.txt"
               @change="e => onFiles(e.target.files)" />
        {{ fileName || 'Open board file' }}
      </label>
      <button v-if="report" class="filebtn" data-testid="export-report"
              @click="exportReport">export report</button>
      <button v-if="report && !baselineReport" class="filebtn" data-testid="compare"
              @click="baselineInput.click()"
              title="compare against another revision — a board file or an exported report">
        compare rev…</button>
      <input ref="baselineInput" type="file" style="display:none"
             data-testid="baseline-input"
             accept=".json,.kicad_pcb,.hyp,.HYP,.xml"
             @change="e => { onBaselineFile(e.target.files[0]); e.target.value = '' }" />
      <select v-if="report || needStackup" data-testid="stackup-select" class="stackup"
              :value="customStackup ? '__active__' : stackupChoice"
              @change="e => chooseStackup(e.target.value)"
              aria-label="Stackup">
        <option v-if="customStackup" value="__active__">
          Stackup: custom ({{ customStackup.filter(l => l.kind === 'copper').length }} layers)</option>
        <option value="">Stackup: from board file</option>
        <option value="default-2layer">Stackup: default 2-layer FR4</option>
        <option value="default-4layer">Stackup: default 4-layer FR4</option>
        <option value="__custom__">Stackup: enter the real one…</option>
      </select>
    </header>

    <div v-if="error" class="banner error" data-testid="error-banner">{{ error }}</div>

    <div v-if="engineLoading && fileName" class="banner wait" data-testid="engine-loading">
      Loading the analysis engine…</div>
    <div v-if="rpError" class="banner error" data-testid="rp-error">{{ rpError }}</div>

    <div v-if="nfError" class="banner error" data-testid="nf-error">{{ nfError }}</div>

    <div v-if="needStackup" class="banner ask" data-testid="stackup-card">
      <p>This board file carries no stackup. Z₀ and coupling need one — choose what the
         board is built on (the report will state your choice):</p>
      <button class="chip" @click="chooseStackup('default-2layer')">Default 2-layer FR4 (1.6 mm)</button>
      <button class="chip" @click="chooseStackup('default-4layer')">Default 4-layer FR4 (1.6 mm)</button>
      <button v-if="stackupSuggest && !['default-2layer', 'default-4layer'].includes(stackupSuggest)"
              class="chip" data-testid="stackup-suggest"
              @click="chooseStackup(stackupSuggest)">
        Default {{ stackupSuggest.match(/\d+/)[0] }}-layer FR4 (this set's copper count)</button>
      <button class="chip real" data-testid="stackup-custom"
              @click="stackupOpen = true">
        Enter the real stackup — every Z₀ and coupling figure stands on it</button>
      <button v-if="hasSavedStackup" class="chip" data-testid="stackup-saved"
              @click="useSavedStackup">
        Use the stackup you saved for this board</button>
    </div>

    <main class="work" v-if="report">
      <!-- The board cell is a fixed grid cell; the return-path / near-field
           summaries float over its LEFT edge instead of sitting above the
           board, so toggling an overlay never moves or rescales the copper —
           the point of toggling is comparing, and comparing needs a still
           board. -->
      <div class="boardcell">
        <BoardView :report="report" :findings="visibleFindings" :selected-id="selectedId"
                   :return-path="returnPath" :near-field="nearField"
                   :shields="shields" :drawing-shield="drawingShield"
                   :has-switch-node="hasSwitchNode"
                   @select="id => selectedId = id"
                   @toggle-return-path="toggleReturnPath"
                   @near-field="toggleNearField"
                   @shield="addShield"
                   @pdn="pdnOpen = true" />
      <!-- while drawing a shield the bars go click-through and faded — a
           floating card must never steal the drag that draws underneath it -->
      <div class="overlaybars" :class="{ ghost: drawingShield }"
           v-if="nearField || returnPath">
      <div v-if="nearField" class="radbar nf" data-testid="nf-bar">
        <b>Near field</b>
        <span>|H| at {{ nfParams.probeHeightMm.toFixed(1) }} mm ·
          {{ nfParams.ringCurrentA.toFixed(1) }} A ring at {{ nfParams.ringMhz.toFixed(0) }} MHz ·
          λ/2π = {{ nearField.lambdaOver2PiMm.toFixed(0) }} mm, the whole board is inside it</span>
        <span v-if="nearField.victims?.length" class="top">
          worst: <b>{{ nearField.victims[0].component }}</b>
          ({{ nearField.victims[0].class }})
          {{ nearField.victims[0].ratio >= 10
             ? (20 * Math.log10(nearField.victims[0].ratio)).toFixed(0) + ' dB over'
             : (nearField.victims[0].ratio * 100).toFixed(0) + '% of' }} threshold</span>
        <span v-if="nearField.shieldedVictims" class="top" data-testid="nf-shielded">
          can separates <b>{{ nearField.shieldedVictims }}</b> victim(s) from the
          aggressor — an upper bound, the can is five-sided</span>
        <button class="detail" data-testid="nf-shield-draw"
                @click="drawingShield = !drawingShield">
          {{ drawingShield ? 'click-drag on the board…' : 'draw a shield can' }}</button>
        <button v-if="shields.length" class="detail" data-testid="nf-shield-clear"
                @click="clearShields">clear {{ shields.length }}</button>
        <label v-if="shields.length" class="spec">pitch
          <input type="range" min="1" max="40" step="0.5" data-testid="nf-shield-pitch"
                 v-model.number="shieldSpec.seamPitchMm" />
          <b>{{ shieldSpec.seamPitchMm.toFixed(1) }} mm</b></label>
        <label v-if="shields.length" class="spec">µ′
          <select v-model.number="shieldSpec.muR" data-testid="nf-can-mur"
                  title="permeability grade — µ′ from the shield material's datasheet at the ring frequency; vendors sell several">
            <option :value="0">default</option>
            <option v-for="g in [40, 60, 120, 220, 300, 1000]" :key="g" :value="g">{{ g }}</option>
          </select></label>
        <button class="detail" data-testid="nf-detail" @click="nfDetail = true">
          victims &amp; shielding →</button>
        <span class="cav"><b>What couples on the board.</b> Quasi-static induction
          at component scale, in A/m — not a radiation map, and only switching
          loops are modelled as sources. No dBµV/m, no limit line: there is no
          reliable near-field to far-field transform. The ring current is your
          assumption. The return-path layer shows where returns detour; the
          emissions panel covers what leaves the board.</span>
      </div>

      <div v-if="returnPath" class="radbar" data-testid="rp-bar">
        <b>Return path</b>
        <span>effective loop height {{ returnPath.minEffHeightMm.toFixed(2) }}–{{
          returnPath.maxEffHeightMm.toFixed(2) }} mm over {{ returnPath.counted }} segments
          — geometry only, no assumed currents</span>
        <span v-if="returnPath.overVoidCount" class="warn" data-testid="rp-void">
          {{ returnPath.overVoidCount }} segment(s) over a plane void or split</span>
        <span v-if="returnPath.layerChangeCount" class="top">
          {{ returnPath.layerChangeCount }} layer change(s),
          <b :class="returnPath.unstitchedCount ? 'bad' : ''">{{ returnPath.unstitchedCount }}
          with no stitching via in reach</b></span>
        <span class="top" v-for="w in returnPath.worst.slice(0, 3)" :key="w.net">
          {{ w.net || '(unnamed)' }} <b>{{ w.areaMm2.toFixed(0) }} mm²</b><template
          v-if="w.unstitched"> · unstitched</template><template
          v-else-if="w.overVoid"> · over void</template></span>
        <button v-if="report.format === 'kicad' && returnPath.unstitchedCount"
                class="detail" data-testid="rp-fix"
                @click="fixStitching">
          generate stitching vias → new file</button>
        <span v-if="fixResult" class="top" data-testid="rp-fix-result">{{ fixResult }}</span>
        <span class="cav"><b>How far away each trace's return current really is</b> —
          the dielectric height where the plane is solid beneath it, the detour where
          it is not, the hop at every layer change. Every number here is a geometric
          fact of the layout. For a defensible far-field figure with a limit line, use
          the emissions panel on a commutation-loop finding; for coupling into nearby
          parts, the near-field layer.</span>
      </div>
      </div>
      </div>
      <FindingsList :findings="visibleFindings" :report="report" :selected-id="selectedId"
                    :rules="ruleCounts" :hidden-rules="hiddenRules" :total="findings.length"
                    @select="id => selectedId = selectedId === id ? '' : id"
                    :hidden-count="hiddenIds.size"
                    :diff="diff" :diff-map="diffMap" :baseline-name="baselineName"
                    :only-changes="onlyChanges"
                    @toggle-changes="onlyChanges = !onlyChanges"
                    @clear-baseline="clearBaseline"
                    @toggle-rule="toggleRule"
                    @bench="id => benchId = id"
                    @emissions="id => emitId = id"
                    @hide="hideFinding"
                    @unhide-all="unhideAll"
                    @glossary="glossaryOpen = true" />
    </main>

    <div v-else-if="!needStackup" class="empty" :class="{ over: dragOver }">
      <div class="board-ghost" aria-hidden="true" />
      <p class="invite">Drop a board here</p>
      <p class="formats"><code>.kicad_pcb</code> · <code>.hyp</code> · <code>IPC-2581 .xml</code>
         · <code>Gerber X2 set</code> · <code>ODB++ job</code> (files, or one zip)</p>
      <button class="chip" data-testid="open-calc" @click="calcOpen = true">
        impedance calculator — no board needed</button>
      <button class="chip" data-testid="load-demo" @click="loadDemo">
        try the demo board</button>
      <p class="sub">Faraday screens the whole board for coupled runs, return-path breaks,
         via and open stubs, decoupling reach, and — on converters — switch nodes and
         commutation-loop area. It ranks the risk and renders it on the copper.
         Analysis runs here, in your browser — your layout never leaves this machine.</p>
    </div>

    <!-- Must stay OUTSIDE the work/empty v-if chain. Sitting between them broke
         the chain: the empty state's v-else-if bound to this element instead of
         to <main v-if="report">, so the drop zone rendered underneath a loaded
         board and split the flex height with it. -->
    <BenchPanel v-if="benchFinding && engine" :engine="engine" :finding="benchFinding"
                :title-a="netName(benchFinding.netA)" :title-b="netName(benchFinding.netB)"
                :layer="layerName(benchFinding.cuA)"
                @close="benchId = ''" />

    <EmissionsPanel v-if="emitFinding && engine" :engine="engine"
                    :finding="emitFinding" @close="emitId = ''" />

    <NearFieldPanel v-if="nfDetail && nearField && engine" :engine="engine"
                    :result="nearField" :params="nfParams"
                    @close="nfDetail = false" @params="setNfParams" />

    <PdnPanel v-if="pdnOpen && engine && report" :engine="engine"
              @close="pdnOpen = false" />

    <ImpedancePanel v-if="calcOpen && engine" :engine="engine"
                    @close="calcOpen = false" />

    <GlossaryPanel v-if="glossaryOpen" :rule-counts="ruleCountMap"
                   :hidden-rules="hiddenRules"
                   @toggle-rule="toggleRule" @close="glossaryOpen = false" />

    <StackupPanel v-if="stackupOpen" :initial="customStackup"
                  :copper-hint="Number((stackupSuggest.match(/\d+/) || [0])[0])"
                  @apply="applyStackup" @close="stackupOpen = false" />

    <footer v-if="meta" class="metastrip" data-testid="meta-strip">
      <span class="m">format: <b>{{ report.format }}</b></span>
      <span class="m">stackup: <b>{{ meta.stackupSource }}</b></span>
      <span v-for="p in meta.planes" :key="p.layer" class="m">
        {{ p.layer }} <b>{{ p.isPlane ? 'plane' : 'signal' }}</b>
        <template v-if="p.zoneCoverage > 0"> · {{ Math.round(p.zoneCoverage * 100) }}% pour</template>
      </span>
      <span v-if="meta.switchNodes?.length" class="m sw" data-testid="meta-switchnodes">
        switch nodes: <b>{{ meta.switchNodes.join(', ') }}</b></span>
      <span v-if="meta.diffPairsRecognized" class="m">{{ meta.diffPairsRecognized }} diff pair(s) recognized</span>
      <span v-if="meta.polygonOnlyNets?.length" class="m">
        {{ meta.polygonOnlyNets.length }} zone-routed net(s) judged by pour outline</span>
      <span v-if="meta.crossingCheckSkippedPlanes?.length" class="m warn">
        void check skipped on {{ meta.crossingCheckSkippedPlanes.join(', ') }} (no fill)</span>
      <span v-if="meta.approximatedArcs" class="m warn">{{ meta.approximatedArcs }} arc(s) chord-approximated</span>
      <span v-if="meta.droppedBelowFloorDb" class="m">{{ meta.droppedBelowFloorDb }} pairs below {{ meta.reportFloorDb }} dB floor</span>
      <span v-if="meta.droppedByFindingCap" class="m warn">{{ meta.droppedByFindingCap }} findings over cap — tighten scope</span>
      <span v-if="!report.board.bboxFromOutline" class="m warn">no Edge.Cuts outline — extents from geometry</span>
    </footer>
  </div>
</template>

<style scoped>
.shell { height: 100%; display: flex; flex-direction: column; }

.topbar {
  display: flex; align-items: center; gap: 14px;
  padding: 10px 16px;
  border-bottom: 1px solid var(--resin-edge);
  background: var(--resin);
}
.brand {
  font-family: var(--display); font-weight: 700; font-size: 22px;
  letter-spacing: 0.14em; color: var(--copper);
}
.tagline { color: var(--tin); font-size: 12.5px; }
.spacer { flex: 1; }

.filebtn {
  border: 1px solid var(--resin-edge); border-radius: 4px;
  padding: 6px 12px; color: var(--silk); cursor: pointer;
  font-family: var(--mono); font-size: 12.5px;
  background: var(--bare-fr4);
}
.filebtn:hover { border-color: var(--copper); }
.filebtn input { position: absolute; width: 1px; height: 1px; opacity: 0; }

.stackup {
  background: var(--bare-fr4); color: var(--silk);
  border: 1px solid var(--resin-edge); border-radius: 4px;
  padding: 6px 8px; font-family: var(--mono); font-size: 12.5px;
}

.banner { padding: 12px 16px; font-size: 13.5px; }
.banner.error { background: #3a1a1e; color: #ffb3b8; font-family: var(--mono); }
.banner.wait { background: var(--resin); color: var(--tin); font-family: var(--mono); }
.banner.ask { background: #2a2418; color: var(--silk); display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.chip {
  border: 1px solid var(--heat-med); color: var(--heat-med);
  border-radius: 999px; padding: 5px 14px; font-size: 13px;
}
.chip:hover { background: var(--heat-med); color: var(--bare-fr4); }

.work { flex: 1; display: grid; grid-template-columns: 1fr 380px; min-height: 0; }
@media (max-width: 900px) { .work { grid-template-columns: 1fr; grid-template-rows: 55% 45%; } }

.empty {
  flex: 1; display: flex; flex-direction: column; align-items: center;
  justify-content: center; gap: 14px; text-align: center; padding: 24px;
  border: 2px dashed transparent; transition: border-color 0.15s;
}
.empty.over { border-color: var(--copper); }
.board-ghost {
  width: 220px; height: 140px; border-radius: 6px;
  border: 1.5px solid var(--resin-edge);
  background:
    linear-gradient(90deg, transparent 48%, var(--resin-edge) 48%, var(--resin-edge) 52%, transparent 52%),
    var(--resin);
}
.invite { font-family: var(--display); font-size: 26px; font-weight: 500; }
.invite code { font-family: var(--mono); color: var(--copper); font-size: 22px; }
.sub { max-width: 520px; color: var(--tin); font-size: 13.5px; }

.boardcell { position: relative; display: grid; min-width: 0; min-height: 0; }
.overlaybars {
  /* below the layer chips (top: 10px + chip height), clear of the toggles */
  position: absolute; left: 10px; top: 46px; bottom: 44px; z-index: 6;
  width: min(320px, 44%); display: flex; flex-direction: column; gap: 10px;
  overflow-y: auto; pointer-events: none;
}
.overlaybars.ghost .radbar { pointer-events: none; opacity: 0.25; }
.radbar {
  pointer-events: auto; flex: none;
  display: flex; gap: 6px; flex-direction: column; align-items: flex-start;
  padding: 10px 13px; border: 1px solid var(--resin-edge); border-radius: 8px;
  background: rgba(16, 22, 19, 0.9); backdrop-filter: blur(3px);
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
}
.radbar > b { color: var(--copper); letter-spacing: 0.06em; }
.radbar.nf > b { color: var(--heat-low); }
.radbar .detail {
  border: 1px solid var(--heat-low); border-radius: 3px; padding: 2px 9px;
  font-size: 11px; color: var(--heat-low);
}
.radbar .detail:hover { background: var(--heat-low); color: var(--bare-fr4); }
.radbar .warn { color: var(--heat-med); }
.radbar .top b { color: var(--silk); }
.radbar .top b.bad { color: var(--heat-high); }
.radbar .spec { display: flex; align-items: center; gap: 6px; }
.radbar .spec input { width: 90px; accent-color: var(--copper); }
.radbar .spec select { background: var(--resin); color: var(--silk);
  font-family: var(--mono); font-size: 11px; border: 1px solid var(--resin-edge);
  border-radius: 3px; padding: 2px 5px; }
.radbar .spec select:hover { border-color: var(--copper); }
.radbar .cav { flex: 1 1 100%; opacity: 0.75; font-family: var(--sans); font-size: 11px; }

.metastrip {
  display: flex; gap: 18px; flex-wrap: wrap;
  padding: 7px 16px; border-top: 1px solid var(--resin-edge);
  background: var(--resin);
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
}
.m b { color: var(--silk); font-weight: 500; }
.m.warn { color: var(--heat-med); }
.m.sw b { color: var(--copper); }
</style>
