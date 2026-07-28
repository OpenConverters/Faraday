<script setup>
import { RULES, TOOLS } from '../glossary.js'

const props = defineProps({
  // rules present on the current board, with counts — glossary rows for rules
  // the board actually triggered get their toggle; the rest are reference
  ruleCounts: { type: Object, default: () => ({}) },
  hiddenRules: { type: Set, default: () => new Set() },
})
const emit = defineEmits(['close', 'toggleRule'])
</script>

<template>
  <div class="scrim" @click.self="emit('close')">
    <section class="panel" data-testid="glossary" role="dialog" aria-label="Glossary">
      <header>
        <h2>GLOSSARY</h2>
        <span class="sub">everything Faraday can find, and what each means</span>
        <div class="sp" />
        <button class="x" @click="emit('close')" aria-label="Close">✕</button>
      </header>

      <div class="list">
        <article v-for="r in RULES" :key="r.id" class="entry"
                 :data-testid="`gloss-${r.id}`">
          <div class="head">
            <h3>{{ r.name }} <code>{{ r.id }}</code></h3>
            <span v-if="ruleCounts[r.id]" class="count">{{ ruleCounts[r.id] }} on this board</span>
            <button v-if="ruleCounts[r.id]" class="vis"
                    :class="{ off: hiddenRules.has(r.id) }"
                    :data-testid="`gloss-toggle-${r.id}`"
                    @click="emit('toggleRule', r.id)">
              {{ hiddenRules.has(r.id) ? 'show' : 'hide' }}</button>
          </div>
          <p><b>What:</b> {{ r.what }}</p>
          <p><b>Physics:</b> {{ r.physics }}</p>
          <p><b>Fix:</b> {{ r.fix }}</p>
          <p class="conf">confidence: {{ r.confidence }}</p>
        </article>

        <h3 class="tools-h">The deep tools</h3>
        <article v-for="t in TOOLS" :key="t.id" class="entry tool">
          <div class="head"><h3>{{ t.name }}</h3></div>
          <p>{{ t.what }}</p>
        </article>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 50; background: rgba(8,12,10,0.72);
  display: flex; align-items: center; justify-content: center; padding: 20px; }
.panel { width: min(820px, 100%); max-height: 100%; display: flex; flex-direction: column;
  overflow: hidden; background: var(--resin); border: 1px solid var(--resin-edge);
  border-radius: 8px; }
header { display: flex; align-items: baseline; gap: 12px; padding: 11px 16px;
  border-bottom: 1px solid var(--resin-edge); flex: none; }
header h2 { font-family: var(--display); font-size: 17px; font-weight: 700;
  letter-spacing: 0.16em; color: var(--copper); }
.sub { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.sp { flex: 1; }
.x { color: var(--tin); font-size: 15px; padding: 0 4px; }
.x:hover { color: var(--silk); }

.list { overflow-y: auto; padding: 12px 16px; }
.entry { border-bottom: 1px solid var(--resin-edge); padding: 10px 0; }
.entry .head { display: flex; align-items: baseline; gap: 10px; }
.entry h3 { font-size: 13.5px; font-weight: 600; }
.entry code { font-family: var(--mono); font-size: 11px; color: var(--copper);
  font-weight: 400; }
.count { font-family: var(--mono); font-size: 11px; color: var(--tin); }
.vis { margin-left: auto; border: 1px solid var(--resin-edge); border-radius: 999px;
  padding: 1px 12px; font-size: 11px; color: var(--silk); }
.vis.off { color: var(--tin); border-style: dashed; }
.vis:hover { border-color: var(--copper); }
.entry p { font-size: 12px; color: var(--tin); margin-top: 4px; }
.entry p b { color: var(--silk); font-weight: 600; }
.conf { font-family: var(--mono); font-size: 10.5px; opacity: 0.8; }
.tools-h { font-family: var(--display); font-size: 12.5px; letter-spacing: 0.09em;
  text-transform: uppercase; color: var(--tin); padding: 14px 0 2px; }
.entry.tool p { color: var(--tin); }
</style>
