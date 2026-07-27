<script setup>
const props = defineProps({
  findings: { type: Array, required: true },
  report: { type: Object, required: true },
  selectedId: { type: String, default: '' },
  rules: { type: Array, default: () => [] },
  hiddenRules: { type: Set, default: () => new Set() },
  total: { type: Number, default: 0 },
})
const emit = defineEmits(['select', 'toggleRule'])

const netName = id =>
  props.report.board.nets.find(n => n.id === id)?.name || (id >= 0 ? `net ${id}` : '')
</script>

<template>
  <aside class="panel">
    <h2 class="ptitle">
      Findings
      <span class="count" data-testid="finding-count">{{ findings.length }}</span>
      <span v-if="findings.length !== total" class="of">of {{ total }}</span>
    </h2>
    <div v-if="rules.length > 1" class="filters" data-testid="rule-filters">
      <button v-for="[rule, n] in rules" :key="rule" class="rchip"
              :class="{ off: hiddenRules.has(rule) }"
              :data-testid="`rule-${rule}`"
              :aria-pressed="!hiddenRules.has(rule)"
              @click="emit('toggleRule', rule)">{{ rule }} <b>{{ n }}</b></button>
    </div>
    <p v-if="!findings.length" class="clean">
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
          <span class="fnum" v-if="f.nextDb !== undefined">{{ f.nextDb.toFixed(1) }} dB</span>
          <span class="fnum" v-else-if="f.rule === 'switch-node' || f.rule === 'commutation-loop'">{{ f.coupledLenMm.toFixed(0) }} mm²</span>
          <span class="fnum" v-else-if="f.coupledLenMm">{{ f.coupledLenMm.toFixed(1) }} mm</span>
        </button>
        <div v-if="f.id === selectedId" class="detail" data-testid="finding-detail">
          <p>{{ f.detail }}</p>
          <p class="fix"><b>Fix:</b> {{ f.remediation }}</p>
          <p class="tags">
            <span class="tag">{{ f.rule }}</span>
            <span class="tag">{{ f.confidence }}</span>
            <span v-if="f.minSepMm < 1e29 && f.minSepMm > 0" class="tag">min gap {{ f.minSepMm.toFixed(2) }} mm</span>
          </p>
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
  display: grid; grid-template-columns: 10px auto 1fr auto;
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
.fnum { font-family: var(--mono); font-size: 12px; color: var(--silk); }

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
