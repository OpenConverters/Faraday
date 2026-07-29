<script setup>
import { computed, reactive, ref, watch } from 'vue'

const props = defineProps({
  // previously applied custom stackup (layers array), to edit in place
  initial: { type: Array, default: null },
  // copper count the board needs, when known (from the engine's refusal)
  copperHint: { type: Number, default: 0 },
})
const emit = defineEmits(['apply', 'close'])

const CU_T = [0.018, 0.035, 0.07]                 // ½oz, 1oz, 2oz

function fromInitial() {
  if (!props.initial) return null
  const cu = props.initial.filter(l => l.kind === 'copper')
  return {
    n: cu.length,
    cuT: cu[0]?.thicknessMm ?? 0.035,
    d: props.initial.filter(l => l.kind === 'dielectric')
      .map(l => ({ h: l.thicknessMm, er: l.epsilonR })),
  }
}

const init = fromInitial()
const nCu = ref(init?.n ?? (props.copperHint || 2))
const cuT = ref(init?.cuT ?? 0.035)
// dielectric rows are kept for the LARGEST layer count seen, so switching
// 4 → 2 → 4 does not wipe what was typed
const diel = reactive([])
function ensureRows() {
  while (diel.length < nCu.value - 1)
    diel.push({ h: diel.length % 2 ? 1.0 : 0.2, er: 4.5 })
}
if (init) init.d.forEach((d, i) => { diel[i] = { ...d } })
ensureRows()
watch(nCu, ensureRows)

const rows = computed(() => diel.slice(0, nCu.value - 1))
const totalMm = computed(() =>
  nCu.value * cuT.value + rows.value.reduce((s, d) => s + (+d.h || 0), 0))

const layers = computed(() => {
  const out = []
  for (let i = 0; i < nCu.value; i++) {
    const name = i === 0 ? 'F.Cu' : i === nCu.value - 1 ? 'B.Cu' : `In${i}.Cu`
    out.push({ kind: 'copper', name, thicknessMm: cuT.value })
    if (i < nCu.value - 1)
      out.push({ kind: 'dielectric', name: `dielectric ${i + 1}`,
                 thicknessMm: +rows.value[i].h, epsilonR: +rows.value[i].er })
  }
  return out
})

const valid = computed(() =>
  rows.value.every(d => +d.h > 0 && +d.er >= 1))

// cross-section preview: heights proportional to thickness, copper never
// thinner than 3px so it stays visible next to a 1.5 mm core
const preview = computed(() => {
  const H = 150
  const total = totalMm.value || 1
  return layers.value.map(l => ({
    kind: l.kind,
    name: l.name,
    mm: l.thicknessMm,
    px: l.kind === 'copper'
      ? Math.max(3, (l.thicknessMm / total) * H)
      : Math.max(6, (l.thicknessMm / total) * H),
  }))
})
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="stackup-editor" role="dialog"
             aria-label="Custom stackup">
      <header>
        <h2>STACKUP</h2>
        <span class="sub">the dielectric heights every Z₀ and coupling figure
          stands on — from your fab's stackup drawing</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <div class="body">
        <div class="ctrl">
          <label><span>copper layers</span>
            <select v-model.number="nCu" data-testid="se-ncu">
              <option v-for="n in [2, 4, 6, 8]" :key="n" :value="n">{{ n }}</option>
            </select></label>
          <label><span>copper thickness</span>
            <select v-model.number="cuT" data-testid="se-cu-t">
              <option v-for="t in CU_T" :key="t" :value="t">
                {{ (t * 1000).toFixed(0) }} µm ({{ t === 0.018 ? '½' : t === 0.035 ? '1' : '2' }} oz)
              </option>
            </select></label>

          <table>
            <thead><tr><th>dielectric</th><th>height mm</th><th>ε<sub>r</sub></th></tr></thead>
            <tbody>
              <tr v-for="(d, i) in rows" :key="i">
                <td class="dname">{{ i + 1 }} <span>({{ i === 0 || i === nCu - 2 ? 'outer' : 'inner' }})</span></td>
                <td><input type="number" step="0.05" min="0.01" v-model="d.h"
                           :data-testid="`se-d${i}-h`" /></td>
                <td><input type="number" step="0.1" min="1" v-model="d.er"
                           :data-testid="`se-d${i}-er`" /></td>
              </tr>
            </tbody>
          </table>

          <p class="tot">board total <b>{{ totalMm.toFixed(2) }} mm</b></p>
          <p class="note">Numbers come from the fab's stackup drawing (often
            called "impedance control stackup"). FR4 cores run ε<sub>r</sub> ≈ 4.2–4.8;
            prepreg ≈ 3.6–4.2. The report will state this stackup as yours.</p>

          <div class="acts">
            <button class="apply" data-testid="se-apply" :disabled="!valid"
                    @click="emit('apply', layers)">use this stackup</button>
            <button class="cancel" @click="emit('close')">cancel</button>
          </div>
        </div>

        <div class="xsec" aria-hidden="true">
          <div v-for="(l, i) in preview" :key="i" class="lay"
               :class="l.kind" :style="{ height: l.px + 'px' }">
            <span>{{ l.name }} · {{ l.mm.toFixed(3) }}</span>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 50; background: rgba(8,12,10,0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px; }
.panel { width: min(680px, 100%); max-height: 100%; overflow: auto;
  background: var(--resin); border: 1px solid var(--resin-edge); border-radius: 8px; }
header { display: flex; align-items: baseline; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper); }
.sub { font-family: var(--mono); font-size: 10.5px; color: var(--tin); max-width: 400px; }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }

.body { display: grid; grid-template-columns: 1fr 190px; gap: 18px; padding: 14px 16px; }
@media (max-width: 620px) { .body { grid-template-columns: 1fr; } }
.ctrl label { display: flex; align-items: center; gap: 10px; margin-bottom: 8px;
  font-size: 12px; color: var(--tin); }
.ctrl select, .ctrl input { background: var(--board); color: var(--silk);
  border: 1px solid var(--resin-edge); border-radius: 4px; padding: 3px 8px;
  font-family: var(--mono); font-size: 12px; }
table { width: 100%; border-collapse: collapse; margin: 10px 0 4px; }
th { font-family: var(--mono); font-size: 10px; text-transform: uppercase;
  letter-spacing: 0.08em; color: var(--tin); text-align: left; padding: 3px 6px; }
td { padding: 3px 6px; }
td input { width: 76px; }
.dname { font-family: var(--mono); font-size: 12px; color: var(--silk); }
.dname span { color: var(--tin); font-size: 10.5px; }
.tot { font-family: var(--mono); font-size: 12px; color: var(--tin); margin: 6px 0; }
.tot b { color: var(--copper); }
.note { font-size: 11.5px; color: var(--tin); line-height: 1.5; margin-bottom: 10px; }
.acts { display: flex; gap: 10px; }
.apply { border: 1px solid var(--copper); color: var(--copper); border-radius: 4px;
  padding: 5px 16px; font-size: 12.5px; }
.apply:disabled { opacity: 0.4; }
.apply:not(:disabled):hover { background: var(--copper); color: var(--board); }
.cancel { color: var(--tin); font-size: 12px; }

.xsec { display: flex; flex-direction: column; justify-content: center;
  padding: 6px 0; }
.lay { display: flex; align-items: center; padding-left: 8px; overflow: hidden; }
.lay span { font-family: var(--mono); font-size: 9px; color: rgba(0,0,0,0.55);
  white-space: nowrap; }
.lay.copper { background: linear-gradient(180deg, #e8955c, #c47844); }
.lay.dielectric { background: #3d4d45; }
.lay.dielectric span { color: rgba(232, 236, 233, 0.6); }
</style>
