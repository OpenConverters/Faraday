<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick, inject } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
  finding: { type: Object, required: true },
  // Every square millimetre of copper that swings with the switching edge,
  // summed off THIS layout by the screener. It is the plate that drives
  // common-mode current into the chassis, and it is why the conducted estimate
  // no longer has to ask anyone to invent a stray capacitance.
  dvdtAreaMm2: { type: Number, default: 0 },
  // An operating point handed over by whatever DESIGNED the converter
  // (Kirchhoff/Heaviside), through the URL fragment — the same private route
  // the Hertz bridge uses in the other direction. Null when nobody said.
  operatingPoint: { type: Object, default: null },
})
const basic = inject('basic', computed(() => false))
// The panel sits on a scrim, so the header toggle is unreachable while it is
// open. Guided mode is worth nothing if "show me the numbers" is behind it.
const view = inject('view', null)
// NOTE: a ref reached through inject is auto-unwrapped in the TEMPLATE, so
// `view.value = ...` written there assigns to a string and silently does
// nothing. The setter belongs in script, where `view` is still the ref.
function switchView() { if (view) view.value = basic.value ? 'advanced' : 'guided' }
const emit = defineEmits(['close'])

// Area comes from the layout — the one input that is hard to get and the
// reason this is worth doing here rather than in a spreadsheet. Everything
// else is the switching waveform, which only the designer knows.
const base = props.finding.emit
// The waveform: from the design if a tool handed one over, otherwise the
// guided preset's numbers. Either way the panel PRINTS what it is using —
// an assumption you cannot see is what makes a tool feel generic.
const op = props.operatingPoint
const current = ref(op?.currentA ?? 10)
const fsw = ref(op?.fSwKhz ?? 500)          // kHz
const duty = ref(op?.duty ?? 0.4)
const rise = ref(op?.riseNs ?? 20)          // ns
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
function onInput() { run(); runCm(); runConducted(); nextTick(() => { draw(); drawConducted() }) }

// ── CONDUCTED: the other half of the answer ───────────────────────────────
// The same trapezoid driven into the two paths a LISN measures, judged against
// the conducted limit line HERE — which mode dominates, where in frequency it
// fails, and how many dB each filter stage has to find. It used to travel
// straight to Hertz as a pair of unjudged curves; a curve with no limit line
// cannot tell anyone whether they have a problem.
//
// The payload still goes to hertz.openconverters.com in the URL FRAGMENT — the
// part of a URL that never reaches any server — for the actual filter
// synthesis (choke, X and Y capacitors, real part numbers).
const vBus = ref(op?.vBusV ?? 48)
const cInUf = ref(10)
// The input-capacitor branch, derived from the board's own parts (ABT #797).
// The DM path of the conducted estimate IS this impedance, so as long as the
// board can supply it the slider has no business being the source.
const branch = ref(null)
const useBoardCin = ref(true)
function loadBranch() {
  try {
    const out = JSON.parse(props.engine.inputBranch())
    branch.value = out.error || !out.derived ? null : out
  } catch { branch.value = null }
}
const branchLh = computed(() => branch.value
  ? branch.value.caps.map(c => ({ cF: c.cF, lH: c.eslH + c.lMountH })) : null)
const derivedCin = computed(() => !!(branch.value && useBoardCin.value))
// C_stray: DERIVED from the dv/dt copper this board actually has, at a stated
// mounting distance. The area is measured; the gap is the one thing a layout
// file cannot carry, so it is the only thing left to ask for. Boards with no
// switching copper (no converter) fall back to an explicitly stated value.
const chassisGapMm = ref(10)
const mounting = ref('air')     // 'air' = spaced off a chassis; 'laminate' = bolted against it
const cStrayPf = ref(50)        // only when there is no dv/dt copper to derive from
const conductedLimit = ref('cispr32b-qp')
const conductedLimits = JSON.parse(props.engine.conductedLimits())
const conducted = ref(null)
const hertzError = ref('')
const derivable = computed(() => props.dvdtAreaMm2 > 0)
const epsR = computed(() => (mounting.value === 'laminate' ? 4.5 : 1.0))

function conductedRequest() {
  const req = {
    currentA: current.value, fSwKhz: fsw.value, duty: duty.value,
    riseNs: rise.value, vBusV: vBus.value, cInF: cInUf.value * 1e-6,
    limit: conductedLimit.value,
  }
  if (derivable.value) {
    req.dvdtAreaMm2 = props.dvdtAreaMm2
    req.chassisGapMm = chassisGapMm.value
    req.chassisEpsR = epsR.value
  } else {
    req.cStrayF = cStrayPf.value * 1e-12
  }
  if (derivedCin.value) req.inputBranches = branchLh.value
  return req
}

function runConducted() {
  hertzError.value = ''
  try {
    const out = JSON.parse(props.engine.conductedEstimate(
      JSON.stringify(conductedRequest())))
    if (out.error) { hertzError.value = out.error; conducted.value = null; return }
    conducted.value = out
  } catch (e) {
    hertzError.value = String(e.message || e)
    conducted.value = null
  }
}

function designInHertz() {
  if (!conducted.value) { runConducted(); if (!conducted.value) return }
  const est = conducted.value
  const payload = {
    v: 1, source: 'faraday', fSwHz: fsw.value * 1e3,
    bands: est.bands, note: est.note, spectra: est.spectra,
  }
  const frag = btoa(unescape(encodeURIComponent(JSON.stringify(payload))))
  window.open('https://hertz.openconverters.com/#handoff=' + frag, '_blank',
              'noopener')
}

// ── a SIMULATED run, read back (ABT #809) ─────────────────────────────────
// The trapezoid below is a seed: an ideal waveform driven into the board's own
// impedances. A simulation of THIS board — its parasitics, a real device model,
// a LISN — is a second, better-founded source for the same two curves, and the
// panel shows both rather than choosing. Where they disagree by more than the
// seed's stated bands, that disagreement is information about the models and
// the panel says so; averaging them would destroy it.
const simulated = ref(null)
const simError = ref('')
function loadSimulated(ev) {
  const file = ev.target.files?.[0]
  ev.target.value = ''
  if (!file) return
  const r = new FileReader()
  r.onload = () => {
    try {
      const p = JSON.parse(String(r.result))
      if (p.v !== 1 || !p.spectra?.dm || !p.spectra?.cm)
        throw new Error('not a simulated-run export (expected v:1 with dm/cm spectra)')
      simulated.value = p
      simError.value = ''
      nextTick(drawConducted)
    } catch (e) {
      simulated.value = null
      simError.value = 'could not read that run: ' + String(e.message || e)
    }
  }
  r.readAsText(file)
}
// The two sources, at the frequency where the seed is worst. A gap wider than
// the seed's own band (DM ±10 dB, CM ±15 dB) is worth a sentence.
const simVsSeed = computed(() => {
  const v = cv.value, p = simulated.value
  if (!v || !p) return null
  const mode = v.worstMode === 'CM' ? 'cm' : 'dm'
  const band = mode === 'cm' ? 15 : 10
  const fHz = v.worstFMhz * 1e6
  let best = null
  for (const [f, l] of p.spectra[mode])
    if (!best || Math.abs(Math.log10(f / fHz)) < Math.abs(Math.log10(best[0] / fHz)))
      best = [f, l]
  if (!best) return null
  return { mode: v.worstMode, seed: v.worstLevelDbuv, sim: best[1],
           fMhz: best[0] / 1e6, band,
           disagrees: Math.abs(best[1] - v.worstLevelDbuv) > band }
})

const cv = computed(() => conducted.value?.verdict || null)
const cStrayPfShown = computed(() =>
  conducted.value ? conducted.value.cStrayF * 1e12 : 0)

// One sentence, in the words the answer is actually worth: which mode, where,
// and by how much. This is the line the whole conducted section exists for.
const conductedLine = computed(() => {
  const v = cv.value
  if (!v) return ''
  const mode = v.worstMode === 'CM' ? 'common-mode' : 'differential-mode'
  const at = v.worstFMhz < 1 ? `${(v.worstFMhz * 1000).toFixed(0)} kHz`
                             : `${v.worstFMhz.toFixed(2)} MHz`
  return v.worstMarginDb < 0
    ? `${Math.abs(v.worstMarginDb).toFixed(0)} dB OVER the limit at ${at}, and it is ${mode} noise`
    : `${v.worstMarginDb.toFixed(0)} dB under the limit, worst at ${at} (${mode})`
})

// ── guided presets ────────────────────────────────────────────────────────
// Four switching waveforms that cover most of what people bring here. Guided
// mode picks one instead of asking for four numbers nobody has to hand; the
// choice is shown, never hidden, and advanced mode still types its own.
const PRESETS = [
  { id: 'pol', label: '3 A point-of-load', sub: '12 V, 500 kHz, 10 ns edges',
    currentA: 3, fSwKhz: 500, riseNs: 10, duty: 0.4, vBusV: 12 },
  { id: 'buck', label: '10 A buck', sub: '48 V, 500 kHz, 20 ns edges',
    currentA: 10, fSwKhz: 500, riseNs: 20, duty: 0.4, vBusV: 48 },
  { id: 'gan', label: 'fast GaN stage', sub: '48 V, 1 MHz, 5 ns edges',
    currentA: 10, fSwKhz: 1000, riseNs: 5, duty: 0.4, vBusV: 48 },
  { id: 'offline', label: 'offline flyback', sub: '400 V, 100 kHz, 50 ns edges',
    currentA: 2, fSwKhz: 100, riseNs: 50, duty: 0.35, vBusV: 400 },
]
const preset = ref('buck')
function applyPreset(p) {
  preset.value = p.id
  current.value = p.currentA; fsw.value = p.fSwKhz
  rise.value = p.riseNs; duty.value = p.duty; vBus.value = p.vBusV
  onInput()
}

watch([limit, ground], onInput)
watch([vBus, cInUf, chassisGapMm, mounting, cStrayPf, conductedLimit, useBoardCin],
      () => { runConducted(); nextTick(drawConducted) })


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

// The conducted chart: both modes and the limit line on one set of axes,
// 150 kHz to 30 MHz. Two curves rather than one is the entire point — the
// question "is this common mode or differential mode" is answered by which of
// them is on top, and no single summed curve can answer it.
const ccanvas = ref(null)

function drawConducted() {
  const el = ccanvas.value
  const v = cv.value
  if (!el || !v || !v.fMhz.length) return
  const dpr = window.devicePixelRatio || 1
  const w = el.clientWidth, h = el.clientHeight
  if (!w || !h) return
  el.width = w * dpr; el.height = h * dpr
  const ctx = el.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  const padL = 46, padR = 10, padT = 20, padB = 26
  const f0 = 0.15, f1 = 30                       // MHz, the conducted band
  const X = f => padL + (Math.log10(f / f0) / Math.log10(f1 / f0)) * (w - padL - padR)
  const all = [...v.dmDbuv, ...v.cmDbuv, ...v.limitDbuv]
  // The comb's sinc nulls run to -140 dBuV and there is nothing to read down
  // there; letting them set the axis squeezes the whole decision — the two
  // curves against the limit — into the top fifth of the plot. Floor the axis
  // 40 dB under the limit, clip the paths to the plot rectangle, and SAY that
  // the nulls run off the bottom rather than quietly flattening them onto it.
  const limLo = Math.min(...v.limitDbuv)
  const lo = Math.floor((limLo - 40) / 20) * 20
  const hi = Math.ceil((Math.max(...all) + 5) / 20) * 20
  const clipped = Math.min(...all) < lo
  const Y = db => padT + (1 - (db - lo) / (hi - lo)) * (h - padT - padB)

  ctx.font = '10px IBM Plex Mono, monospace'
  ctx.strokeStyle = 'rgba(157,180,173,0.13)'
  ctx.fillStyle = 'rgba(157,180,173,0.65)'
  ctx.lineWidth = 1
  for (const f of [0.15, 0.3, 0.5, 1, 3, 5, 10, 30]) {
    ctx.beginPath(); ctx.moveTo(X(f), padT); ctx.lineTo(X(f), h - padB); ctx.stroke()
    ctx.textAlign = 'center'
    ctx.fillText(f < 1 ? String(f * 1000) : String(f), X(f), h - padB + 13)
  }
  for (let db = lo; db <= hi; db += 20) {
    ctx.beginPath(); ctx.moveTo(padL, Y(db)); ctx.lineTo(w - padR, Y(db)); ctx.stroke()
    ctx.textAlign = 'right'
    ctx.fillText(String(db), padL - 6, Y(db) + 3)
  }
  ctx.textAlign = 'left'
  ctx.fillText('dBµV', 4, 10)          // above the plot: the top gridline's
                                       // own label lives at x = padL - 6
  ctx.textAlign = 'center'
  ctx.fillText('kHz | MHz', (padL + w) / 2, h - 3)

  const path = (ys, colour, width, dash = []) => {
    ctx.strokeStyle = colour; ctx.lineWidth = width; ctx.setLineDash(dash)
    ctx.beginPath()
    for (let i = 0; i < v.fMhz.length; i++) {
      const x = X(v.fMhz[i]), y = Y(ys[i])
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
    }
    ctx.stroke(); ctx.setLineDash([])
  }
  ctx.save()
  ctx.beginPath()
  ctx.rect(padL, padT, w - padL - padR, h - padT - padB)
  ctx.clip()
  path(v.dmDbuv, '#6f9fc4', 1.6)                 // differential mode
  path(v.cmDbuv, '#d98b5f', 1.6)                 // common mode
  path(v.limitDbuv, '#ff5d5d', 2, [6, 4])        // the limit

  // the simulated run, on its own frequency base — drawn thinner and dashed,
  // because it is a different KIND of number and must not be mistaken for the
  // seed it is being compared against
  const sim = simulated.value
  if (sim) {
    const simPath = (pts, colour) => {
      ctx.strokeStyle = colour
      ctx.lineWidth = 1.2
      ctx.setLineDash([3, 3])
      ctx.beginPath()
      let started = false
      for (const [f, lv] of pts) {
        const mhz = f / 1e6
        if (mhz < f0 || mhz > f1) continue
        const x = X(mhz), y = Y(lv)
        started ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
        started = true
      }
      ctx.stroke()
      ctx.setLineDash([])
    }
    simPath(sim.spectra.dm, '#9fd0ef')
    simPath(sim.spectra.cm, '#ffc79a')
  }

  // the worst point of the dominant mode — the frequency to quote
  ctx.fillStyle = '#ff5d5d'
  ctx.beginPath()
  ctx.arc(X(Math.min(Math.max(f0, v.worstFMhz), f1)),
          Y(v.worstLevelDbuv), 3.5, 0, 7)
  ctx.fill()
  ctx.restore()

  if (clipped) {
    ctx.fillStyle = 'rgba(157,180,173,0.7)'
    ctx.textAlign = 'right'
    ctx.fillText('nulls run below the axis', w - padR, h - padB - 4)
  }
}

const ro = new ResizeObserver(() => { draw(); drawConducted() })
onMounted(() => {
  run()
  runCm()
  loadBranch()
  runConducted()
  nextTick(() => {
    draw(); drawConducted()
    if (canvas.value) ro.observe(canvas.value)
    if (ccanvas.value) ro.observe(ccanvas.value)
  })
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
        <button v-if="view" class="vtog" data-testid="panel-view-toggle"
                @click="switchView">
          {{ basic ? 'show the numbers' : 'plain language' }}</button>
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
          <p v-if="basic" class="verd" data-testid="verdict-plain">{{
            r.level === 'fail'
              ? 'As drawn, this loop alone would be over the radiated limit.'
              : r.level === 'watch'
                ? 'Just under the limit — close enough that a real board could go either way.'
                : 'Well under the limit, for this loop.' }}</p>
          <p v-else class="verd">{{ r.level === 'fail' ? 'Over the limit as estimated.'
            : r.level === 'watch' ? 'Under, but inside the noise of this estimate.'
            : 'Comfortably under, on this mechanism.' }}</p>
          <dl v-if="!basic">
            <div><dt>plateau</dt><dd>{{ num(r.plateauDbuvM) }} dBµV/m</dd></div>
            <div><dt>edge knee</dt><dd>{{ num(r.kneeMhz) }} MHz</dd></div>
            <div><dt>small-loop to</dt><dd>{{ num(r.smallLoopMaxMhz, 0) }} MHz</dd></div>
          </dl>
          <p class="hint">{{ basic
            ? 'Three things move this number: the loop area, the current in it, and how fast the switch turns on. Halve any one of them and you gain 6 dB.'
            : 'The plateau above the knee is set by loop area, current and edge rate alone — halve any one of them for 6 dB.' }}</p>
        </div>
      </div>

      <div v-if="r && basic" class="caveat" data-testid="caveat-plain">
        <b>This is an estimate, not a test report.</b> It covers the noise this
        one loop radiates. It does not cover noise carried out on your cables,
        your enclosure, or anything ringing on the board — and cable noise is
        what fails most products. A clean result here means this loop is not
        your problem, not that the product passes.
      </div>

      <div v-if="r && !basic" class="caveat" data-testid="emissions-caveat">
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

      <div v-if="cm && !basic" class="cmbudget" data-testid="cm-budget">
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

      <!-- Guided: pick the converter, don't type its waveform. The four
           numbers behind the choice are printed underneath it, because an
           assumption you cannot see is exactly what makes a tool feel
           "generic". -->
      <div v-if="basic" class="presets" data-testid="emissions-presets">
        <p class="plabel">What is switching here?</p>
        <div class="prow">
          <button v-for="p in PRESETS" :key="p.id" class="pchip"
                  :class="{ on: preset === p.id }" :data-testid="`preset-${p.id}`"
                  @click="applyPreset(p)">
            {{ p.label }}<small>{{ p.sub }}</small></button>
        </div>
        <p class="passumed" data-testid="preset-assumed">
          Assuming <b>{{ num(current) }} A</b> switched at <b>{{ num(fsw, 0) }} kHz</b>
          with <b>{{ num(rise) }} ns</b> edges, off a <b>{{ num(vBus, 0) }} V</b> bus —
          the loop area ({{ num(base.areaMm2, 0) }} mm²) is measured off your copper.
          Switch to <b>advanced</b> in the header to type your own.</p>
      </div>

      <div v-if="!basic" class="controls">
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

      <!-- ── CONDUCTED ────────────────────────────────────────────────────
           Below 30 MHz nothing radiates off a board this size; it walks out on
           the wires, and it walks out as two different problems that need two
           different components. Which one you have is the first question, and
           it is answered here rather than one site away. -->
      <div class="conducted" data-testid="conducted">
        <div class="chead">
          <h3>{{ basic ? 'Noise going out on the wires' : 'Conducted emissions (150 kHz – 30 MHz)' }}</h3>
          <label class="pick"><span>standard</span>
            <select v-model="conductedLimit" data-testid="conducted-limit">
              <option v-for="l in conductedLimits" :key="l.id" :value="l.id">{{ l.label }}</option>
            </select></label>
        </div>

        <p v-if="hertzError" class="warn" data-testid="conducted-error">{{ hertzError }}</p>

        <template v-if="cv">
          <p class="cbig" :class="cv.level" data-testid="conducted-verdict">
            <b>{{ conductedLine }}</b></p>
          <figure class="cfig">
            <canvas ref="ccanvas" data-testid="conducted-chart" />
            <figcaption>
              <span class="k dm">differential mode</span>
              <span class="k cmm">common mode</span>
              <span class="k lim">{{ cv.limitLabel }}</span>
            </figcaption>
          </figure>

          <p class="cwhich" data-testid="conducted-which">
            <template v-if="basic">
              {{ cv.worstMode === 'CM'
                ? 'Common mode is the bigger problem here: noise leaving on ALL the wires together and coming back through earth. That is what a common-mode choke and the Y capacitors are for — a bigger input capacitor will not touch it.'
                : 'Differential mode is the bigger problem here: noise going out on one wire and back on the other. That is what the X capacitor and the choke\'s leakage inductance are for.' }}
              <template v-if="cv.cmCrossoverMhz > 0">
                Above {{ num(cv.cmCrossoverMhz, 2) }} MHz it is common mode from there up.</template>
            </template>
            <template v-else>
              CM leads at {{ (cv.cmDominantFraction * 100).toFixed(0) }}% of the band<template
                v-if="cv.cmCrossoverMhz > 0"> and from {{ num(cv.cmCrossoverMhz, 2) }} MHz upward</template>.
              Worst DM margin {{ num(cv.dmWorstMarginDb) }} dB at {{ num(cv.dmWorstFMhz, 2) }} MHz;
              worst CM margin {{ num(cv.cmWorstMarginDb) }} dB at {{ num(cv.cmWorstFMhz, 2) }} MHz.
              Each mode is judged against the full limit line, which is how a filter stage is
              sized (ANP015); the line voltage carries both, and equal modes sum only 6 dB
              above either.
            </template>
          </p>

          <dl v-if="cv.designFCovered" class="creq" data-testid="conducted-required">
            <div><dt>CM stage needs</dt>
              <dd>{{ cv.requiredCmDb > 0 ? num(cv.requiredCmDb, 0) + ' dB' : 'nothing' }}</dd></div>
            <div><dt>DM stage needs</dt>
              <dd>{{ cv.requiredDmDb > 0 ? num(cv.requiredDmDb, 0) + ' dB' : 'nothing' }}</dd></div>
            <div><dt>at</dt><dd>{{ cv.designFMhz < 1
              ? num(cv.designFMhz * 1000, 0) + ' kHz' : num(cv.designFMhz, 2) + ' MHz' }}</dd></div>
            <div v-if="!basic"><dt>incl. margin</dt><dd>{{ num(cv.designMarginDb, 0) }} dB</dd></div>
          </dl>
          <p v-else class="cwhich warn" data-testid="conducted-uncovered">
            This standard only regulates protected broadcast bands, and
            {{ cv.designFMhz < 1 ? num(cv.designFMhz * 1000, 0) + ' kHz'
                                 : num(cv.designFMhz, 2) + ' MHz' }} — where this
            converter's filter would be designed — is not inside one. No
            attenuation figure is quoted here rather than one against a limit
            that does not exist at that frequency. The bands that ARE measured
            are still judged above.
          </p>
          <p v-if="cv.automotiveLisn" class="cwhich warn" data-testid="conducted-lisn">
            <b>Read the DM curve with care against an automotive line.</b> The
            differential-mode path here is modelled against the 50 µH / 100 Ω
            mains artificial network; a CISPR 25 measurement uses a 5 µH LISN,
            whose impedance below a few megahertz is much lower. The common-mode
            path and the limit line are right; the DM level is the mains case.
          </p>
        </template>

        <!-- A simulated run of THIS board, read back (ABT #809). -->
        <div class="simrow" data-testid="sim-row">
          <label class="simbtn">
            <input type="file" accept=".json,application/json"
                   data-testid="sim-input" @change="loadSimulated" />
            {{ simulated ? 'replace the simulated run…' : 'load a simulated run…' }}
          </label>
          <span v-if="simError" class="warn" data-testid="sim-error">{{ simError }}</span>
          <template v-else-if="simulated">
            <span class="k dm">DM simulated</span><span class="k cmm">CM simulated</span>
            <span class="simnote" data-testid="sim-note">
              <template v-if="basic">
                These dashed curves come from simulating <b>your own board</b> —
                its measured parasitics with a real switching device — instead of
                an ideal waveform. Closer to the truth, and still not a
                measurement.
              </template>
              <template v-else>
                <b>Simulated</b>, not measured: {{ simulated.note }}
              </template>
            </span>
            <span v-if="simVsSeed" class="simnote"
                  :class="{ warn: simVsSeed.disagrees }" data-testid="sim-vs-seed">
              At {{ num(simVsSeed.fMhz, 2) }} MHz the seed reads
              {{ num(simVsSeed.seed) }} dBµV and the simulation
              {{ num(simVsSeed.sim) }} dBµV<template v-if="simVsSeed.disagrees">
                — further apart than the seed's own ±{{ simVsSeed.band }} dB band,
                so one of the two models is wrong about this board and the
                difference is worth chasing rather than averaging</template><template
                v-else> — inside the seed's stated ±{{ simVsSeed.band }} dB, so
                the two independent paths agree</template>.
            </span>
          </template>
        </div>

        <!-- The differential-mode source term: the board's own capacitors.
             The DM curve is this branch's impedance, so where the layout can
             supply it, a slider asking for "input capacitor" is the tool
             ignoring what it can already see. -->
        <div class="cstray" data-testid="input-branch">
          <p v-if="branch" class="note" data-testid="branch-derived">
            <b>Input capacitors, off this board:</b>
            {{ branch.caps.map(c => c.ref).join(' + ') }} on
            <b>{{ branch.rail }}</b> —
            <b>{{ branch.cF >= 1e-6 ? (branch.cF * 1e6).toFixed(1) + ' µF'
                 : (branch.cF * 1e9).toFixed(0) + ' nF' }}</b> total, branch
            inductance <b>{{ (branch.lH * 1e9).toFixed(2) }} nH</b> of which
            {{ (branch.lMountShare * 100).toFixed(0) }}% is the MOUNTING, measured
            pad-to-via on your copper. The commutation loop named
            {{ branch.loopCapRef }}, which is what identified the rail.
            <template v-if="branch.unparsed"> {{ branch.unparsed }} capacitor(s)
              on this rail had a value this refuses to guess at, and are left
              out rather than invented.</template>
            <b>ESR is not on the board</b> — every branch carries the model's
            stated 15 mΩ, and a large electrolytic's real ESR is several times
            that, which makes the DM level here optimistic where the bulk
            dominates.
            <label class="inl"><input type="checkbox" v-model="useBoardCin"
                     data-testid="use-board-cin" /> use it</label>
          </p>
          <p v-else class="note" data-testid="branch-stated">
            No input-capacitor branch could be derived from this board (no switch
            node with a derived commutation loop, or no parseable capacitor on its
            rail), so the differential-mode path uses the value you state below.
          </p>
        </div>

        <!-- The common-mode source term. Derived where the board can supply it:
             the plate is the switching copper, measured; only the distance to
             the metalwork is asked for, because a layout file cannot carry it. -->
        <div class="cstray" data-testid="cstray">
          <p v-if="derivable" class="note" data-testid="cstray-derived">
            <b>{{ num(cStrayPfShown, cStrayPfShown < 10 ? 2 : 0) }} pF</b> to the chassis,
            <b>derived</b> from the <b>{{ num(dvdtAreaMm2, 0) }} mm²</b> of switching copper
            on this board at the mounting below. That capacitance is what turns dV/dt into
            common-mode current — a lower bound (fringing only adds, and a heatsink on the
            device tab or a transformer's inter-winding capacitance add paths this cannot see).
          </p>
          <p v-else class="note" data-testid="cstray-stated">
            No switching copper was identified on this board, so the stray capacitance to
            earth cannot be derived from it — state one. It is the term the whole
            common-mode estimate stands on, so it is asked for rather than assumed.
          </p>
          <div class="controls">
            <label v-if="derivable" class="sl"><span>gap to chassis / heatsink
                <b>{{ num(chassisGapMm, 1) }} mm</b></span>
              <input data-testid="bridge-gap" type="range" min="0.5" max="50" step="0.5"
                     v-model.number="chassisGapMm" /></label>
            <label v-if="derivable" class="pick"><span>mounting</span>
              <select v-model="mounting" data-testid="bridge-mounting">
                <option value="air">spaced off it (air)</option>
                <option value="laminate">bolted against it (through the laminate)</option>
              </select></label>
            <label v-else class="sl"><span>C_stray to earth <b>{{ num(cStrayPf, 0) }} pF</b> (stated)</span>
              <input data-testid="bridge-cstray" type="range" min="5" max="500" step="5"
                     v-model.number="cStrayPf" /></label>
            <label class="sl"><span>bus voltage <b>{{ num(vBus, 0) }} V</b></span>
              <input data-testid="bridge-vbus" type="range" min="5" max="800" step="1"
                     v-model.number="vBus" /></label>
            <label v-if="!basic && !derivedCin" class="sl"><span>input capacitor <b>{{ num(cInUf, 0) }} µF</b></span>
              <input data-testid="bridge-cin" type="range" min="0.1" max="200" step="0.1"
                     v-model.number="cInUf" /></label>
          </div>
        </div>

        <button class="hbtn" data-testid="design-in-hertz" @click="designInHertz">
          {{ basic ? 'design the filter that fixes this →' : 'design the input filter in Hertz →' }}</button>
        <span v-if="hertzError" class="warn" data-testid="hertz-bridge-error">{{ hertzError }}</span>
        <p class="note">{{ basic
          ? 'Opens Hertz with these two curves already loaded, and it picks the choke and the capacitors. The curves travel inside the link itself — they never reach a server.'
          : 'Opens hertz.openconverters.com with the predicted CM/DM spectra in the URL fragment — the part of a URL that never leaves your browser. Hertz sizes the CM and DM stages, rounds onto real parts, and can generate the filter\'s own PCB.' }}</p>
        <p class="note"><b>A seeding estimate, not a measurement.</b> DM ±10 dB (the input
          capacitor's branch parasitics are assumed), CM ±15 dB (the stray path is a floor).
          Levels are peak against a {{ cv ? cv.detector : 'quasi-peak' }} line, which errs
          pessimistic. Verify with a LISN.</p>
      </div>

    </section>
  </div>
</template>

<style scoped>
.conducted {
  margin: 0 16px 14px; padding-top: 10px;
  border-top: 1px solid var(--resin-edge, #2a3a32);
}
.chead { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }
.chead h3 { font-family: var(--display); font-size: 13px; font-weight: 700;
  letter-spacing: 0.12em; text-transform: uppercase; color: var(--copper); }
.chead .pick { margin-left: auto; }
.cbig { margin: 8px 0; font-size: 14px; }
.cbig.fail b { color: var(--heat-high); }
.cbig.watch b { color: var(--heat-med); }
.cbig.ok b { color: var(--heat-low); }
.cfig canvas { height: 220px; }
.k.dm { color: #6f9fc4; }
.k.cmm { color: var(--copper); }
.cwhich { font-size: 12.5px; color: var(--tin); margin-top: 4px; }
.creq { display: flex; gap: 18px; flex-wrap: wrap; margin: 8px 0;
  font-family: var(--mono); font-size: 12px; }
.creq > div { display: flex; gap: 6px; }
.creq dt { color: var(--tin); }
.creq dd { color: var(--silk); }
.cstray { margin-top: 8px; }
.simrow { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px;
  margin: 8px 0; }
.simbtn { border: 1px solid var(--resin-edge); border-radius: 999px;
  padding: 2px 12px; font-family: var(--mono); font-size: 11px;
  color: var(--tin); cursor: pointer; }
.simbtn:hover { border-color: var(--copper); color: var(--copper); }
.simbtn input { display: none; }
.simnote { flex: 1 1 100%; font-size: 12px; color: var(--tin); }
.simnote.warn { color: var(--heat-med); }
.inl { margin-left: 8px; font-family: var(--mono); font-size: 11px; }
.cwhich.warn { color: var(--heat-med); }
.conducted .hbtn {
  margin-top: 6px; padding: 7px 14px; cursor: pointer;
  background: none; color: #58c79a; border: 1px solid #2a5a46; border-radius: 5px;
  font: 600 13px/1 var(--mono, monospace); letter-spacing: .03em;
}
.conducted .hbtn:hover { border-color: #58c79a; }

.presets { margin: 0 16px 12px; }
.plabel { font-size: 13px; color: var(--silk); margin-bottom: 6px; }
.prow { display: flex; gap: 8px; flex-wrap: wrap; }
.pchip {
  display: flex; flex-direction: column; align-items: flex-start; gap: 2px;
  border: 1px solid var(--resin-edge); border-radius: 6px; padding: 6px 12px;
  color: var(--silk); font-size: 12.5px; text-align: left;
}
.pchip small { font-family: var(--mono); font-size: 10.5px; color: var(--tin); }
.pchip:hover { border-color: var(--copper); }
.pchip.on { border-color: var(--copper); background: rgba(217,139,95,0.12); }
.passumed { margin-top: 8px; font-size: 12px; color: var(--tin); }
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
.vtog { font-family: var(--mono); font-size: 11px; color: var(--tin);
  border: 1px solid var(--resin-edge); border-radius: 999px; padding: 2px 10px; }
.vtog:hover { border-color: var(--copper); color: var(--copper); }
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
