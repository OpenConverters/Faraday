<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
  finding: { type: Object, required: true },
  titleA: { type: String, default: '' },
  titleB: { type: String, default: '' },
  layer: { type: String, default: '' },
})
const emit = defineEmits(['close'])

// ---- controls ------------------------------------------------------------
// Seeded from the finding's own cross-section, so the bench opens on the pair
// as it is actually laid out rather than on a default anybody has to correct.
const base = props.finding.solve
const gap = ref(base.gapMm)
const rise = ref(0.5)
const lengthMm = ref(base.lengthMm)
const amplitude = ref(3.3)
const family = ref('lvcmos33')
const termination = ref('cmos')
const view = ref('e')

const TERMINATIONS = {
  cmos: { label: 'CMOS (unterminated)', zSrc: 30, zTerm: 1e4, zVictim: 30 },
  series: { label: 'Series terminated', zSrc: 50, zTerm: 1e4, zVictim: 50 },
  matched: { label: 'Matched 50 Ω', zSrc: 50, zTerm: 50, zVictim: 50 },
}
const families = JSON.parse(props.engine.logicFamilies())

const result = ref(null)
const error = ref('')
const busy = ref(false)

function request(withField) {
  const t = TERMINATIONS[termination.value]
  return JSON.stringify({
    ...base,
    gapMm: gap.value,
    lengthMm: lengthMm.value,
    riseNs: rise.value,
    amplitudeV: amplitude.value,
    zSrcOhm: t.zSrc, zTermOhm: t.zTerm, zVictimOhm: t.zVictim,
    family: family.value,
    field: withField,
    fix: withField,
  })
}

// While a slider is moving we re-solve the physics but skip the field map and
// the auto-fix bisection, which are the two expensive parts. They land on the
// next idle frame. Extraction plus transient is a few milliseconds, so the
// numbers and the waveform genuinely track the drag.
let idleTimer = null
function solve(withField) {
  try {
    const out = JSON.parse(props.engine.solvePair(request(withField)))
    if (out.error) { error.value = out.error; return }
    error.value = ''
    // keep the previous field while dragging so the picture does not blink
    if (!withField && result.value?.field) out.field = result.value.field
    result.value = out
  } catch (e) {
    error.value = String(e)
  }
}
function onInput() {
  busy.value = true
  solve(false)
  clearTimeout(idleTimer)
  idleTimer = setTimeout(() => {
    solve(true); busy.value = false; draw()
    runSweep(); nextTick(drawSweep)
  }, 180)
  nextTick(draw)
}
watch([family, termination], () => {
  solve(true)
  runSweep()
  nextTick(() => { draw(); drawSweep() })
})

// ---- field rendering -----------------------------------------------------
const fieldCanvas = ref(null)
const waveCanvas = ref(null)
const sweepCanvas = ref(null)

// The whole curve, not one point at a time: peak noise against separation, 22
// extractions at ~3 ms each. "How much gap do I need" answered visually.
const sweep = ref(null)
function runSweep() {
  try {
    const pts = []
    for (let i = 0; i < 22; i++) {
      const g = 0.05 * Math.pow(2 / 0.05, i / 21)
      const out = JSON.parse(props.engine.solvePair(JSON.stringify({
        ...JSON.parse(request(false)), gapMm: g, field: false, fix: false,
      })))
      if (out.error) continue
      pts.push({ g, mv: Math.max(Math.abs(out.spice.nextMv), Math.abs(out.spice.fextMv)) })
    }
    sweep.value = pts
  } catch { sweep.value = null }
}

function drawSweep() {
  const cv = sweepCanvas.value
  const pts = sweep.value
  if (!cv || !pts?.length || !result.value) return
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)
  const budget = result.value.verdict.budgetV * 1000
  const top = Math.max(budget * 1.4, ...pts.map(p => p.mv)) * 1.1
  const X = g => 34 + (Math.log(g / 0.05) / Math.log(2 / 0.05)) * (w - 40)
  const Y = mv => 6 + (1 - mv / top) * (h - 22)
  ctx.font = '9px IBM Plex Mono, monospace'
  ctx.strokeStyle = 'rgba(255,93,93,0.55)'
  ctx.setLineDash([4, 3])
  ctx.beginPath(); ctx.moveTo(34, Y(budget)); ctx.lineTo(w - 4, Y(budget)); ctx.stroke()
  ctx.setLineDash([])
  ctx.fillStyle = 'rgba(255,93,93,0.8)'
  ctx.fillText('budget', 36, Y(budget) - 3)
  ctx.strokeStyle = '#d98b5f'
  ctx.lineWidth = 1.6
  ctx.beginPath()
  pts.forEach((p2, i) => i ? ctx.lineTo(X(p2.g), Y(p2.mv)) : ctx.moveTo(X(p2.g), Y(p2.mv)))
  ctx.stroke()
  // the current setting
  ctx.fillStyle = '#58c79a'
  ctx.beginPath(); ctx.arc(X(Math.max(0.05, Math.min(2, gap.value))),
    Y(Math.max(...[result.value.verdict.peakMv]) || 0), 3.2, 0, 7); ctx.fill()
  ctx.fillStyle = 'rgba(157,180,173,0.7)'
  for (const g of [0.1, 0.5, 1, 2]) { ctx.textAlign = 'center'; ctx.fillText(String(g), X(g), h - 4) }
  ctx.textAlign = 'left'
  ctx.fillText('mm', 4, h - 4)
}

// The field window is framed on the structure, so its aspect is whatever the
// geometry needs. Stretching it into a fixed canvas ratio would misrepresent
// the shape of the field, which is the one thing the picture is for.
const fieldAspect = computed(() => {
  const f = result.value?.field
  if (!f) return 2
  return Math.max(1.2, Math.min(4.5, (f.x1Mm - f.x0Mm) / (f.y1Mm - f.y0Mm)))
})

function decode(b64) {
  const bin = atob(b64)
  const a = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i)
  return a
}

// |E| runs over decades and is shipped log-scaled, so a perceptual ramp from
// the board's own dark resin through copper to hot silk reads as field
// strength without inventing a rainbow.
function ramp(t, mode) {
  if (mode === 'v') {
    // potential: 0 V is the plane, 1 V is the aggressor
    return [16 + 232 * t * t, 22 + 150 * t, 19 + 110 * t * t]
  }
  // |E| arrives log-scaled over four decades, and most of a cross-section sits
  // within a couple of them — mapped straight onto the ramp that lights up
  // almost the whole picture. The gamma pushes the bulk back down so the field
  // CONCENTRATION at the trace edges, which is the thing worth looking at, is
  // what stands out.
  const s = Math.pow(Math.min(1, Math.max(0, t)), 2.2)
  if (s < 0.6) { const u = s / 0.6; return [16 + 201 * u, 22 + 117 * u, 19 + 76 * u] }
  const u = (s - 0.6) / 0.4
  return [217 + 38 * u, 139 + 98 * u, 95 + 137 * u]
}

function draw() {
  drawField()
  drawWave()
}

function drawField() {
  const cv = fieldCanvas.value
  const r = result.value
  if (!cv || !r?.field) return
  const f = r.field
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  const data = decode(view.value === 'v' ? f.v : f.e)
  const img = ctx.createImageData(f.nx, f.ny)
  for (let y = 0; y < f.ny; y++) {
    for (let x = 0; x < f.nx; x++) {
      const src = y * f.nx + x
      // the field grid runs bottom-up in y; canvas runs top-down
      const dst = ((f.ny - 1 - y) * f.nx + x) * 4
      const [rr, gg, bb] = ramp(data[src] / 255, view.value)
      img.data[dst] = rr; img.data[dst + 1] = gg; img.data[dst + 2] = bb
      img.data[dst + 3] = 255
    }
  }
  const off = document.createElement('canvas')
  off.width = f.nx; off.height = f.ny
  off.getContext('2d').putImageData(img, 0, 0)
  ctx.imageSmoothingEnabled = true
  ctx.imageSmoothingQuality = 'high'
  ctx.drawImage(off, 0, 0, w, h)

  // conductor outlines on top, in board coordinates
  const g = r.geometry
  const sx = w / (f.x1Mm - f.x0Mm), sy = h / (f.y1Mm - f.y0Mm)
  const X = mm => (mm - f.x0Mm) * sx
  const Y = mm => h - (mm - f.y0Mm) * sy
  const rect = (x0, y0, x1, y1, label) => {
    ctx.fillStyle = 'rgba(230,237,232,0.92)'
    ctx.fillRect(X(x0), Y(y1), (x1 - x0) * sx, (y1 - y0) * sy)
    ctx.strokeStyle = '#101613'; ctx.lineWidth = 1
    ctx.strokeRect(X(x0), Y(y1), (x1 - x0) * sx, (y1 - y0) * sy)
    if (label) {
      // single-letter tags: at realistic separations two words centred on two
      // 0.2 mm traces overlap into an unreadable smudge
      ctx.fillStyle = '#e6ede8'
      ctx.font = '600 11px IBM Plex Mono, monospace'
      ctx.textAlign = 'center'
      ctx.fillText(label, X((x0 + x1) / 2), Y(y1) - 6)
    }
  }
  const t = g.tMm
  if (g.mode === 'broadside') {
    rect(-g.w1Mm / 2, g.hMm, g.w1Mm / 2, g.hMm + t, 'A')
    rect(-g.w2Mm / 2, g.hMm + g.hvMm, g.w2Mm / 2, g.hMm + g.hvMm + t, 'V')
  } else {
    const y = g.mode === 'stripline' ? (g.bMm - t) / 2 : g.hMm
    rect(-(g.gapMm / 2 + g.w1Mm), y, -g.gapMm / 2, y + t, 'A')
    rect(g.gapMm / 2, y, g.gapMm / 2 + g.w2Mm, y + t, 'V')
    if (g.mode === 'stripline') {
      ctx.fillStyle = 'rgba(230,237,232,0.92)'
      ctx.fillRect(0, Y(g.bMm + t), w, Math.max(2, t * sy))
    }
  }
  // the reference plane: exact in the solve, drawn as the floor it is
  ctx.fillStyle = 'rgba(230,237,232,0.92)'
  ctx.fillRect(0, h - 3, w, 3)

  // dielectric interface
  if (g.mode === 'microstrip') {
    ctx.strokeStyle = 'rgba(157,180,173,0.35)'
    ctx.setLineDash([4, 3]); ctx.lineWidth = 1
    ctx.beginPath(); ctx.moveTo(0, Y(g.hMm)); ctx.lineTo(w, Y(g.hMm)); ctx.stroke()
    ctx.setLineDash([])
  }
}

function drawWave() {
  const cv = waveCanvas.value
  const r = result.value
  if (!cv || !r?.spice) return
  const sp = r.spice
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  const budget = r.verdict.budgetV
  const peak = Math.max(Math.abs(sp.nextMv), Math.abs(sp.fextMv)) / 1000
  const span = Math.max(budget * 1.25, peak * 1.3, 0.02)
  const t1 = sp.t[sp.t.length - 1] || 1
  const X = t => (t / t1) * (w - 44) + 40
  const Y = v => h / 2 - (v / span) * (h / 2 - 12)

  // the receiver threshold: the line this whole panel is about
  for (const s of [1, -1]) {
    ctx.strokeStyle = 'rgba(255,93,93,0.5)'
    ctx.setLineDash([5, 4]); ctx.lineWidth = 1
    ctx.beginPath(); ctx.moveTo(40, Y(s * budget)); ctx.lineTo(w - 4, Y(s * budget))
    ctx.stroke(); ctx.setLineDash([])
  }
  ctx.fillStyle = 'rgba(255,93,93,0.75)'
  ctx.font = '10px IBM Plex Mono, monospace'
  ctx.textAlign = 'left'
  ctx.fillText(`${(budget * 1000).toFixed(0)} mV`, 4, Y(budget) + 3)

  ctx.strokeStyle = 'rgba(157,180,173,0.25)'
  ctx.beginPath(); ctx.moveTo(40, Y(0)); ctx.lineTo(w - 4, Y(0)); ctx.stroke()

  const line = (arr, color, width) => {
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.beginPath()
    for (let i = 0; i < arr.length; i++) {
      const x = X(sp.t[i]), y = Y(arr[i])
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
    }
    ctx.stroke()
  }
  line(sp.vicFar, '#ffb454', 1.2)
  line(sp.vicNear, '#58c79a', 1.8)
  if (Math.abs(sp.nextMv) / 1000 > budget || Math.abs(sp.fextMv) / 1000 > budget)
    line(sp.vicNear, '#ff5d5d', 1.8)

  ctx.fillStyle = 'rgba(157,180,173,0.8)'
  ctx.textAlign = 'right'
  ctx.fillText(`${(t1 * 1e9).toFixed(1)} ns`, w - 4, h - 3)
}

const ro = new ResizeObserver(() => draw())
onMounted(() => {
  solve(true)
  runSweep()
  nextTick(() => {
    draw()
    drawSweep()
    if (fieldCanvas.value) ro.observe(fieldCanvas.value)
    if (waveCanvas.value) ro.observe(waveCanvas.value)
  })
  window.addEventListener('keydown', onKey)
})
onBeforeUnmount(() => {
  ro.disconnect()
  clearTimeout(idleTimer)
  window.removeEventListener('keydown', onKey)
})
function onKey(e) { if (e.key === 'Escape') emit('close') }
watch(view, () => nextTick(drawField))

const v = computed(() => result.value?.verdict ?? null)
const rl = computed(() => result.value?.rlgc ?? null)
const num = (x, d = 2) => (x === undefined || x === null ? '—' : x.toFixed(d))
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="bench" data-testid="bench" role="dialog" aria-label="Field bench">
      <header class="bhead">
        <h2>BENCH</h2>
        <span class="pair">{{ titleA }} <i>↔</i> {{ titleB }}</span>
        <span class="where">{{ layer }} · {{ result?.geometry?.mode ?? base.mode }}</span>
        <div class="sp" />
        <span v-if="result" class="cost" data-testid="bench-cost">
          {{ num(result.timingMs.total, 1) }} ms · {{ result.geometry.panels }} panels
        </span>
        <button class="x" @click="emit('close')" aria-label="Close bench">✕</button>
      </header>

      <p v-if="error" class="err" data-testid="bench-error">{{ error }}</p>

      <div v-if="result" class="grid">
        <!-- hero: the field, solved from the actual cross-section -->
        <figure class="fieldwrap">
          <canvas ref="fieldCanvas" data-testid="bench-field"
                  :style="{ aspectRatio: fieldAspect }" />
          <figcaption>
            <button :class="{ on: view === 'e' }" @click="view = 'e'">|E| field</button>
            <button :class="{ on: view === 'v' }" @click="view = 'v'">potential</button>
            <span class="cap"><b>A</b> aggressor at 1 V · <b>V</b> victim ·
              boundary-element solve, reference plane exact by images</span>
          </figcaption>
        </figure>

        <!-- the answer -->
        <div class="verdict" :class="v.level" data-testid="bench-verdict">
          <div class="big">{{ num(v.peakMv, 0) }}<small>mV</small></div>
          <div class="of">on the victim, against {{ num(v.budgetV * 1000, 0) }} mV of
            {{ v.familyLabel }} margin</div>
          <div class="bar"><i :style="{ width: Math.min(100, v.pctOfBudget) + '%' }" /></div>
          <div class="pct">{{ num(v.pctOfBudget, 0) }}% of budget
            <b>{{ v.level === 'fail' ? 'over' : v.level === 'watch' ? 'tight' : 'clear' }}</b>
          </div>
          <button v-if="result.fix" class="fix" data-testid="bench-fix"
                  @click="gap = result.fix.gapMm; onInput()">
            Open the gap to <b>{{ num(result.fix.gapMm, 2) }} mm</b>
            → {{ num(result.fix.pctAfter, 0) }}%
          </button>
          <p v-else-if="v.level !== 'ok'" class="nofix">
            Separation alone will not fix this — shorten the run, add a guarded
            trace, or move one net across a plane.
          </p>
        </div>

        <!-- transient -->
        <figure class="wavewrap">
          <canvas ref="waveCanvas" data-testid="bench-wave" />
          <figcaption>
            <span class="k near">near-end (NEXT) {{ num(result.spice.nextMv, 0) }} mV</span>
            <span class="k far">far-end (FEXT) {{ num(result.spice.fextMv, 0) }} mV</span>
            <span class="cap">{{ result.spice.sections }}-section coupled ladder ·
              {{ result.spice.steps }} steps · {{ num(result.spice.delayNs, 2) }} ns one way</span>
          </figcaption>
        </figure>

        <!-- the whole curve, not one point: peak noise vs separation -->
        <figure class="sweepwrap">
          <canvas ref="sweepCanvas" data-testid="bench-sweep" />
          <figcaption><span class="cap">peak victim noise vs separation — the
            green dot is where your sliders are now</span></figcaption>
        </figure>

        <!-- extracted parameters -->
        <dl class="params" data-testid="bench-rlgc">
          <div><dt>Z₀</dt><dd>{{ num(rl.z0, 1) }} Ω</dd></div>
          <div><dt>Z even / odd</dt><dd>{{ num(rl.zEven, 1) }} / {{ num(rl.zOdd, 1) }} Ω</dd></div>
          <div><dt>Z diff</dt><dd>{{ num(rl.zDiff, 1) }} Ω</dd></div>
          <div><dt>εeff</dt><dd>{{ num(rl.epsEff, 2) }}</dd></div>
          <div><dt>delay</dt><dd>{{ num(rl.delayPsPerMm, 1) }} ps/mm</dd></div>
          <div><dt>k backward</dt><dd>{{ num(rl.kb, 4) }} ({{ num(rl.kbDb, 1) }} dB)</dd></div>
          <div><dt>L mutual</dt><dd>{{ num(rl.lMutualNhPerMm, 3) }} nH/mm</dd></div>
          <div><dt>C mutual</dt><dd>{{ num(rl.cMutualPfPerMm, 4) }} pF/mm</dd></div>
        </dl>
      </div>

      <!-- controls -->
      <div class="controls" :class="{ busy }">
        <label class="sl"><span>separation <b>{{ num(gap, 2) }} mm</b></span>
          <input data-testid="bench-gap" type="range" min="0.05" max="2" step="0.01"
                 v-model.number="gap" @input="onInput" /></label>
        <label class="sl"><span>edge rate <b>{{ num(rise, 2) }} ns</b></span>
          <input data-testid="bench-rise" type="range" min="0.05" max="5" step="0.05"
                 v-model.number="rise" @input="onInput" /></label>
        <label class="sl"><span>coupled length <b>{{ num(lengthMm, 1) }} mm</b></span>
          <input type="range" min="2" max="250" step="1"
                 v-model.number="lengthMm" @input="onInput" /></label>
        <label class="sl"><span>swing <b>{{ num(amplitude, 1) }} V</b></span>
          <input type="range" min="0.8" max="5" step="0.1"
                 v-model.number="amplitude" @input="onInput" /></label>
        <label class="pick"><span>receiver</span>
          <select v-model="family" data-testid="bench-family">
            <option v-for="f in families" :key="f.id" :value="f.id">{{ f.label }}</option>
          </select></label>
        <label class="pick"><span>termination</span>
          <select v-model="termination" data-testid="bench-term">
            <option v-for="(t, k) in TERMINATIONS" :key="k" :value="k">{{ t.label }}</option>
          </select></label>
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
.bench {
  width: min(1120px, 100%); max-height: 100%;
  display: flex; flex-direction: column;
  background: var(--resin); border: 1px solid var(--resin-edge);
  border-radius: 8px; overflow: auto;
}

.bhead {
  display: flex; align-items: baseline; gap: 12px;
  padding: 11px 16px; border-bottom: 1px solid var(--resin-edge);
}
.bhead h2 {
  font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper);
}
.pair { font-family: var(--mono); font-size: 13px; }
.pair i { color: var(--tin); font-style: normal; }
.where, .cost { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }
.err {
  margin: 12px 16px; padding: 10px 12px; border-radius: 4px;
  background: #3a1a1e; color: #ffb3b8; font-family: var(--mono); font-size: 12.5px;
}

.grid {
  display: grid; gap: 14px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1.55fr) minmax(0, 1fr);
}
@media (max-width: 820px) { .grid { grid-template-columns: 1fr; } }

figure { min-width: 0; }
.fieldwrap canvas { width: 100%; display: block; border-radius: 4px; background: #0d1210; }
.wavewrap canvas { width: 100%; height: 150px; display: block; border-radius: 4px; background: #0d1210; }
.sweepwrap canvas { width: 100%; height: 92px; display: block; border-radius: 4px; background: #0d1210; }
figcaption {
  display: flex; align-items: center; gap: 8px; flex-wrap: wrap;
  padding-top: 6px; font-family: var(--mono); font-size: 11px; color: var(--tin);
}
figcaption button {
  border: 1px solid var(--resin-edge); border-radius: 3px;
  padding: 2px 8px; font-size: 11px; color: var(--tin);
}
figcaption button.on { border-color: var(--copper); color: var(--copper); }
.cap { flex: 1 1 100%; opacity: 0.75; }
.k::before { content: '—'; margin-right: 4px; font-weight: 700; }
.k.near { color: var(--heat-low); }
.k.far { color: var(--heat-med); }

.verdict {
  align-self: start;
  border: 1px solid var(--resin-edge); border-left-width: 3px;
  border-radius: 4px; padding: 14px;
  display: flex; flex-direction: column; gap: 8px;
}
.verdict.ok { border-left-color: var(--heat-low); }
.verdict.watch { border-left-color: var(--heat-med); }
.verdict.fail { border-left-color: var(--heat-high); }
.big { font-family: var(--display); font-size: 46px; font-weight: 700; line-height: 1; }
.big small { font-size: 17px; font-weight: 500; color: var(--tin); margin-left: 4px; }
.of { font-size: 12.5px; color: var(--tin); }
.bar { height: 6px; border-radius: 3px; background: #0d1210; overflow: hidden; }
.bar i { display: block; height: 100%; background: var(--heat-low); }
.verdict.watch .bar i { background: var(--heat-med); }
.verdict.fail .bar i { background: var(--heat-high); }
.pct { font-family: var(--mono); font-size: 12px; color: var(--tin); }
.pct b { color: var(--silk); }
.fix {
  margin-top: 2px; text-align: left;
  border: 1px solid var(--copper); border-radius: 4px;
  padding: 7px 10px; font-size: 12.5px; color: var(--copper);
}
.fix:hover { background: var(--copper); color: var(--bare-fr4); }
.nofix { font-size: 12px; color: var(--tin); }

.params {
  grid-column: 2; display: grid; gap: 2px 12px;
  font-family: var(--mono); font-size: 11.5px;
}
.params > div { display: flex; justify-content: space-between; gap: 10px;
  border-bottom: 1px dotted var(--resin-edge); padding: 3px 0; }
.params dt { color: var(--tin); }
@media (max-width: 820px) { .params { grid-column: 1; } }

.controls {
  display: grid; gap: 10px 18px; padding: 12px 16px;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  border-top: 1px solid var(--resin-edge); background: var(--bare-fr4);
}
.controls.busy { cursor: progress; }
.sl, .pick { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b, .pick span { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: var(--copper); }
.pick select {
  background: var(--resin); color: var(--silk); font-family: var(--mono); font-size: 12px;
  border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px;
}
</style>
