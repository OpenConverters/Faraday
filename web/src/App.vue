<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import BoardView from './components/BoardView.vue'
import FindingsList from './components/FindingsList.vue'
import BenchPanel from './components/BenchPanel.vue'
import EmissionsPanel from './components/EmissionsPanel.vue'
import NearFieldPanel from './components/NearFieldPanel.vue'

const engine = ref(null)
const boardText = ref('')
const fileName = ref('')
const report = ref(null)
const error = ref('')
const needStackup = ref(false)
const stackupChoice = ref('')   // '' = from board file
const selectedId = ref('')
const dragOver = ref(false)

onMounted(async () => {
  engine.value = await window.createFaraday()
})

function analyze() {
  if (!engine.value || !boardText.value) return
  error.value = ''
  needStackup.value = false
  const out = JSON.parse(engine.value.analyze(boardText.value, stackupChoice.value))
  if (out.error) {
    report.value = null
    if (out.error.includes('no stackup')) {
      // explicit choice required — Faraday never assumes a stackup silently
      needStackup.value = true
    } else {
      error.value = out.error
    }
    return
  }
  report.value = out
  selectedId.value = ''
}

async function onFile(file) {
  if (!file) return
  fileName.value = file.name
  boardText.value = await file.text()
  stackupChoice.value = ''
  analyze()
}

function onDrop(e) {
  dragOver.value = false
  onFile(e.dataTransfer?.files?.[0])
}

function chooseStackup(name) {
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
  findings.value.filter(f => !hiddenRules.value.has(f.rule)))
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
// The radiation layer: a whole-board attribution of differential-mode
// emissions to the copper that causes it. Off by default — it re-colours the
// board, so it has to be asked for.
const radiation = ref(null)
const radError = ref('')
function toggleRadiation() {
  if (radiation.value) { radiation.value = null; return }
  try {
    const out = JSON.parse(engine.value.radiationMap(JSON.stringify({})))
    if (out.error) { radError.value = out.error; return }
    radError.value = ''
    radiation.value = out
  } catch (e) { radError.value = String(e) }
}
watch(report, () => { radiation.value = null; radError.value = '' })

// The component near-field map: a different regime from the far-field
// attribution, so it is a separate panel with its own units and its own caveat.
const nearFieldOpen = ref(false)
watch(report, () => { nearFieldOpen.value = false })

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
        <input data-testid="file-input" type="file" accept=".kicad_pcb,.hyp,.HYP,.xml"
               @change="e => onFile(e.target.files[0])" />
        {{ fileName || 'Open board file' }}
      </label>
      <select v-if="report || needStackup" data-testid="stackup-select" class="stackup"
              :value="stackupChoice" @change="e => chooseStackup(e.target.value)"
              aria-label="Stackup">
        <option value="">Stackup: from board file</option>
        <option value="default-2layer">Stackup: default 2-layer FR4</option>
        <option value="default-4layer">Stackup: default 4-layer FR4</option>
      </select>
    </header>

    <div v-if="error" class="banner error" data-testid="error-banner">{{ error }}</div>
    <div v-if="radError" class="banner error" data-testid="rad-error">{{ radError }}</div>

    <div v-if="radiation" class="radbar" data-testid="rad-bar">
      <b>Radiation attribution</b>
      <span>{{ radiation.totalDbuvM.toFixed(1) }} dBµV/m total at
        {{ radiation.distanceM.toFixed(0) }} m, over {{ radiation.counted }} segments</span>
      <span v-if="radiation.overVoidCount" class="warn" data-testid="rad-void">
        {{ radiation.overVoidCount }} segments run over a plane void or split —
        their return current detours, and the map shows what that costs</span>
      <span v-if="radiation.noReferenceCount" class="warn">
        {{ radiation.noReferenceCount }} segments have no reference plane —
        {{ radiation.noReferenceSharePct.toFixed(0) }}% of the total</span>
      <span class="top" v-for="t in radiation.top.slice(0, 4)" :key="t.seg">
        {{ t.net || '(unnamed)' }} <b>{{ t.sharePct.toFixed(1) }}%</b></span>
      <span class="cav">Differential-mode attribution from loop area × current —
        a ranking of your copper, not a chamber. Current is assumed, and scales
        it all linearly.</span>
    </div>

    <div v-if="needStackup" class="banner ask" data-testid="stackup-card">
      <p>This board file carries no stackup. Z₀ and coupling need one — choose what the
         board is built on (the report will state your choice):</p>
      <button class="chip" @click="chooseStackup('default-2layer')">Default 2-layer FR4 (1.6 mm)</button>
      <button class="chip" @click="chooseStackup('default-4layer')">Default 4-layer FR4 (1.6 mm)</button>
    </div>

    <main class="work" v-if="report">
      <BoardView :report="report" :findings="visibleFindings" :selected-id="selectedId"
                 :radiation="radiation"
                 @select="id => selectedId = id"
                 @toggle-radiation="toggleRadiation"
                 @near-field="nearFieldOpen = true" />
      <FindingsList :findings="visibleFindings" :report="report" :selected-id="selectedId"
                    :rules="ruleCounts" :hidden-rules="hiddenRules" :total="findings.length"
                    @select="id => selectedId = selectedId === id ? '' : id"
                    @toggle-rule="toggleRule"
                    @bench="id => benchId = id"
                    @emissions="id => emitId = id" />
    </main>

    <div v-else-if="!needStackup" class="empty" :class="{ over: dragOver }">
      <div class="board-ghost" aria-hidden="true" />
      <p class="invite">Drop a board here</p>
      <p class="formats"><code>.kicad_pcb</code> · <code>.hyp</code> · <code>IPC-2581 .xml</code></p>
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

    <NearFieldPanel v-if="nearFieldOpen && engine && report" :engine="engine"
                    :report="report" @close="nearFieldOpen = false" />

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

.radbar {
  display: flex; gap: 14px; align-items: baseline; flex-wrap: wrap;
  padding: 7px 16px; border-bottom: 1px solid var(--resin-edge);
  background: var(--resin); font-family: var(--mono); font-size: 11.5px;
  color: var(--tin);
}
.radbar > b { color: var(--copper); letter-spacing: 0.06em; }
.radbar .warn { color: var(--heat-med); }
.radbar .top b { color: var(--silk); }
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
