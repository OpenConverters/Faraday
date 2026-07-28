<script setup>
import { ref, computed, watch } from 'vue'

// Pre-layout impedance calculator. The same 2D boundary-element extraction the
// bench uses, with no board required — which makes Faraday useful BEFORE a
// layout exists. Every number is a real field solve, not a curve fit.
const props = defineProps({
  engine: { type: Object, required: true },
})
const emit = defineEmits(['close'])

const mode = ref('microstrip')
const w = ref(0.3)
const gap = ref(0.2)
const h = ref(0.2)
const b = ref(0.8)
const er = ref(4.3)
const t = ref(0.035)
const targetZ = ref(50)

const result = ref(null)
const error = ref('')
const found = ref(null)

function solve(width) {
  const out = JSON.parse(props.engine.solvePair(JSON.stringify({
    mode: mode.value, w1Mm: width, w2Mm: width, gapMm: gap.value,
    hMm: h.value, bMm: b.value, tMm: t.value, epsR: er.value,
    lengthMm: 10, riseNs: 1, field: false, fix: false,
  })))
  if (out.error) throw new Error(out.error)
  return out.rlgc
}

function run() {
  try {
    result.value = solve(w.value)
    error.value = ''
  } catch (e) { error.value = String(e.message || e); result.value = null }
}

// Bisection on width for a target Z0 — each probe is a full extraction, which
// is what makes this a solver-backed answer rather than a chart lookup.
function findWidth() {
  try {
    let lo = 0.05, hi = 5.0
    if (solve(lo).z0 < targetZ.value) { found.value = { note: 'even 0.05 mm is below target' }; return }
    if (solve(hi).z0 > targetZ.value) { found.value = { note: 'even 5 mm is above target' }; return }
    for (let i = 0; i < 14; i++) {
      const mid = (lo + hi) / 2
      if (solve(mid).z0 > targetZ.value) lo = mid
      else hi = mid
    }
    const width = (lo + hi) / 2
    const r = solve(width)
    found.value = { widthMm: width, z0: r.z0 }
    w.value = Number(width.toFixed(3))
    result.value = r
  } catch (e) { error.value = String(e.message || e) }
}

watch([mode, w, gap, h, b, er, t], run, { immediate: true })
const num = (x, d = 2) => (x === undefined || x === null ? '—' : Number(x).toFixed(d))
const r = computed(() => result.value)
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="calc-panel" role="dialog"
             aria-label="Impedance calculator">
      <header>
        <h2>IMPEDANCE</h2>
        <span class="sub">2D field solve — no board needed</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <p v-if="error" class="err" data-testid="calc-error">{{ error }}</p>

      <div class="body">
        <div class="in">
          <label class="pick"><span>structure</span>
            <select v-model="mode" data-testid="calc-mode">
              <option value="microstrip">microstrip (outer layer)</option>
              <option value="stripline">stripline (buried)</option>
            </select></label>
          <label class="sl"><span>trace width <b>{{ num(w, 3) }} mm</b></span>
            <input data-testid="calc-w" type="range" min="0.05" max="2" step="0.005"
                   v-model.number="w" /></label>
          <label class="sl"><span>pair gap <b>{{ num(gap) }} mm</b></span>
            <input type="range" min="0.05" max="2" step="0.01" v-model.number="gap" /></label>
          <label v-if="mode === 'microstrip'" class="sl">
            <span>height to plane <b>{{ num(h) }} mm</b></span>
            <input type="range" min="0.05" max="1.5" step="0.01" v-model.number="h" /></label>
          <label v-else class="sl"><span>plane spacing <b>{{ num(b) }} mm</b></span>
            <input type="range" min="0.2" max="2" step="0.02" v-model.number="b" /></label>
          <label class="sl"><span>ε_r <b>{{ num(er, 1) }}</b></span>
            <input type="range" min="1.5" max="10" step="0.1" v-model.number="er" /></label>
          <label class="sl"><span>copper <b>{{ num(t, 3) }} mm</b></span>
            <input type="range" min="0.017" max="0.105" step="0.001"
                   v-model.number="t" /></label>
          <div class="target">
            <label><span>target Z₀</span>
              <input data-testid="calc-target" type="number" min="10" max="200"
                     v-model.number="targetZ" /> Ω</label>
            <button class="go" data-testid="calc-find" @click="findWidth">
              find width →</button>
            <span v-if="found?.widthMm" class="hit" data-testid="calc-found">
              w = <b>{{ num(found.widthMm, 3) }} mm</b> → {{ num(found.z0, 1) }} Ω</span>
            <span v-else-if="found?.note" class="hit">{{ found.note }}</span>
          </div>
        </div>

        <dl v-if="r" class="out" data-testid="calc-out">
          <div><dt>Z₀ (single)</dt><dd><b>{{ num(r.z0, 1) }} Ω</b></dd></div>
          <div><dt>Z even / odd</dt><dd>{{ num(r.zEven, 1) }} / {{ num(r.zOdd, 1) }} Ω</dd></div>
          <div><dt>Z differential</dt><dd><b>{{ num(r.zDiff, 1) }} Ω</b></dd></div>
          <div><dt>ε_eff</dt><dd>{{ num(r.epsEff) }}</dd></div>
          <div><dt>delay</dt><dd>{{ num(r.delayPsPerMm, 1) }} ps/mm</dd></div>
          <div><dt>k backward</dt><dd>{{ num(r.kb, 4) }} ({{ num(r.kbDb, 1) }} dB)</dd></div>
          <div><dt>L self</dt><dd>{{ num(r.lSelfNhPerMm, 3) }} nH/mm</dd></div>
          <div><dt>C self</dt><dd>{{ num(r.cSelfPfPerMm, 4) }} pF/mm</dd></div>
        </dl>
      </div>

      <p class="note">Boundary-element extraction of the actual cross-section —
        the same solver the bench validates against Cohn's exact solution to
        0.05%. Copper thickness is included; solder mask is not.</p>
    </section>
  </div>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 50; background: rgba(8,12,10,0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px; }
.panel { width: min(860px, 100%); max-height: 100%; display: flex; flex-direction: column;
  overflow: auto; background: var(--resin); border: 1px solid var(--resin-edge); border-radius: 8px; }
header { display: flex; align-items: baseline; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper); }
.sub { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }
.err { margin: 12px 16px; padding: 10px 12px; border-radius: 4px; background: #3a1a1e;
  color: #ffb3b8; font-family: var(--mono); font-size: 12.5px; }

.body { display: grid; gap: 18px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1.1fr) minmax(0, 1fr); }
@media (max-width: 720px) { .body { grid-template-columns: 1fr; } }
.in { display: flex; flex-direction: column; gap: 9px; }
.sl, .pick { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: var(--copper); }
.pick select { background: var(--bare-fr4); color: var(--silk); font-family: var(--mono);
  font-size: 12px; border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px; }
.target { display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
  border-top: 1px dotted var(--resin-edge); padding-top: 10px; margin-top: 4px;
  font-size: 11.5px; color: var(--tin); }
.target input { width: 62px; background: var(--bare-fr4); color: var(--silk);
  font-family: var(--mono); border: 1px solid var(--resin-edge); border-radius: 3px;
  padding: 3px 6px; }
.go { border: 1px solid var(--copper); border-radius: 4px; padding: 4px 12px;
  font-size: 12px; color: var(--copper); }
.go:hover { background: var(--copper); color: var(--bare-fr4); }
.hit { font-family: var(--mono); }
.hit b { color: var(--heat-low); }

.out { display: grid; gap: 2px; font-family: var(--mono); font-size: 12px;
  align-content: start; }
.out > div { display: flex; justify-content: space-between; gap: 10px;
  border-bottom: 1px dotted var(--resin-edge); padding: 5px 0; }
.out dt { color: var(--tin); }
.out b { color: var(--silk); font-size: 14px; }
.note { padding: 0 16px 14px; font-size: 11.5px; color: var(--tin); }
</style>
