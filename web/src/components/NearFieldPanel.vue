<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
  report: { type: Object, required: true },
})
const emit = defineEmits(['close'])

const current = ref(10)        // A, switched at the fundamental
// The HF amplitude at the ring frequency, which is a FRACTION of the switched
// current. Driving the ring with the full DC current is what produced victim
// ratios in the millions — a number that destroys trust rather than conveying
// risk.
const ringCurrent = ref(2)     // A, at the ring frequency
const ring = ref(130)          // MHz, hot-loop ring
const height = ref(3)          // mm, emulated probe height
const victimArea = ref(4)      // mm², victim loop

const result = ref(null)
const error = ref('')

function run() {
  try {
    const out = JSON.parse(props.engine.nearField(JSON.stringify({
      currentA: current.value,
      ringCurrentA: ringCurrent.value,
      ringMhz: ring.value,
      probeHeightMm: height.value,
      victimAreaMm2: victimArea.value,
    })))
    if (out.error) { error.value = out.error; result.value = null; return }
    error.value = ''
    result.value = out
  } catch (e) { error.value = String(e) }
}
function onInput() { run(); nextTick(draw) }

// ---- the field picture --------------------------------------------------
// A quasi-static H map: |H| summed in POWER over the dipoles, sampled on a
// grid at the stated height. Not a radiation pattern, and never labelled in
// dBuV/m.
const canvas = ref(null)

// |H| from a filamentary polygon loop, in A/m. Same expression as
// NearField.hpp's h_loop, which is pinned against the point-dipole limit.
// Coordinates in mm, current in A.
function hLoop(hull, px, py, pz, cur) {
  if (!hull || hull.length < 3) return 0
  let hx = 0, hy = 0, hz = 0
  for (let i = 0; i < hull.length; i++) {
    const A = hull[i], B = hull[(i + 1) % hull.length]
    const ax = (A[0] - px) * 1e-3, ay = (A[1] - py) * 1e-3, az = -pz * 1e-3
    const bx = (B[0] - px) * 1e-3, by = (B[1] - py) * 1e-3, bz = -pz * 1e-3
    const na = Math.hypot(ax, ay, az), nb = Math.hypot(bx, by, bz)
    if (!(na > 0) || !(nb > 0)) continue
    const cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx
    const dot = ax * bx + ay * by + az * bz
    const den = na * nb * (na * nb + dot)
    if (!(den > 0)) continue
    const k = cur * (na + nb) / (4 * Math.PI * den)
    hx += k * cx; hy += k * cy; hz += k * cz
  }
  return Math.hypot(hx, hy, hz)
}

function draw() {
  const cv = canvas.value
  const r = result.value
  if (!cv || !r || !r.aggressors.length) return
  const b = props.report.board
  const dpr = window.devicePixelRatio || 1
  const w = cv.clientWidth, h = cv.clientHeight
  cv.width = w * dpr; cv.height = h * dpr
  const ctx = cv.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, w, h)

  // board.bbox is [x1, y1, x2, y2]
  const [bx1, by1, bx2, by2] = b.bbox
  const bw = bx2 - bx1, bh = by2 - by1
  const s = Math.min(w / bw, h / bh) * 0.94
  const ox = (w - bw * s) / 2, oy = (h - bh * s) / 2
  const X = mm => ox + (mm - bx1) * s
  const Y = mm => oy + (mm - by1) * s

  // sample the quasi-static field on a grid
  const step = 4
  const nx = Math.ceil(w / step), ny = Math.ceil(h / step)
  const z = height.value
  const field = new Float64Array(nx * ny)
  let peak = 0
  for (let iy = 0; iy < ny; iy++) {
    for (let ix = 0; ix < nx; ix++) {
      const mx = bx1 + ((ix * step - ox) / s)
      const my = by1 + ((iy * step - oy) / s)
      // Exact Biot-Savart over each loop polygon — valid at EVERY distance
      // outside the conductor. The point dipole would have to be blanked
      // within ~46 mm of a 267 mm2 loop, which on a 100 mm board is most of
      // the picture; that hole is what the first version drew.
      let p = 0
      for (const a of r.aggressors) {
        const hh = hLoop(a.hull, mx, my, z, ringCurrent.value)
        p += hh * hh
      }
      const v = Math.sqrt(p)
      field[iy * nx + ix] = v
      if (v > peak) peak = v
    }
  }
  if (!(peak > 0)) return

  // three decades, cool teal to hot — the same visual language as the
  // radiation layer, but this one is A/m and says so
  const img = ctx.createImageData(nx, ny)
  const floor = peak * 1e-3
  for (let i = 0; i < nx * ny; i++) {
    const v = field[i]
    const t = v > floor ? Math.log(v / floor) / Math.log(peak / floor) : 0
    let rr, gg, bb
    if (t < 0.5) { const u = t / 0.5; rr = 16 + 62 * u; gg = 22 + 126 * u; bb = 19 + 113 * u }
    else if (t < 0.8) { const u = (t - 0.5) / 0.3; rr = 78 + 139 * u; gg = 148 - 9 * u; bb = 132 - 37 * u }
    else { const u = (t - 0.8) / 0.2; rr = 217 + 38 * u; gg = 139 + 98 * u; bb = 95 + 137 * u }
    const o = i * 4
    img.data[o] = rr; img.data[o + 1] = gg; img.data[o + 2] = bb; img.data[o + 3] = 235
  }
  const off = document.createElement('canvas')
  off.width = nx; off.height = ny
  off.getContext('2d').putImageData(img, 0, 0)
  ctx.imageSmoothingEnabled = true
  ctx.imageSmoothingQuality = 'high'
  ctx.drawImage(off, 0, 0, w, h)

  // board outline
  ctx.strokeStyle = 'rgba(157,180,173,0.35)'
  ctx.lineWidth = 1
  ctx.strokeRect(X(bx1), Y(by1), bw * s, bh * s)

  // the aggressors, with their validity radius drawn as the honest boundary
  for (const a of r.aggressors) {
    // the loop itself, which is what the field is actually integrated over
    if (a.hull && a.hull.length >= 3) {
      ctx.strokeStyle = 'rgba(255,93,93,0.85)'
      ctx.lineWidth = 1.6
      ctx.beginPath()
      a.hull.forEach(([hx2, hy2], i) => i ? ctx.lineTo(X(hx2), Y(hy2))
                                          : ctx.moveTo(X(hx2), Y(hy2)))
      ctx.closePath(); ctx.stroke()
    }
    // context only: beyond this radius a point dipole would also have done
    ctx.strokeStyle = 'rgba(157,180,173,0.28)'
    ctx.setLineDash([2, 4]); ctx.lineWidth = 1
    ctx.beginPath(); ctx.arc(X(a.xMm), Y(a.yMm), a.validFromMm * s, 0, 7); ctx.stroke()
    ctx.setLineDash([])
  }

  // the victims
  for (const v of r.victims.slice(0, 24)) {
    const col = v.level === 'over' ? '#ff5d5d'
              : v.level === 'watch' ? '#ffb454' : '#58c79a'
    ctx.strokeStyle = col; ctx.lineWidth = 1.6
    ctx.beginPath(); ctx.arc(X(v.xMm), Y(v.yMm), 5, 0, 7); ctx.stroke()
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

const num = (x, d = 1) => (x === undefined || x === null ? '—' : Number(x).toFixed(d))
const r = computed(() => result.value)
const worst = computed(() => r.value?.victims?.[0] ?? null)
const mv = x => (Math.abs(x) >= 1 ? `${num(x, 2)} mV` : `${num(x * 1000, 0)} µV`)
// A ratio of 15,200x is not informative as "1520061% of threshold". Past 10x,
// engineers read decibels.
const overBy = r => (r >= 10 ? `${num(20 * Math.log10(r), 0)} dB over`
                             : `${num(r * 100, 0)}% of`)
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="nearfield" role="dialog"
             aria-label="Component near-field map">
      <header>
        <h2>NEAR FIELD</h2>
        <span class="sub">component scale · quasi-static</span>
        <div class="sp" />
        <span v-if="r" class="ctx" data-testid="nf-context">
          λ/2π = {{ num(r.lambdaOver2PiMm, 0) }} mm at {{ num(r.ringMhz, 0) }} MHz —
          the whole board is inside it
        </span>
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <p v-if="error" class="err" data-testid="nf-error">{{ error }}</p>

      <div v-if="r" class="body">
        <figure>
          <canvas ref="canvas" data-testid="nf-canvas" />
          <figcaption>
            <span class="k src">▭ switching loop (the field is integrated over it)</span>
            <span class="k dash">◌ beyond this a point dipole would also do</span>
            <span class="k ok">○ victim</span>
            <span class="cap">|H| in A/m at {{ num(height, 1) }} mm above the board.
              Colour is relative to the strongest point on <b>this</b> board — it is
              not a limit.</span>
          </figcaption>
        </figure>

        <div class="side">
          <div v-if="worst" class="verdict" :class="worst.level" data-testid="nf-verdict">
            <div class="big">{{ mv(worst.inducedMv) }}</div>
            <div class="of">induced in <b>{{ worst.component }}</b> ({{ worst.net }})
              against {{ mv(worst.thresholdMv) }} for {{ worst.classLabel }}</div>
            <div class="bar"><i :style="{ width: Math.min(100, worst.ratio * 100) + '%' }" /></div>
            <div class="pct">{{ overBy(worst.ratio) }} its threshold ·
              {{ num(worst.distanceMm, 1) }} mm from {{ worst.aggressor }}</div>
            <p class="hint">Near-field decay is 1/r³ — <b>18 dB per doubling</b> of
              distance. Moving this part twice as far away is worth far more than
              any shield.</p>
          </div>

          <table class="vic" data-testid="nf-victims">
            <thead><tr><th>part</th><th>class</th><th>d</th><th>|H|</th><th>induced</th></tr></thead>
            <tbody>
              <tr v-for="v in r.victims.slice(0, 10)" :key="v.component + v.net"
                  :class="v.level">
                <td>{{ v.component }}</td>
                <td>{{ v.class }}</td>
                <td>{{ num(v.distanceMm, 1) }}<i v-if="!v.dipoleValid" class="inside"
                    title="inside the point-dipole radius — the exact loop integral is used here">*</i></td>
                <td>{{ num(v.hDbuaM, 0) }} dBµA/m</td>
                <td>{{ mv(v.inducedMv) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div v-if="r" class="caveat" data-testid="nf-caveat">
        <b>This is not a radiation map and cannot predict compliance.</b> It is the
        quasi-static induction field at component scale, in A/m at a stated height.
        There is no reliable near-field to far-field transform, so there is no
        dBµV/m here, no limit line and no pass/fail — only rank order on this board.
        The <b>ring current</b> — the HF amplitude at the resonance, not the DC or
        switched current — is <b>your assumption</b> and scales every number
        linearly. Cable common-mode current, which usually dominates real failures,
        is not modelled here — see the emissions panel for that budget.
        <span v-if="r.tooCloseCount" class="note">
          {{ r.tooCloseCount }} part(s) marked <b>*</b> sit closer than a point-dipole
          approximation would allow. They still get a real number: the field is an
          exact Biot-Savart integral over the loop itself, which holds at any
          distance outside the copper.</span>
      </div>

      <div class="controls">
        <label class="sl"><span>ring current <b>{{ num(ringCurrent) }} A</b></span>
          <input data-testid="nf-current" type="range" min="0.05" max="20" step="0.05"
                 v-model.number="ringCurrent" @input="onInput" /></label>
        <label class="sl"><span>ring frequency <b>{{ num(ring, 0) }} MHz</b></span>
          <input data-testid="nf-ring" type="range" min="10" max="500" step="5"
                 v-model.number="ring" @input="onInput" /></label>
        <label class="sl"><span>probe height <b>{{ num(height, 1) }} mm</b></span>
          <input data-testid="nf-height" type="range" min="0.5" max="30" step="0.5"
                 v-model.number="height" @input="onInput" /></label>
        <label class="sl"><span>victim loop <b>{{ num(victimArea, 1) }} mm²</b></span>
          <input type="range" min="0.5" max="40" step="0.5"
                 v-model.number="victimArea" @input="onInput" /></label>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 50; background: rgba(8,12,10,0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px; }
.panel { width: min(1120px, 100%); max-height: 100%; display: flex; flex-direction: column;
  overflow: auto; background: var(--resin); border: 1px solid var(--resin-edge); border-radius: 8px; }
header { display: flex; align-items: baseline; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper); }
.sub, .ctx { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }
.err { margin: 12px 16px; padding: 10px 12px; border-radius: 4px; background: #3a1a1e;
  color: #ffb3b8; font-family: var(--mono); font-size: 12.5px; }

.body { display: grid; gap: 14px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1.35fr) minmax(0, 1fr); }
@media (max-width: 860px) { .body { grid-template-columns: 1fr; } }
canvas { width: 100%; aspect-ratio: 4 / 3; display: block; border-radius: 4px; background: #0d1210; }
figcaption { display: flex; gap: 12px; flex-wrap: wrap; padding-top: 6px;
  font-family: var(--mono); font-size: 11px; color: var(--tin); }
.k.src { color: var(--heat-high); }
.k.ok { color: var(--heat-low); }
.cap { flex: 1 1 100%; opacity: 0.78; font-family: var(--sans); }

.side { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.verdict { border: 1px solid var(--resin-edge); border-left-width: 3px; border-radius: 4px;
  padding: 13px; display: flex; flex-direction: column; gap: 7px; }
.verdict.ok { border-left-color: var(--heat-low); }
.verdict.watch { border-left-color: var(--heat-med); }
.verdict.over { border-left-color: var(--heat-high); }
.big { font-family: var(--display); font-size: 38px; font-weight: 700; line-height: 1; }
.of { font-size: 12.5px; color: var(--tin); }
.of b { color: var(--silk); font-family: var(--mono); }
.bar { height: 6px; border-radius: 3px; background: #0d1210; overflow: hidden; }
.bar i { display: block; height: 100%; background: var(--heat-low); }
.verdict.watch .bar i { background: var(--heat-med); }
.verdict.over .bar i { background: var(--heat-high); }
.pct { font-family: var(--mono); font-size: 11.5px; color: var(--tin); }
.hint { font-size: 11.5px; color: var(--tin); }

.vic { width: 100%; border-collapse: collapse; font-family: var(--mono); font-size: 11px; }
.vic th { text-align: left; color: var(--tin); font-weight: 500; padding: 3px 6px 5px 0;
  border-bottom: 1px solid var(--resin-edge); }
.vic td { padding: 3px 6px 3px 0; border-bottom: 1px dotted var(--resin-edge); }
.vic tr.over td:first-child { color: var(--heat-high); }
.vic tr.watch td:first-child { color: var(--heat-med); }
.vic tr.tc td { color: var(--tin); opacity: 0.7; }

.caveat { margin: 0 16px 12px; padding: 10px 12px; border-radius: 4px;
  background: var(--bare-fr4); border: 1px solid var(--resin-edge);
  font-size: 12px; color: var(--tin); }
.caveat b { color: var(--silk); }
.caveat .warn { display: block; margin-top: 6px; color: var(--heat-med); }
.caveat .note { display: block; margin-top: 6px; }
.vic .inside { color: var(--heat-med); font-style: normal; padding-left: 2px; }

.controls { display: grid; gap: 10px 18px; padding: 12px 16px;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  border-top: 1px solid var(--resin-edge); background: var(--bare-fr4); }
.sl { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: var(--copper); }
</style>
