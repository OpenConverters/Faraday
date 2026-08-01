<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
  finding: { type: Object, required: true },
})
const emit = defineEmits(['close'])

// Area comes from the layout — the one input that is hard to get and the
// reason this is worth doing here rather than in a spreadsheet. Everything
// else is the switching waveform, which only the designer knows.
const base = props.finding.emit
const current = ref(10)
const fsw = ref(500)          // kHz
const duty = ref(0.4)
const rise = ref(20)          // ns
const limit = ref('cispr32b')
const ground = ref(true)

const limits = JSON.parse(props.engine.limitLines())
const result = ref(null)
const error = ref('')

// The common-mode budget. The caveat below says cable common-mode current is
// what usually fails a product; this is the number that makes that actionable.
// It needs no unknowns — only the cable length and the standard.
const cable = ref(1.0)
const cm = ref(null)
function runCm() {
  try {
    const out = JSON.parse(props.engine.cmBudget(JSON.stringify({
      cableM: cable.value, limit: limit.value, groundReflection: ground.value,
    })))
    cm.value = out.error ? null : out
  } catch { cm.value = null }
}

function run() {
  try {
    const out = JSON.parse(props.engine.predictEmissions(JSON.stringify({
      areaMm2: base.areaMm2,
      currentA: current.value,
      fSwKhz: fsw.value,
      duty: duty.value,
      riseNs: rise.value,
      limit: limit.value,
      groundReflection: ground.value,
    })))
    if (out.error) { error.value = out.error; result.value = null; return }
    error.value = ''
    result.value = out
  } catch (e) {
    error.value = String(e)
  }
}
function onInput() { run(); runCm(); nextTick(draw) }

// ── the bridge to Hertz: a pre-hardware CONDUCTED estimate from the same
// trapezoid, handed to hertz.openconverters.com as a URL fragment — the data
// travels in the #hash, which never reaches any server. Bands are stated in
// the payload; this SEEDS a filter design, it does not replace a LISN.
const vBus = ref(48)
const cStrayPf = ref(50)
const cInUf = ref(10)
const hertzError = ref('')
function designInHertz() {
  hertzError.value = ''
  try {
    const est = JSON.parse(props.engine.conductedEstimate(JSON.stringify({
      currentA: current.value, fSwKhz: fsw.value, duty: duty.value,
      riseNs: rise.value, vBusV: vBus.value, cStrayF: cStrayPf.value * 1e-12,
      cInF: cInUf.value * 1e-6,
    })))
    if (est.error) { hertzError.value = est.error; return }
    const payload = {
      v: 1, source: 'faraday', fSwHz: fsw.value * 1e3,
      bands: est.bands, note: est.note, spectra: est.spectra,
    }
    const frag = btoa(unescape(encodeURIComponent(JSON.stringify(payload))))
    window.open('https://hertz.openconverters.com/#handoff=' + frag, '_blank',
                'noopener')
  } catch (e) { hertzError.value = String(e.message || e) }
}
watch([limit, ground], onInput)

// ---- the chart -----------------------------------------------------------
const canvas = ref(null)

function draw() {
  const cv = canvas.value
  const r = result.value
  if (!cv || !r) return
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  const padL = 46, padR = 10, padT = 12, padB = 26
  const f0 = 30, f1 = 1000                     // MHz, the regulated band
  // Frequency is logarithmic because the band spans a decade and a half and
  // the interesting structure sits at the bottom of it.
  const X = f => padL + (Math.log10(f / f0) / Math.log10(f1 / f0)) * (w - padL - padR)
  const lo = Math.min(0, r.worstLevelDbuvM - 15)
  const hi = Math.max(70, r.worstLevelDbuvM + 15)
  const Y = db => padT + (1 - (db - lo) / (hi - lo)) * (h - padT - padB)

  // grid
  ctx.font = '10px IBM Plex Mono, monospace'
  ctx.strokeStyle = 'rgba(157,180,173,0.13)'
  ctx.fillStyle = 'rgba(157,180,173,0.65)'
  ctx.lineWidth = 1
  for (const f of [30, 50, 100, 200, 300, 500, 1000]) {
    ctx.beginPath(); ctx.moveTo(X(f), padT); ctx.lineTo(X(f), h - padB); ctx.stroke()
    ctx.textAlign = 'center'
    ctx.fillText(String(f), X(f), h - padB + 13)
  }
  for (let db = Math.ceil(lo / 20) * 20; db <= hi; db += 20) {
    ctx.beginPath(); ctx.moveTo(padL, Y(db)); ctx.lineTo(w - padR, Y(db)); ctx.stroke()
    ctx.textAlign = 'right'
    ctx.fillText(String(db), padL - 6, Y(db) + 3)
  }
  ctx.textAlign = 'left'
  ctx.fillText('dBµV/m', 4, padT + 2)
  ctx.textAlign = 'center'
  ctx.fillText('MHz', (padL + w) / 2, h - 3)

  const path = (ys, colour, width, dash = []) => {
    ctx.strokeStyle = colour; ctx.lineWidth = width; ctx.setLineDash(dash)
    ctx.beginPath()
    for (let i = 0; i < r.fMhz.length; i++) {
      const x = X(r.fMhz[i]), y = Y(ys[i])
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
    }
    ctx.stroke(); ctx.setLineDash([])
  }

  // the line spectrum, drawn as the spikes it is
  ctx.strokeStyle = 'rgba(217,139,95,0.5)'
  ctx.lineWidth = 1
  ctx.beginPath()
  for (let i = 0; i < r.fMhz.length; i++) {
    const x = X(r.fMhz[i])
    ctx.moveTo(x, Y(lo)); ctx.lineTo(x, Y(r.lineDbuvM[i]))
  }
  ctx.stroke()

  path(r.envelopeDbuvM, '#d98b5f', 1.8)          // envelope — the honest bound
  path(r.limitDbuvM, '#ff5d5d', 2, [6, 4])       // the limit line

  // mark where the model stops being valid
  if (r.smallLoopMaxMhz < f1) {
    const x = X(Math.max(f0, r.smallLoopMaxMhz))
    ctx.fillStyle = 'rgba(16,22,19,0.55)'
    ctx.fillRect(x, padT, w - padR - x, h - padT - padB)
    ctx.strokeStyle = 'rgba(157,180,173,0.4)'
    ctx.setLineDash([3, 3])
    ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, h - padB); ctx.stroke()
    ctx.setLineDash([])
    ctx.fillStyle = 'rgba(157,180,173,0.8)'
    ctx.textAlign = 'left'
    ctx.fillText('loop no longer small', x + 5, padT + 12)
  }

  // the worst point
  ctx.fillStyle = '#ff5d5d'
  ctx.beginPath()
  ctx.arc(X(Math.max(f0, r.worstFMhz)), Y(r.worstLevelDbuvM), 3.5, 0, 7)
  ctx.fill()
}

const ro = new ResizeObserver(() => draw())
onMounted(() => {
  run()
  runCm()
  nextTick(() => { draw(); if (canvas.value) ro.observe(canvas.value) })
  window.addEventListener('keydown', onKey)
})
onBeforeUnmount(() => {
  ro.disconnect()
  window.removeEventListener('keydown', onKey)
})
function onKey(e) { if (e.key === 'Escape') emit('close') }

const num = (x, d = 1) => (x === undefined || x === null ? '—' : x.toFixed(d))
const r = computed(() => result.value)

// Above the edge knee the level is FLAT, so naming the single harmonic that
// happened to be scanned first implies an offending frequency that does not
// exist. When the worst point sits on the plateau, say so.
// Past a quarter wave the effective length is lambda/4, which falls as 1/f
// exactly as fast as the field rises with f — so the budget goes CONSTANT
// across the rest of the band. Naming one frequency inside that flat stretch
// implies a worst point that does not exist.
const cmWhere = computed(() => {
  const v = cm.value
  if (!v) return ''
  const flat = v.fMhz.filter((_, i) => v.budgetUa[i] <= v.tightestUa * 1.01)
  if (flat.length < 3) return `worst at ${num(v.tightestFMhz, 0)} MHz`
  const lo = Math.min(...flat), hi = Math.max(...flat)
  return hi / lo < 1.3 ? `worst at ${num(v.tightestFMhz, 0)} MHz`
                       : `flat across ${num(lo, 0)}\u2013${num(hi, 0)} MHz`
})

const where = computed(() => {
  const v = result.value
  if (!v) return ''
  const flat = v.kneeMhz < 30 && Math.abs(v.worstLevelDbuvM - v.plateauDbuvM) < 0.5
  return flat ? 'flat across the band below 230 MHz'
              : `at ${num(v.worstFMhz, 0)} MHz`
})
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="emissions" role="dialog"
             aria-label="Radiated emissions estimate">
      <header>
        <h2>EMISSIONS</h2>
        <span class="net">{{ base.net }}</span>
        <span class="area">{{ num(base.areaMm2, 0) }} mm² loop, measured off the copper</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <p v-if="error" class="err" data-testid="emissions-error">{{ error }}</p>

      <div v-if="r" class="body">
        <figure>
          <canvas ref="canvas" data-testid="emissions-chart" />
          <figcaption>
            <span class="k env">envelope</span>
            <span class="k line">harmonics</span>
            <span class="k lim">{{ r.limitLabel }} at {{ num(r.distanceM, 0) }} m</span>
          </figcaption>
        </figure>

        <div class="verdict" :class="r.level" data-testid="emissions-verdict">
          <div class="big">{{ r.worstMarginDb >= 0 ? '+' : '' }}{{ num(r.worstMarginDb) }}<small>dB</small></div>
          <div class="of">margin {{ where }} — {{ num(r.worstLevelDbuvM) }}
            against a {{ num(r.worstLevelDbuvM + r.worstMarginDb) }} dBµV/m limit</div>
          <p class="verd">{{ r.level === 'fail' ? 'Over the limit as estimated.'
            : r.level === 'watch' ? 'Under, but inside the noise of this estimate.'
            : 'Comfortably under, on this mechanism.' }}</p>
          <dl>
            <div><dt>plateau</dt><dd>{{ num(r.plateauDbuvM) }} dBµV/m</dd></div>
            <div><dt>edge knee</dt><dd>{{ num(r.kneeMhz) }} MHz</dd></div>
            <div><dt>small-loop to</dt><dd>{{ num(r.smallLoopMaxMhz, 0) }} MHz</dd></div>
          </dl>
          <p class="hint">The plateau above the knee is set by loop area, current
            and edge rate alone — halve any one of them for 6 dB.</p>
        </div>
      </div>

      <div v-if="r" class="caveat" data-testid="emissions-caveat">
        <b>Estimate, not a compliance prediction.</b> Differential-mode loop
        radiation only. It does <b>not</b> model common-mode current on attached
        cables, which dominates most real failures, nor enclosures, nor board
        resonances. A clean result here is not a pass — it means this loop is not
        your problem.
        <span v-if="r.harmonicsUnresolved" class="warn">
          At {{ num(fsw, 0) }} kHz several harmonics fall inside the receiver's
          120 kHz bandwidth, so a real measurement reads higher than any single
          line above.</span>
        <span v-if="r.beyondModelCount" class="warn">
          {{ r.beyondModelCount }} harmonics lie above {{ num(r.smallLoopMaxMhz, 0) }} MHz
          where the loop is no longer electrically small; they are drawn but not
          counted in the margin.</span>
      </div>

      <div v-if="cm" class="cmbudget" data-testid="cm-budget">
        <div class="cmhead">
          <h3>Common-mode budget</h3>
          <label class="cmlen">cable
            <input data-testid="cm-cable" type="range" min="0.1" max="5" step="0.1"
                   v-model.number="cable" @input="runCm" />
            <b>{{ num(cable, 1) }} m</b></label>
        </div>
        <p class="cmbig">
          <b data-testid="cm-tightest">{{ cm.tightestUa < 1 ? cm.tightestUa.toFixed(2)
            : cm.tightestUa.toFixed(1) }} µA</b>
          is all the common-mode current a {{ num(cm.cableM, 1) }} m cable may carry
          and still meet {{ cm.limitLabel }} at {{ num(cm.distanceM, 0) }} m —
          {{ cmWhere }}.
        </p>
        <p class="cmnote">Microamps, not milliamps: an ordinary current probe cannot
          see this. Faraday cannot predict your actual common-mode current — it comes
          from ground-plane impedance and return-path detours, not from geometry — but
          this is the number it has to stay under. Above
          {{ num(cm.quarterWaveMhz, 0) }} MHz the cable is longer than a quarter wave
          and stops counting as longer still.</p>
      </div>

      <div class="controls">
        <label class="sl"><span>switched current <b>{{ num(current) }} A</b></span>
          <input data-testid="emissions-current" type="range" min="0.1" max="60" step="0.1"
                 v-model.number="current" @input="onInput" /></label>
        <label class="sl"><span>switching <b>{{ num(fsw, 0) }} kHz</b></span>
          <input data-testid="emissions-fsw" type="range" min="20" max="3000" step="10"
                 v-model.number="fsw" @input="onInput" /></label>
        <label class="sl"><span>edge <b>{{ num(rise) }} ns</b></span>
          <input data-testid="emissions-rise" type="range" min="1" max="200" step="1"
                 v-model.number="rise" @input="onInput" /></label>
        <label class="sl"><span>duty <b>{{ num(duty, 2) }}</b></span>
          <input type="range" min="0.05" max="0.95" step="0.01"
                 v-model.number="duty" @input="onInput" /></label>
        <label class="pick"><span>standard</span>
          <select v-model="limit" data-testid="emissions-limit">
            <option v-for="l in limits" :key="l.id" :value="l.id">{{ l.label }}</option>
          </select></label>
        <label class="chk"><input type="checkbox" v-model="ground" />
          <span>ground-plane reflection (+6 dB)</span></label>
      </div>

      <div class="hertz-bridge" data-testid="hertz-bridge">
        <p class="k line">conducted → filter design</p>
        <p class="note">The same switching waveform, driven into the two paths a LISN
          measures: DM through the input-capacitor branch, CM through an <em>assumed</em>
          stray capacitance to earth. A seeding estimate (DM ±10 dB, CM ±15 dB) — enough
          to design the line filter <b>before hardware exists</b>; verify with a LISN.</p>
        <div class="controls">
          <label class="sl"><span>bus voltage <b>{{ num(vBus, 0) }} V</b></span>
            <input data-testid="bridge-vbus" type="range" min="5" max="800" step="1"
                   v-model.number="vBus" /></label>
          <label class="sl"><span>C_stray to earth <b>{{ num(cStrayPf, 0) }} pF</b> (assumed)</span>
            <input data-testid="bridge-cstray" type="range" min="5" max="500" step="5"
                   v-model.number="cStrayPf" /></label>
          <label class="sl"><span>input capacitor <b>{{ num(cInUf, 0) }} µF</b></span>
            <input data-testid="bridge-cin" type="range" min="0.1" max="200" step="0.1"
                   v-model.number="cInUf" /></label>
        </div>
        <button class="hbtn" data-testid="design-in-hertz" @click="designInHertz">
          design the input filter in Hertz →</button>
        <span v-if="hertzError" class="warn" data-testid="hertz-bridge-error">{{ hertzError }}</span>
        <p class="note">Opens hertz.openconverters.com with the predicted CM/DM spectra in the
          URL <b>fragment</b> — the part of a URL that never leaves your browser. Hertz judges
          them against the limit, designs the filter, and can generate the filter's own PCB.</p>
      </div>
    </section>
  </div>
</template>

<style scoped>
.hertz-bridge {
  margin-top: 14px; padding-top: 10px; border-top: 1px solid var(--resin-edge, #2a3a32);
}
.hertz-bridge .hbtn {
  margin-top: 6px; padding: 7px 14px; cursor: pointer;
  background: none; color: #58c79a; border: 1px solid #2a5a46; border-radius: 5px;
  font: 600 13px/1 var(--mono, monospace); letter-spacing: .03em;
}
.hertz-bridge .hbtn:hover { border-color: #58c79a; }
.scrim {
  position: fixed; inset: 0; z-index: 50;
  background: rgba(8, 12, 10, 0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px;
}
.panel {
  width: min(1060px, 100%); max-height: 100%;
  display: flex; flex-direction: column; overflow: auto;
  background: var(--resin); border: 1px solid var(--resin-edge); border-radius: 8px;
}
header { display: flex; align-items: baseline; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper); }
.net { font-family: var(--mono); font-size: 13px; }
.area { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }
.err { margin: 12px 16px; padding: 10px 12px; border-radius: 4px;
  background: #3a1a1e; color: #ffb3b8; font-family: var(--mono); font-size: 12.5px; }

.body { display: grid; gap: 14px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1.6fr) minmax(0, 1fr); }
@media (max-width: 820px) { .body { grid-template-columns: 1fr; } }
canvas { width: 100%; height: 300px; display: block; border-radius: 4px; background: #0d1210; }
figcaption { display: flex; gap: 14px; flex-wrap: wrap; padding-top: 6px;
  font-family: var(--mono); font-size: 11px; color: var(--tin); }
.k::before { content: '—'; margin-right: 4px; font-weight: 700; }
.k.env { color: var(--copper); }
.k.line { color: rgba(217,139,95,0.6); }
.k.lim { color: var(--heat-high); }

.verdict { align-self: start; border: 1px solid var(--resin-edge); border-left-width: 3px;
  border-radius: 4px; padding: 14px; display: flex; flex-direction: column; gap: 8px; }
.verdict.ok { border-left-color: var(--heat-low); }
.verdict.watch { border-left-color: var(--heat-med); }
.verdict.fail { border-left-color: var(--heat-high); }
.big { font-family: var(--display); font-size: 44px; font-weight: 700; line-height: 1; }
.big small { font-size: 17px; font-weight: 500; color: var(--tin); margin-left: 4px; }
.of { font-size: 12.5px; color: var(--tin); }
.verd { font-size: 13px; }
.verdict dl { display: grid; gap: 2px; font-family: var(--mono); font-size: 11.5px; }
.verdict dl > div { display: flex; justify-content: space-between; gap: 10px;
  border-bottom: 1px dotted var(--resin-edge); padding: 3px 0; }
.verdict dt { color: var(--tin); }
.hint { font-size: 11.5px; color: var(--tin); }

.caveat { margin: 0 16px 12px; padding: 10px 12px; border-radius: 4px;
  background: var(--bare-fr4); border: 1px solid var(--resin-edge);
  font-size: 12px; color: var(--tin); }
.caveat b { color: var(--silk); }
.caveat .warn { display: block; margin-top: 6px; color: var(--heat-med); }

.cmbudget {
  margin: 0 16px 12px; padding: 11px 13px; border-radius: 4px;
  background: var(--bare-fr4); border: 1px solid var(--resin-edge);
  border-left: 3px solid var(--heat-med);
}
.cmhead { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
.cmhead h3 { font-family: var(--display); font-size: 13.5px; font-weight: 700;
  letter-spacing: 0.09em; text-transform: uppercase; color: var(--tin); }
.cmlen { display: flex; align-items: center; gap: 7px; font-size: 11.5px; color: var(--tin); }
.cmlen input { width: 110px; accent-color: var(--copper); }
.cmlen b { font-family: var(--mono); color: var(--silk); }
.cmbig { margin-top: 6px; font-size: 13px; }
.cmbig b { font-family: var(--mono); font-size: 17px; color: var(--heat-med); }
.cmnote { margin-top: 5px; font-size: 11.5px; color: var(--tin); }

.controls { display: grid; gap: 10px 18px; padding: 12px 16px;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  border-top: 1px solid var(--resin-edge); background: var(--bare-fr4); }
.sl, .pick { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b, .pick span { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: var(--copper); }
.pick select { background: var(--resin); color: var(--silk); font-family: var(--mono);
  font-size: 12px; border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px; }
.chk { display: flex; align-items: center; gap: 7px; font-size: 11.5px; color: var(--tin); }
.chk input { accent-color: var(--copper); }
</style>
