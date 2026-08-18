<script setup>
import { computed, inject } from 'vue'
import { plainFor, PLAIN_SEVERITY } from '../glossary.js'

const props = defineProps({
  findings: { type: Array, required: true },
  report: { type: Object, required: true },
  selectedId: { type: String, default: '' },
  rules: { type: Array, default: () => [] },
  hiddenRules: { type: Set, default: () => new Set() },
  total: { type: Number, default: 0 },
  hiddenCount: { type: Number, default: 0 },
  diff: { type: Object, default: null },
  diffMap: { type: Object, default: () => ({}) },
  baselineName: { type: String, default: '' },
  onlyChanges: { type: Boolean, default: false },
})
// guided (basic) vs advanced — provided once by the app, never a prop drill
const basic = inject('basic', computed(() => false))

const emit = defineEmits(['select', 'toggleRule', 'bench', 'emissions', 'hide', 'unhideAll', 'glossary', 'toggleChanges', 'clearBaseline'])

const netName = id =>
  props.report.board.nets.find(n => n.id === id)?.name || (id >= 0 ? `net ${id}` : '')

const toolCount = computed(() =>
  props.findings.filter(f => f.solve || f.emit).length)

// Plain language for guided mode. A rule with no entry keeps its engineering
// text: better a sentence with a decibel in it than a finding that says less
// than the engine knows.
const plain = f => plainFor(f.rule)
const riskWord = f => PLAIN_SEVERITY[f.severityLabel] || f.severityLabel
// Guided mode drops the units from the row (the dB IS the ranking, and the
// list is already ranked) but keeps the two that are plain physical sizes.
const rowNumber = f => {
  if (basic.value) return f.rule === 'switch-node' || f.rule === 'commutation-loop'
    ? `${f.coupledLenMm.toFixed(0)} mm²` : ''
  if (f.nextDb !== undefined) return `${f.nextDb.toFixed(1)} dB`
  if (f.rule === 'switch-node' || f.rule === 'commutation-loop')
    return `${f.coupledLenMm.toFixed(0)} mm²`
  return f.coupledLenMm ? `${f.coupledLenMm.toFixed(1)} mm` : ''
}

const toolTitle = f => [
  f.solve && 'Field solve: extract this cross-section and simulate the crosstalk',
  f.emit && 'Emissions: predict the radiated field against the limit line',
].filter(Boolean).join(' · ')
</script>

<template>
  <aside class="panel">
    <h2 class="ptitle">
      Findings
      <span class="count" data-testid="finding-count">{{ findings.length }}</span>
      <span v-if="findings.length !== total" class="of">of {{ total }}</span>
      <button class="gloss" data-testid="open-glossary" title="what every rule means"
              @click="emit('glossary')">glossary</button>
    </h2>
    <button v-if="hiddenCount" class="restore" data-testid="restore-hidden"
            @click="emit('unhideAll')">restore {{ hiddenCount }} dismissed</button>
    <div v-if="diff" class="diffbar" data-testid="diff-bar"
         :class="diff.verdict">
      <b>vs {{ baselineName }}</b>
      <span>{{ diff.added.length }} new · {{ diff.worsened.length }} worsened ·
        {{ diff.improved.length }} improved · {{ diff.resolved.length }} resolved
        — <b>{{ diff.verdict }}</b></span>
      <button class="dchip" :class="{ on: onlyChanges }" data-testid="only-changes"
              @click="emit('toggleChanges')">only changes</button>
      <button class="dchip" data-testid="clear-baseline"
              @click="emit('clearBaseline')">✕</button>
      <span v-if="diff.resolved.length && !diff.added.length && !diff.worsened.length"
            class="dres">resolved: {{ diff.resolved.map(r => r.title).slice(0, 3).join(' · ') }}</span>
    </div>
    <div v-if="rules.length > 1 && !basic" class="filters" data-testid="rule-filters">
      <button v-for="[rule, n] in rules" :key="rule" class="rchip"
              :class="{ off: hiddenRules.has(rule) }"
              :data-testid="`rule-${rule}`"
              :aria-pressed="!hiddenRules.has(rule)"
              @click="emit('toggleRule', rule)">{{ rule }} <b>{{ n }}</b></button>
    </div>
    <p v-if="toolCount && !basic" class="legend" data-testid="tools-legend">
      <svg class="tool" viewBox="0 0 13 12" aria-hidden="true">
        <rect x="4" y="2" width="5" height="2.2" /><path d="M0.5 10.5h12" />
        <path d="M4 4.6C2.6 6.2 2.2 8.4 2.2 10.4" class="faint" />
        <path d="M9 4.6C10.4 6.2 10.8 8.4 10.8 10.4" class="faint" />
      </svg> field solve
      <svg class="tool" viewBox="0 0 13 12" aria-hidden="true">
        <circle cx="2.6" cy="10.2" r="1.3" />
        <path d="M5.8 10.2A3.2 3.2 0 0 0 2.6 7" />
        <path d="M8.6 10.2A6 6 0 0 0 2.6 4.2" class="faint" />
        <path d="M11.4 10.2A8.8 8.8 0 0 0 2.6 1.4" class="faint" />
      </svg> emissions
      <span class="lhint">— open a marked finding to run it</span>
    </p>
    <p v-if="!findings.length && basic" class="clean" data-testid="clean-plain">
      Nothing came up on this board. That means nothing was found by these
      checks — not that the board will pass a test lab. Clocks and switching
      nets still deserve a look by hand.
    </p>
    <p v-if="!findings.length && !basic" class="clean">
      Nothing flagged at the screening tier. That is a rank, not a guarantee —
      review clock and switching nets by hand.
    </p>
    <ol class="list">
      <li v-for="f in findings" :key="f.id">
        <button class="row" :class="{ sel: f.id === selectedId }"
                :data-testid="`finding-${f.id}`"
                @click="emit('select', f.id)">
          <span class="heat" :class="f.rule === 'diff-pair' ? 'pair'
                                   : f.rule === 'switch-node' ? 'sw'
                                   : f.rule === 'commutation-loop' ? 'loop' : f.severityLabel" />
          <span class="fid">{{ f.id }}</span>
          <span class="ftitle">{{ f.title }}</span>
          <!-- Which deep tools this finding supports. Drawn in the collapsed
               row because the buttons themselves live inside the detail, and a
               capability you have to open a finding to discover is one nobody
               finds. Indicative only: the row is already a button, so these
               cannot be buttons too. -->
          <span v-if="(f.solve || f.emit) && !basic" class="tools"
                :data-testid="`tools-${f.id}`" :title="toolTitle(f)">
            <svg v-if="f.solve" class="tool" viewBox="0 0 13 12" aria-hidden="true">
              <rect x="4" y="2" width="5" height="2.2" />
              <path d="M0.5 10.5h12" />
              <path d="M4 4.6C2.6 6.2 2.2 8.4 2.2 10.4" class="faint" />
              <path d="M9 4.6C10.4 6.2 10.8 8.4 10.8 10.4" class="faint" />
            </svg>
            <svg v-if="f.emit" class="tool" viewBox="0 0 13 12" aria-hidden="true">
              <circle cx="2.6" cy="10.2" r="1.3" />
              <path d="M5.8 10.2A3.2 3.2 0 0 0 2.6 7" />
              <path d="M8.6 10.2A6 6 0 0 0 2.6 4.2" class="faint" />
              <path d="M11.4 10.2A8.8 8.8 0 0 0 2.6 1.4" class="faint" />
            </svg>
          </span>
          <span v-if="basic" class="risk" :class="f.severityLabel"
                :data-testid="`risk-${f.id}`">{{ riskWord(f) }}</span>
          <span class="fnum">{{ rowNumber(f) }}</span>
          <span v-if="diffMap[f.id]" class="dbadge" :class="diffMap[f.id]"
                :data-testid="`diff-${f.id}`">{{ diffMap[f.id] === 'new' ? 'NEW' : 'WORSE' }}</span>
          <span class="dismiss" :data-testid="`dismiss-${f.id}`" role="button"
                title="dismiss this finding for this review"
                @click.stop="emit('hide', f.id)">✕</span>
        </button>
        <div v-if="f.id === selectedId" class="detail" data-testid="finding-detail">
          <!-- Guided: what it means, what to do, and an honest door to the
               engineering text — never a shortened version of the physics
               pretending to be the whole of it. -->
          <template v-if="basic && plain(f)">
            <p data-testid="plain-says">{{ plain(f).says }}</p>
            <p class="fix"><b>What to do:</b> {{ plain(f).do }}</p>
            <button class="why" :data-testid="`why-${f.id}`"
                    @click.stop="emit('glossary')">
              why this matters, and how it was measured →</button>
          </template>
          <template v-else>
            <p>{{ f.detail }}</p>
            <p class="fix"><b>Fix:</b> {{ f.remediation }}</p>
            <p class="tags">
              <span class="tag">{{ f.rule }}</span>
              <span class="tag">{{ f.confidence }}</span>
              <span v-if="f.minSepMm < 1e29 && f.minSepMm > 0" class="tag">min gap {{ f.minSepMm.toFixed(2) }} mm</span>
            </p>
          </template>
          <button v-if="f.solve" class="bench" :data-testid="`bench-${f.id}`"
                  @click.stop="emit('bench', f.id)">
            {{ basic ? 'How much noise actually lands on the other track? →'
                     : 'Solve this cross-section →' }}
          </button>
          <button v-if="f.emit" class="bench" :data-testid="`emit-${f.id}`"
                  @click.stop="emit('emissions', f.id)">
            {{ basic ? 'Would this pass an EMC test? →'
                     : 'Predict radiated emissions →' }}
          </button>
        </div>
      </li>
    </ol>
  </aside>
</template>

<style scoped>
.panel {
  border-left: 1px solid var(--resin-edge);
  background: var(--resin);
  overflow-y: auto; min-height: 0;
  padding: 12px 0;
}
.ptitle {
  font-family: var(--display); font-weight: 700; font-size: 15px;
  letter-spacing: 0.1em; text-transform: uppercase; color: var(--tin);
  padding: 0 14px 8px;
  display: flex; align-items: baseline; gap: 8px;
}
.count { font-family: var(--mono); font-size: 13px; color: var(--copper); }
.gloss { margin-left: auto; border: 1px solid var(--resin-edge); border-radius: 999px;
  padding: 1px 12px; font-size: 11px; color: var(--tin); text-transform: none;
  letter-spacing: 0; font-family: var(--sans); font-weight: 400; }
.gloss:hover { border-color: var(--copper); color: var(--copper); }
.restore { margin: 0 14px 8px; border: 1px dashed var(--resin-edge); border-radius: 999px;
  padding: 2px 12px; font-size: 11px; color: var(--tin); }
.restore:hover { border-color: var(--copper); color: var(--copper); }
.diffbar { display: flex; align-items: baseline; gap: 8px; flex-wrap: wrap;
  margin: 0 14px 8px; padding: 6px 10px; border-radius: 6px;
  border: 1px solid var(--resin-edge); font-family: var(--mono); font-size: 11px;
  color: var(--tin); }
.diffbar.regression { border-color: var(--heat-high); }
.diffbar.improved { border-color: var(--heat-low); }
.diffbar > b { color: var(--silk); font-weight: 600; }
.diffbar.regression span b { color: var(--heat-high); }
.diffbar.improved span b { color: var(--heat-low); }
.dchip { border: 1px solid var(--resin-edge); border-radius: 999px;
  padding: 0 9px; font-size: 10.5px; color: var(--tin); margin-left: auto; }
.dchip + .dchip { margin-left: 0; }
.dchip.on, .dchip:hover { border-color: var(--copper); color: var(--copper); }
.dres { width: 100%; font-size: 10px; opacity: 0.8; }
.dbadge { font-family: var(--mono); font-size: 9px; font-weight: 700;
  padding: 1px 5px; border-radius: 3px; flex: none; }
.dbadge.new { background: var(--heat-high); color: var(--board); }
.dbadge.worse { background: var(--heat-med, #ffb454); color: var(--board); }
.dismiss { color: var(--tin); opacity: 0; font-size: 11px; padding: 0 2px; flex: none; }
.row:hover .dismiss { opacity: 0.7; }
.dismiss:hover { opacity: 1; color: var(--heat-high); }

.tools { display: inline-flex; gap: 4px; align-items: center; flex: none; }
.tool {
  width: 13px; height: 12px; flex: none;
  fill: currentColor; stroke: currentColor;
  stroke-width: 1.1; stroke-linecap: round;
  color: var(--copper);
}
.tool path { fill: none; }
.tool .faint { stroke-opacity: 0.45; }
.legend {
  display: flex; align-items: center; gap: 5px; flex-wrap: wrap;
  padding: 0 14px 8px; font-family: var(--mono); font-size: 10.5px; color: var(--tin);
}
.legend .tool { margin-left: 6px; }
.legend .tool:first-child { margin-left: 0; }
.lhint { opacity: 0.7; }
.bench {
  margin-top: 8px; width: 100%; text-align: left;
  border: 1px solid var(--copper); border-radius: 4px;
  padding: 6px 10px; font-size: 12.5px; color: var(--copper);
}
.bench:hover { background: var(--copper); color: var(--bare-fr4); }
.of { font-family: var(--mono); font-size: 11px; color: var(--tin); letter-spacing: 0; }

.filters { display: flex; flex-wrap: wrap; gap: 5px; padding: 0 14px 10px; }
.rchip {
  font-family: var(--mono); font-size: 10.5px;
  padding: 2px 8px; border-radius: 999px;
  border: 1px solid var(--resin-edge); color: var(--tin);
}
.rchip b { color: var(--silk); font-weight: 500; }
.rchip:hover { border-color: var(--copper); }
.rchip.off { opacity: 0.4; border-style: dashed; }
.rchip.off b { color: var(--tin); }
.clean { padding: 4px 14px; color: var(--tin); font-size: 13px; }

.list { list-style: none; }
.row {
  /* five columns: heat, id, title, capability marks, value. The marks are
     conditional, and with only four columns declared their presence pushed the
     value onto a second grid row. */
  /* seven columns: heat, id, title, tools, value, diff-badge, dismiss — a
     conditional child beyond the declared columns wraps onto a second grid
     row (learned twice, now encoded) */
  /* eight: heat, id, title, tools, risk-word, value, diff-badge, dismiss.
     Every conditional child needs its own declared column — one beyond the
     template and the row silently wraps onto a second grid row (learned
     three times now, so the count is stated rather than counted). */
  display: grid; grid-template-columns: 10px auto 1fr auto auto auto auto auto;
  gap: 8px; align-items: center;
  width: 100%; text-align: left;
  padding: 8px 14px;
  border-top: 1px solid transparent; border-bottom: 1px solid transparent;
}
.row:hover { background: rgba(217, 139, 95, 0.07); }
.row.sel {
  background: rgba(217, 139, 95, 0.12);
  border-color: var(--resin-edge);
}
.heat { width: 8px; height: 8px; border-radius: 50%; }
.heat.high { background: var(--heat-high); box-shadow: 0 0 6px var(--heat-high); }
.heat.medium { background: var(--heat-med); box-shadow: 0 0 5px var(--heat-med); }
.heat.low { background: var(--heat-low); }
.heat.info { background: var(--tin); }
.heat.pair { background: #6f9fc4; }
.heat.sw { background: var(--copper); box-shadow: 0 0 6px var(--copper); }
.heat.loop { background: #e8d24a; box-shadow: 0 0 6px #e8d24a; }
.fid { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.ftitle {
  font-size: 12.5px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.fnum { font-family: var(--mono); font-size: 12px; color: var(--silk); white-space: nowrap; }
.risk { font-size: 10.5px; font-weight: 600; white-space: nowrap; letter-spacing: 0.02em; }
.risk.high { color: var(--heat-high); }
.risk.medium { color: var(--heat-med); }
.risk.low, .risk.info { color: var(--tin); }
.why { margin-top: 8px; font-size: 12px; color: var(--copper); text-align: left; }
.why:hover { text-decoration: underline; }

.detail {
  padding: 6px 14px 12px 32px;
  font-size: 12.5px; color: var(--tin);
  border-bottom: 1px solid var(--resin-edge);
}
.detail .fix { margin-top: 6px; color: var(--silk); }
.detail .fix b { color: var(--heat-low); font-weight: 600; }
.tags { margin-top: 7px; display: flex; gap: 6px; flex-wrap: wrap; }
.tag {
  font-family: var(--mono); font-size: 10.5px;
  border: 1px solid var(--resin-edge); border-radius: 3px;
  padding: 1px 6px; color: var(--tin);
}
</style>
