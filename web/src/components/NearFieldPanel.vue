<script setup>
import { ref, computed, watch } from 'vue'

const props = defineProps({
  engine: { type: Object, required: true },
  result: { type: Object, required: true },
  params: { type: Object, required: true },
})
const emit = defineEmits(['close', 'params'])

// ---- shielding -----------------------------------------------------------
// A can is offered as a CONDITIONAL, never as a fix. What it buys is set by
// the wall at low frequency and by the seam at high frequency, and those have
// opposite sensitivities — so both are shown and the binding one is named.
const materials = ref([])
const material = ref('tinsteel')
const wall = ref(0.2)
const seam = ref(5)
const shield = ref(null)

function runShield() {
  try {
    if (!materials.value.length)
      materials.value = JSON.parse(props.engine.shieldMaterials())
    const out = JSON.parse(props.engine.shielding(JSON.stringify({
      material: material.value, wallMm: wall.value, seamPitchMm: seam.value,
      fMhz: props.result.ringMhz, fiveSided: true,
    })))
    shield.value = out.error ? null : out
  } catch { shield.value = null }
}
watch([material, wall, seam, () => props.result.ringMhz], runShield, { immediate: true })

const num = (x, d = 1) => (x === undefined || x === null ? '—' : Number(x).toFixed(d))
const mv = x => (Math.abs(x) >= 1 ? `${num(x, 2)} mV` : `${num(x * 1000, 0)} µV`)
// Past 10x a percentage conveys nothing. Engineers read decibels.
const overBy = r => (r >= 10 ? `${num(20 * Math.log10(r), 0)} dB over`
                             : `${num(r * 100, 0)}% of`)
const magnetic = computed(() => shield.value?.results?.find(r => r.field === 'magnetic'))
const electric = computed(() => shield.value?.results?.find(r => r.field === 'electric'))
const worst = computed(() => props.result.victims?.[0] ?? null)

function set(k, v) { emit('params', { [k]: Number(v) }) }
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="nf-panel" role="dialog"
             aria-label="Near-field victims and shielding">
      <header>
        <h2>NEAR FIELD</h2>
        <span class="sub">victims &amp; what a shield would buy</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <div class="body">
        <div class="col">
          <h3>Coupled into sensitive parts</h3>
          <div v-if="worst" class="verdict" :class="worst.level" data-testid="nf-verdict">
            <div class="big">{{ mv(worst.inducedMv) }}</div>
            <div class="of">in <b>{{ worst.component }}</b> ({{ worst.net }}) against
              {{ mv(worst.thresholdMv) }} for {{ worst.classLabel }} —
              <b>{{ overBy(worst.ratio) }}</b> its threshold,
              {{ num(worst.distanceMm, 1) }} mm from {{ worst.aggressor }}</div>
            <p class="hint">Decay is 1/r³, <b>18 dB per doubling</b>. Moving this part
              from {{ num(worst.distanceMm, 0) }} to {{ num(worst.distanceMm * 2, 0) }} mm
              is worth ~18 dB and costs nothing. Rotating its loop edge-on nulls the
              coupling entirely — and no distance rule can see that.</p>
          </div>

          <table class="vic" data-testid="nf-victims">
            <thead><tr><th>part</th><th>class</th><th>d</th><th>|H|</th><th>cosθ</th><th>induced</th></tr></thead>
            <tbody>
              <tr v-for="v in result.victims.slice(0, 12)" :key="v.component + v.net"
                  :class="v.level">
                <td>{{ v.component }}</td>
                <td>{{ v.class }}</td>
                <td>{{ num(v.distanceMm, 1) }}<i v-if="!v.dipoleValid" class="inside"
                    title="closer than a point dipole allows — the exact loop integral is used here">*</i></td>
                <td>{{ num(v.hDbuaM, 0) }} dBµA/m</td>
                <td :title="v.oriented ? 'from this net\u2019s own routed direction'
                    : 'net unrouted near the pad — worst case 1.0 assumed'">
                  {{ v.oriented ? num(v.cosTheta, 2) : '1*' }}</td>
                <td>{{ mv(v.inducedMv) }}{{ v.shieldDb > 0 ? ' 🛡' : '' }}</td>
              </tr>
            </tbody>
          </table>
        </div>

        <div class="col">
          <table v-if="result.capacitive?.length" class="vic" data-testid="nf-cap">
            <caption>Broadside over switch-node copper — the E mechanism</caption>
            <thead><tr><th>part</th><th>overlap</th><th>C₁₂</th><th>ΔV bound</th></tr></thead>
            <tbody>
              <tr v-for="ch in result.capacitive.slice(0, 6)" :key="ch.component + ch.net"
                  :class="ch.level">
                <td>{{ ch.component }} ({{ ch.net }})</td>
                <td>{{ num(ch.overlapMm2, 1) }} mm²</td>
                <td>{{ num(ch.c12Pf, 2) }} pF</td>
                <td>{{ mv(ch.dvMv) }} / {{ mv(ch.thresholdMv) }}</td>
              </tr>
            </tbody>
          </table>
          <h3>What a shield can would buy</h3>
          <div v-if="shield && magnetic && electric" class="shield" data-testid="nf-shield">
            <div class="rows">
              <div class="row hdr"><span /><span>magnetic (this map)</span><span>electric</span></div>
              <div class="row"><span>delivered</span>
                <b>{{ num(magnetic.seDb, 0) }} dB</b>
                <b>{{ num(electric.seDb, 0) }} dB</b></div>
              <div class="row"><span>wall absorbs</span>
                <span>{{ num(magnetic.absorptionDb, 0) }} dB</span>
                <span>{{ num(electric.absorptionDb, 0) }} dB</span></div>
              <div class="row"><span>seam allows</span>
                <span>{{ num(magnetic.apertureDb, 0) }} dB</span>
                <span>{{ num(electric.apertureDb, 0) }} dB</span></div>
              <div class="row"><span>limited by</span>
                <b class="lim" data-testid="nf-limited">{{ magnetic.limitedBy }}</b>
                <b class="lim">{{ electric.limitedBy }}</b></div>
            </div>
            <p class="skin">Wall is {{ num(magnetic.wallsPerSkin, 1) }} skin depths
              ({{ num(magnetic.skinDepthUm, 1) }} µm at {{ num(result.ringMhz, 0) }} MHz).</p>
            <p v-if="magnetic.caveat" class="warn">{{ magnetic.caveat }}.</p>
            <p class="note">
              At <b>{{ num(result.ringMhz, 0) }} MHz</b> the metal is opaque and only the
              <b>contact pitch</b> matters — on a two-part can that is the cover-to-frame
              spring spacing, not the alloy on the datasheet. Below a few MHz it inverts:
              the wall binds and permeability is worth tens of dB. And a can does
              <b>nothing</b> for cable common-mode current, which usually dominates real
              failures — see the emissions panel for that budget.</p>
          </div>

          <div class="ctl">
            <label><span>material</span>
              <select v-model="material" data-testid="nf-shield-material">
                <option v-for="m in materials" :key="m.id" :value="m.id">{{ m.label }}</option>
              </select></label>
            <label><span>wall <b>{{ num(wall, 2) }} mm</b></span>
              <input type="range" min="0.05" max="1" step="0.05" v-model.number="wall" /></label>
            <label><span>contact pitch <b>{{ num(seam, 1) }} mm</b></span>
              <input data-testid="nf-shield-seam" type="range" min="1" max="40" step="0.5"
                     v-model.number="seam" /></label>
          </div>
        </div>
      </div>

      <div class="controls">
        <label class="sl"><span>ring current <b>{{ num(params.ringCurrentA) }} A</b></span>
          <input data-testid="nf-current" type="range" min="0.05" max="20" step="0.05"
                 :value="params.ringCurrentA"
                 @input="e => set('ringCurrentA', e.target.value)" /></label>
        <label class="sl"><span>ring frequency <b>{{ num(params.ringMhz, 0) }} MHz</b></span>
          <input data-testid="nf-ring" type="range" min="10" max="500" step="5"
                 :value="params.ringMhz" @input="e => set('ringMhz', e.target.value)" /></label>
        <label class="sl"><span>probe height <b>{{ num(params.probeHeightMm, 1) }} mm</b></span>
          <input data-testid="nf-height" type="range" min="0.5" max="30" step="0.5"
                 :value="params.probeHeightMm"
                 @input="e => set('probeHeightMm', e.target.value)" /></label>
        <label class="sl"><span>victim loop <b>{{ num(params.victimAreaMm2, 1) }} mm²</b></span>
          <input type="range" min="0.5" max="40" step="0.5"
                 :value="params.victimAreaMm2"
                 @input="e => set('victimAreaMm2', e.target.value)" /></label>
        <label class="sl"><span>inductor construction</span>
          <select data-testid="nf-inductor" :value="params.inductorType ?? 'unshielded'"
                  @change="e => emit('params', { inductorType: e.target.value })"
                  class="isel">
            <option value="unshielded">unshielded drum (1.0×)</option>
            <option value="semi">semi-shielded (0.65×)</option>
            <option value="shielded">shielded ferrite (0.35×)</option>
            <option value="composite">moulded composite (0.3×)</option>
          </select></label>
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
  letter-spacing: 0.16em; color: var(--heat-low); }
.sub { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }

.body { display: grid; gap: 18px; padding: 14px 16px;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); }
@media (max-width: 900px) { .body { grid-template-columns: 1fr; } }
.col { min-width: 0; display: flex; flex-direction: column; gap: 10px; }
h3 { font-family: var(--display); font-size: 13px; font-weight: 700; letter-spacing: 0.08em;
  text-transform: uppercase; color: var(--tin); }

.verdict { border: 1px solid var(--resin-edge); border-left-width: 3px; border-radius: 4px;
  padding: 12px; display: flex; flex-direction: column; gap: 6px; }
.verdict.ok { border-left-color: var(--heat-low); }
.verdict.watch { border-left-color: var(--heat-med); }
.verdict.over { border-left-color: var(--heat-high); }
.big { font-family: var(--display); font-size: 34px; font-weight: 700; line-height: 1; }
.of { font-size: 12.5px; color: var(--tin); }
.of b { color: var(--silk); font-family: var(--mono); }
.hint { font-size: 11.5px; color: var(--tin); }

.vic { width: 100%; border-collapse: collapse; font-family: var(--mono); font-size: 11px; }
.vic th { text-align: left; color: var(--tin); font-weight: 500; padding: 3px 6px 5px 0;
  border-bottom: 1px solid var(--resin-edge); }
.vic td { padding: 3px 6px 3px 0; border-bottom: 1px dotted var(--resin-edge); }
.vic tr.over td:first-child { color: var(--heat-high); }
.vic tr.watch td:first-child { color: var(--heat-med); }
.vic .inside { color: var(--heat-med); font-style: normal; padding-left: 2px; }

.shield { border: 1px solid var(--resin-edge); border-radius: 4px; padding: 12px;
  display: flex; flex-direction: column; gap: 8px; }
.rows { display: flex; flex-direction: column; font-family: var(--mono); font-size: 11.5px; }
.row { display: grid; grid-template-columns: 1fr auto auto; gap: 12px; padding: 3px 0;
  border-bottom: 1px dotted var(--resin-edge); }
.row > :first-child { color: var(--tin); text-align: left; }
.row > :not(:first-child) { text-align: right; }
.row.hdr { color: var(--tin); border-bottom: 1px solid var(--resin-edge); }
.row b.lim { color: var(--silk); }
.skin, .note, .warn { font-size: 11.5px; color: var(--tin); }
.warn { color: var(--heat-med); }
.note b { color: var(--silk); }

.ctl { display: grid; gap: 8px; }
.ctl label { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.ctl b { color: var(--silk); font-family: var(--mono); }
.ctl input { accent-color: var(--heat-low); }
.ctl select { background: var(--resin); color: var(--silk); font-family: var(--mono);
  font-size: 12px; border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px; }

.controls { display: grid; gap: 10px 18px; padding: 12px 16px;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  border-top: 1px solid var(--resin-edge); background: var(--bare-fr4); }
.sl { display: flex; flex-direction: column; gap: 3px; font-size: 11.5px; color: var(--tin); }
.sl span b { color: var(--silk); font-family: var(--mono); }
.sl input { width: 100%; accent-color: var(--heat-low); }
.sl .isel { background: var(--resin); color: var(--silk); font-family: var(--mono);
  font-size: 12px; border: 1px solid var(--resin-edge); border-radius: 3px; padding: 4px 6px; }
.vic caption { text-align: left; color: var(--tin); font-size: 11px; padding-bottom: 4px; }
</style>
