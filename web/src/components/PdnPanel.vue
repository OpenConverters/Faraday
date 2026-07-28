<script setup>
import { ref, computed, onMounted, onBeforeUnmount, nextTick, watch } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
})
const emit = defineEmits(['close'])

const result = ref(null)
const error = ref('')
const railIdx = ref(0)
// The target is the user's, not the model's: Z_target = allowed ripple over
// transient current. Both are design decisions Faraday cannot know.
const dI = ref(1.0)      // A
const dV = ref(50)       // mV
const vrmL = ref(20)     // nH

function run() {
  try {
    const out = JSON.parse(props.engine.pdn(JSON.stringify({ vrmLnH: vrmL.value })))
    if (out.error) { error.value = out.error; result.value = null; return }
    error.value = ''
    result.value = out
    if (railIdx.value >= out.rails.length) railIdx.value = 0
  } catch (e) { error.value = String(e) }
}

const rail = computed(() => result.value?.rails?.[railIdx.value] ?? null)
const zTarget = computed(() => (dV.value / 1000) / Math.max(dI.value, 1e-6))
const worstAntires = computed(() => {
  const r = rail.value
  if (!r?.antires?.length) return null
  return r.antires.reduce((a, b) => (b.zOhm > a.zOhm ? b : a))
})

// ---- the chart: log-log |Z|(f) ------------------------------------------
const canvas = ref(null)
function draw() {
  const cv = canvas.value
  const r = rail.value
  if (!cv || !r) return
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  const padL = 46, padR = 10, padT = 10, padB = 24
  const f0 = 0.01, f1 = 1000                       // MHz
  const zs = r.zOhm.filter(z => z > 0)
  const zlo = Math.max(1e-3, Math.min(...zs) / 2)
  const zhi = Math.max(zTarget.value * 4, Math.max(...zs) * 1.5)
  const X = f => padL + (Math.log10(f / f0) / Math.log10(f1 / f0)) * (w - padL - padR)
  const Y = z => padT + (1 - Math.log10(z / zlo) / Math.log10(zhi / zlo)) * (h - padT - padB)

  ctx.font = '10px IBM Plex Mono, monospace'
  ctx.strokeStyle = 'rgba(157,180,173,0.13)'
  ctx.fillStyle = 'rgba(157,180,173,0.65)'
  for (const f of [0.01, 0.1, 1, 10, 100, 1000]) {
    ctx.beginPath(); ctx.moveTo(X(f), padT); ctx.lineTo(X(f), h - padB); ctx.stroke()
    ctx.textAlign = 'center'
    ctx.fillText(f >= 1 ? String(f) : String(f), X(f), h - padB + 13)
  }
  for (const z of [0.001, 0.01, 0.1, 1, 10]) {
    if (z < zlo || z > zhi) continue
    ctx.beginPath(); ctx.moveTo(padL, Y(z)); ctx.lineTo(w - padR, Y(z)); ctx.stroke()
    ctx.textAlign = 'right'
    ctx.fillText(z >= 1 ? `${z}Ω` : `${z * 1000}mΩ`, padL - 5, Y(z) + 3)
  }
  ctx.textAlign = 'center'
  ctx.fillText('MHz', (padL + w) / 2, h - 3)

  // the target line: above it, the rail cannot hold the ripple at that f
  ctx.strokeStyle = 'rgba(255,93,93,0.6)'
  ctx.setLineDash([6, 4]); ctx.lineWidth = 1.6
  ctx.beginPath(); ctx.moveTo(padL, Y(zTarget.value)); ctx.lineTo(w - padR, Y(zTarget.value))
  ctx.stroke(); ctx.setLineDash([])
  ctx.fillStyle = 'rgba(255,93,93,0.85)'
  ctx.textAlign = 'left'
  ctx.fillText('target', padL + 4, Y(zTarget.value) - 4)

  // |Z|
  ctx.strokeStyle = '#8fb8ff'
  ctx.lineWidth = 1.8
  ctx.beginPath()
  for (let i = 0; i < r.fMhz.length; i++) {
    const x = X(Math.max(f0, r.fMhz[i])), y = Y(Math.min(zhi, Math.max(zlo, r.zOhm[i])))
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
  }
  ctx.stroke()

  // per-cap series resonances as ticks — where each cap actually works
  ctx.fillStyle = 'rgba(88,199,154,0.8)'
  for (const c of r.caps) {
    const x = X(Math.max(f0, Math.min(f1, c.fResMhz)))
    ctx.fillRect(x - 1, h - padB - 6, 2, 6)
  }
  // anti-resonances
  ctx.fillStyle = '#ffb454'
  for (const a of r.antires) {
    ctx.beginPath()
    ctx.arc(X(Math.max(f0, Math.min(f1, a.fMhz))), Y(Math.min(zhi, a.zOhm)), 3, 0, 7)
    ctx.fill()
  }
}

const ro = new ResizeObserver(() => draw())
onMounted(() => {
  run()
  nextTick(() => { draw(); if (canvas.value) ro.observe(canvas.value) })
  window.addEventListener('keydown', onKey)
})
onBeforeUnmount(() => { ro.disconnect(); window.removeEventListener('keydown', onKey) })
function onKey(e) { if (e.key === 'Escape') emit('close') }
watch([rail, dI, dV], () => nextTick(draw))
watch(vrmL, () => { run(); nextTick(draw) })

const num = (x, d = 1) => (x === undefined || x === null ? '—' : Number(x).toFixed(d))
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="pdn-panel" role="dialog" aria-label="PDN impedance">
      <header>
        <h2>PDN</h2>
        <span class="sub" v-if="result">against {{ result.gnd }}</span>
        <select v-if="result && result.rails.length > 1" v-model.number="railIdx"
                data-testid="pdn-rail" class="railsel">
          <option v-for="(r, i) in result.rails" :key="r.net" :value="i">
            {{ r.net }} ({{ r.caps.length }} caps)</option>
        </select>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <p v-if="error" class="err" data-testid="pdn-error">{{ error }}</p>

      <div v-if="rail" class="body">
        <figure>
          <canvas ref="canvas" data-testid="pdn-chart" />
          <figcaption>
            <span class="k z">|Z| of {{ rail.net }}</span>
            <span class="k res">▎cap series resonances</span>
            <span class="k anti">● anti-resonances</span>
            <span class="k tgt">- - target {{ (zTarget * 1000).toFixed(0) }} mΩ</span>
          </figcaption>
        </figure>

        <div class="side">
          <div class="verdict" :class="rail.zMaxOhm > zTarget ? 'over' : 'ok'"
               data-testid="pdn-verdict">
            <div class="big">{{ rail.zMaxOhm >= 1 ? num(rail.zMaxOhm, 2) + ' Ω'
                              : num(rail.zMaxOhm * 1000, 0) + ' mΩ' }}</div>
            <div class="of">worst impedance in 0.1–100 MHz, at
              {{ num(rail.zMaxMhz, 1) }} MHz — target
              {{ (zTarget * 1000).toFixed(0) }} mΩ from
              {{ num(dI) }} A / {{ num(dV, 0) }} mV</div>
            <p v-if="worstAntires" class="hint">Worst anti-resonance
              {{ worstAntires.zOhm >= 1 ? num(worstAntires.zOhm, 2) + ' Ω'
                 : num(worstAntires.zOhm * 1000, 0) + ' mΩ' }} at
              {{ num(worstAntires.fMhz, 1) }} MHz — paralleled caps fighting.
              Adding a capacitor can make a frequency <b>worse</b>; this is where.</p>
          </div>

          <table class="caps" data-testid="pdn-caps">
            <thead><tr><th>cap</th><th>C</th><th>ESL</th><th>mount</th><th>f res</th></tr></thead>
            <tbody>
              <tr v-for="c in [...rail.caps].sort((a, b) => b.lMountNh - a.lMountNh).slice(0, 12)"
                  :key="c.ref" :class="{ bad: c.noVia }">
                <td>{{ c.ref }}</td>
                <td>{{ c.cLabel }}</td>
                <td>{{ num(c.eslNh, 1) }} nH</td>
                <td>{{ num(c.lMountNh, 1) }} nH<i v-if="c.noVia"
                    title="no same-net via within reach of a pad">!</i></td>
                <td>{{ num(c.fResMhz, 1) }} MHz</td>
              </tr>
            </tbody>
          </table>
          <p class="note">Mounting inductance is <b>measured off this board</b> —
            pad-to-via escape on each terminal plus the barrels, ~0.8 nH/mm and
            0.3 nH/via, order-of-magnitude figures meant to RANK. A cap whose
            mount exceeds its ESL is wasted by placement, not by choice of part.
            <template v-if="rail.planeCpF > 1"> Plane pair contributes
              {{ num(rail.planeCpF, 0) }} pF over
              {{ num(rail.planeOverlapMm2, 0) }} mm² of real overlap.</template>
            <template v-if="rail.skippedUnparsed"> {{ rail.skippedUnparsed }}
              cap value(s) unparseable — skipped, not guessed.</template>
            Lumped and linear: no distributed plane resonance above ~1 GHz, no
            die capacitance.</p>
        </div>
      </div>

      <div class="controls">
        <label class="sl"><span>transient current <b>{{ num(dI) }} A</b></span>
          <input data-testid="pdn-di" type="range" min="0.1" max="20" step="0.1"
                 v-model.number="dI" /></label>
        <label class="sl"><span>allowed ripple <b>{{ num(dV, 0) }} mV</b></span>
          <input type="range" min="5" max="300" step="5" v-model.number="dV" /></label>
        <label class="sl"><span>VRM inductance <b>{{ num(vrmL, 0) }} nH</b></span>
          <input type="range" min="1" max="200" step="1" v-model.number="vrmL" /></label>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 50; background: rgba(8,12,10,0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px; }
.panel { width: min(1120px, 100%); max-height: 100%; display: flex; flex-direction: column;
  overflow: auto; background: var(--resin); border: 1px solid var(--resin-edge); border-radius: 8px; }
header { display: flex; align-items: center; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: #8fb8ff; }
.sub { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.railsel { background: var(--bare-fr4); color: var(--silk); font-family: var(--mono);
  font-size: 12px; border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px; }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }
.err { margin: 12px 16px; padding: 10px 12px; border-radius: 4px; background: #3a1a1e;
  color: #ffb3b8; font-family: var(--mono); font-size: 12.5px; }

.body { display: grid; gap: 14px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1.5fr) minmax(0, 1fr); }
@media (max-width: 880px) { .body { grid-template-columns: 1fr; } }
canvas { width: 100%; height: 320px; display: block; border-radius: 4px; background: #0d1210; }
figcaption { display: flex; gap: 14px; flex-wrap: wrap; padding-top: 6px;
  font-family: var(--mono); font-size: 11px; color: var(--tin); }
.k.z { color: #8fb8ff; }
.k.res { color: var(--heat-low); }
.k.anti { color: var(--heat-med); }
.k.tgt { color: var(--heat-high); }

.side { display: flex; flex-direction: column; gap: 10px; min-width: 0; }
.verdict { border: 1px solid var(--resin-edge); border-left-width: 3px; border-radius: 4px;
  padding: 12px; display: flex; flex-direction: column; gap: 6px; }
.verdict.ok { border-left-color: var(--heat-low); }
.verdict.over { border-left-color: var(--heat-high); }
.big { font-family: var(--display); font-size: 34px; font-weight: 700; line-height: 1; }
.of { font-size: 12.5px; color: var(--tin); }
.hint { font-size: 11.5px; color: var(--tin); }
.hint b { color: var(--silk); }

.caps { width: 100%; border-collapse: collapse; font-family: var(--mono); font-size: 11px; }
.caps th { text-align: left; color: var(--tin); font-weight: 500; padding: 3px 6px 5px 0;
  border-bottom: 1px solid var(--resin-edge); }
.caps td { padding: 3px 6px 3px 0; border-bottom: 1px dotted var(--resin-edge); }
.caps tr.bad td { color: var(--heat-med); }
.caps i { color: var(--heat-high); font-style: normal; padding-left: 2px; }
.note { font-size: 11.5px; color: var(--tin); }
.note b { color: var(--silk); }

.controls { display: grid; gap: 10px 18px; padding: 12px 16px;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  border-top: 1px solid var(--resin-edge); background: var(--bare-fr4); }
.sl { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: #8fb8ff; }
</style>
