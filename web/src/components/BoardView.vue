<script setup>
import { ref, reactive, computed, watch, onMounted, onBeforeUnmount } from 'vue'

const props = defineProps({
  report: { type: Object, required: true },
  // already filtered by the rule chips — the board shows exactly what the list shows
  findings: { type: Array, required: true },
  selectedId: { type: String, default: '' },
})
const emit = defineEmits(['select'])

const wrap = ref(null)
const canvas = ref(null)
const view = reactive({ scale: 1, ox: 0, oy: 0 })
const layerVis = reactive({})
const overlaysOn = ref(true)
const hover = ref(null) // { x, y, kind, lines: [...] , findingId? }

const board = computed(() => props.report.board)
const findings = computed(() => props.findings)

const HEAT = { high: '#ff5d5d', medium: '#ffb454', low: '#58c79a', info: '#9db4ad' }
// intentional coupling and identified aggressors read as their own thing, not
// as heat: diff pairs cool blue-grey, switch nodes copper (the board's own hue)
const RULE_COLOR = { 'diff-pair': '#6f9fc4', 'switch-node': '#d98b5f',
                     'commutation-loop': '#e8d24a' }
const colorFor = f => RULE_COLOR[f.rule] ?? HEAT[f.severityLabel] ?? HEAT.info
const INNER_COLORS = ['#b8c24d', '#c778b8', '#5dc7b0', '#c7a15d']

function layerColor(cu) {
  const names = board.value.copperNames
  if (cu === 0) return '#e8955c'            // F.Cu: copper
  if (cu === names.length - 1) return '#5d9ec7' // B.Cu: tinned blue
  return INNER_COLORS[(cu - 1) % INNER_COLORS.length]
}

const z0map = computed(() => {
  const m = new Map()
  for (const e of props.report.z0Table ?? [])
    m.set(`${e.cu}:${e.widthMm.toFixed(4)}`, e.z0Ohm)
  return m
})

// ---- view transform ----
const toScreen = (x, y) => [(x - view.ox) * view.scale, (y - view.oy) * view.scale]
const toWorld = (sx, sy) => [sx / view.scale + view.ox, sy / view.scale + view.oy]

function fit() {
  const el = canvas.value
  if (!el) return
  const [x1, y1, x2, y2] = board.value.bbox
  const w = el.clientWidth, h = el.clientHeight
  view.scale = Math.min(w / (x2 - x1), h / (y2 - y1)) * 0.92
  view.ox = x1 - (w / view.scale - (x2 - x1)) / 2
  view.oy = y1 - (h / view.scale - (y2 - y1)) / 2
}

function zoomTo(f) {
  const el = canvas.value
  const pts = []
  for (const l of f.geom.lines) pts.push([l.x1, l.y1], [l.x2, l.y2])
  for (const m of f.geom.markers) pts.push(m)
  if (!pts.length) return
  let x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30
  for (const [x, y] of pts) {
    x1 = Math.min(x1, x); y1 = Math.min(y1, y)
    x2 = Math.max(x2, x); y2 = Math.max(y2, y)
  }
  const pad = 4
  x1 -= pad; y1 -= pad; x2 += pad; y2 += pad
  const w = el.clientWidth, h = el.clientHeight
  view.scale = Math.min(w / (x2 - x1), h / (y2 - y1), 40)
  view.ox = x1 - (w / view.scale - (x2 - x1)) / 2
  view.oy = y1 - (h / view.scale - (y2 - y1)) / 2
  draw()
}

// ---- drawing ----
function draw() {
  const el = canvas.value
  if (!el) return
  const dpr = window.devicePixelRatio || 1
  if (el.width !== el.clientWidth * dpr || el.height !== el.clientHeight * dpr) {
    el.width = el.clientWidth * dpr
    el.height = el.clientHeight * dpr
  }
  const ctx = el.getContext('2d')
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, el.clientWidth, el.clientHeight)

  const b = board.value
  // board substrate
  const [bx1, by1] = toScreen(b.bbox[0], b.bbox[1])
  const [bx2, by2] = toScreen(b.bbox[2], b.bbox[3])
  ctx.fillStyle = '#182019'
  ctx.strokeStyle = '#2c3a30'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.roundRect(bx1, by1, bx2 - bx1, by2 - by1, 3)
  ctx.fill()
  ctx.stroke()

  const nCu = b.copperNames.length
  // bottom -> top so F.Cu renders on top
  for (let cu = nCu - 1; cu >= 0; --cu) {
    if (!layerVis[b.copperNames[cu]]) continue
    const col = layerColor(cu)
    ctx.globalAlpha = 0.26
    ctx.fillStyle = col
    for (const z of b.zones) {
      if (z.cu !== cu) continue
      ctx.beginPath()
      z.pts.forEach(([x, y], i) => {
        const [sx, sy] = toScreen(x, y)
        i ? ctx.lineTo(sx, sy) : ctx.moveTo(sx, sy)
      })
      ctx.closePath()
      ctx.fill()
    }
    ctx.globalAlpha = 0.92
    ctx.strokeStyle = col
    ctx.lineCap = 'round'
    for (const s of b.segments) {
      if (s.cu !== cu) continue
      ctx.lineWidth = Math.max(s.w * view.scale, 0.6)
      ctx.beginPath()
      const [ax, ay] = toScreen(s.x1, s.y1)
      const [ex, ey] = toScreen(s.x2, s.y2)
      ctx.moveTo(ax, ay)
      ctx.lineTo(ex, ey)
      ctx.stroke()
    }
    ctx.fillStyle = col
    for (const p of b.pads) {
      if (p.cu !== cu && !(p.th && cu === 0)) continue
      const [px, py] = toScreen(p.x, p.y)
      ctx.fillRect(px - (p.w * view.scale) / 2, py - (p.h * view.scale) / 2,
                   p.w * view.scale, p.h * view.scale)
    }
    ctx.globalAlpha = 1
  }
  // vias
  for (const v of board.value.vias) {
    const [vx, vy] = toScreen(v.x, v.y)
    ctx.fillStyle = '#b9c4bf'
    ctx.beginPath(); ctx.arc(vx, vy, (v.size / 2) * view.scale, 0, 7); ctx.fill()
    ctx.fillStyle = '#101613'
    ctx.beginPath(); ctx.arc(vx, vy, (v.drill / 2) * view.scale, 0, 7); ctx.fill()
  }

  // risk overlays: heat on copper (the signature)
  if (overlaysOn.value) {
    for (const f of findings.value) {
      const col = colorFor(f)
      const dim = props.selectedId && f.id !== props.selectedId
      ctx.globalAlpha = dim ? 0.18 : 0.95
      ctx.shadowColor = col
      ctx.shadowBlur = f.id === props.selectedId ? 18 : 8
      ctx.strokeStyle = col
      ctx.lineCap = 'round'
      for (const l of f.geom.lines) {
        ctx.lineWidth = Math.max(l.w * view.scale + 2.5, 3.5)
        ctx.beginPath()
        const [ax, ay] = toScreen(l.x1, l.y1)
        const [ex, ey] = toScreen(l.x2, l.y2)
        ctx.moveTo(ax, ay); ctx.lineTo(ex, ey); ctx.stroke()
      }
      ctx.lineWidth = 2
      for (const [mx, my] of f.geom.markers) {
        const [sx, sy] = toScreen(mx, my)
        ctx.beginPath()
        ctx.moveTo(sx - 4, sy - 4); ctx.lineTo(sx + 4, sy + 4)
        ctx.moveTo(sx - 4, sy + 4); ctx.lineTo(sx + 4, sy - 4)
        ctx.stroke()
      }
      ctx.shadowBlur = 0
      ctx.globalAlpha = 1
    }
  }
}

// ---- hit testing / tooltips ----
function distToSeg(px, py, x1, y1, x2, y2) {
  const dx = x2 - x1, dy = y2 - y1
  const L2 = dx * dx + dy * dy
  const t = L2 ? Math.max(0, Math.min(1, ((px - x1) * dx + (py - y1) * dy) / L2)) : 0
  return Math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))
}

function hitTest(sx, sy) {
  const [wx, wy] = toWorld(sx, sy)
  const tolPx = 6 / view.scale
  // findings first (they sit on top)
  if (overlaysOn.value) {
    for (const f of findings.value) {
      for (const l of f.geom.lines)
        if (distToSeg(wx, wy, l.x1, l.y1, l.x2, l.y2) < l.w / 2 + tolPx)
          return { kind: 'finding', f }
      for (const [mx, my] of f.geom.markers)
        if (Math.hypot(wx - mx, wy - my) < tolPx * 1.5) return { kind: 'finding', f }
    }
  }
  const b = board.value
  for (let cu = 0; cu < b.copperNames.length; ++cu) {
    if (!layerVis[b.copperNames[cu]]) continue
    for (const s of b.segments)
      if (s.cu === cu && distToSeg(wx, wy, s.x1, s.y1, s.x2, s.y2) < s.w / 2 + tolPx)
        return { kind: 'segment', s }
  }
  for (const v of b.vias)
    if (Math.hypot(wx - v.x, wy - v.y) < v.size / 2 + tolPx) return { kind: 'via', v }
  return null
}

const netName = id => board.value.nets.find(n => n.id === id)?.name || `net ${id}`

function tooltipFor(hit) {
  if (hit.kind === 'finding') {
    const f = hit.f
    const lines = [`${f.id} · ${f.severityLabel.toUpperCase()} · ${f.rule}`, f.title]
    if (f.nextDb !== undefined) lines.push(`NEXT (saturated) ${f.nextDb.toFixed(1)} dB · ${f.coupledLenMm.toFixed(1)} mm`)
    else if (f.rule === 'switch-node') lines.push(`copper extent ${f.coupledLenMm.toFixed(0)} mm² · ${f.confidence}`)
    else if (f.rule === 'commutation-loop') lines.push(`enclosed loop ${f.coupledLenMm.toFixed(0)} mm² · ${f.confidence}`)
    else if (f.coupledLenMm) lines.push(`${f.coupledLenMm.toFixed(1)} mm`)
    lines.push('click to inspect')
    return { findingId: f.id, lines }
  }
  if (hit.kind === 'segment') {
    const s = hit.s
    const len = Math.hypot(s.x2 - s.x1, s.y2 - s.y1)
    const z0 = z0map.value.get(`${s.cu}:${s.w.toFixed(4)}`)
    const lines = [
      `${netName(s.net)} · ${board.value.copperNames[s.cu]}`,
      `w ${s.w.toFixed(2)} mm · seg ${len.toFixed(1)} mm`,
    ]
    lines.push(z0 !== undefined ? `Z₀ ≈ ${z0.toFixed(0)} Ω (screening est.)` : 'Z₀: no reference plane')
    return { lines }
  }
  const v = hit.v
  return { lines: [`via · ${netName(v.net)}`, `⌀${v.size.toFixed(2)} / drill ${v.drill.toFixed(2)} mm`] }
}

// ---- interaction ----
let panning = null
function onPointerDown(e) {
  panning = { x: e.clientX, y: e.clientY, ox: view.ox, oy: view.oy, moved: false }
  canvas.value.setPointerCapture(e.pointerId)
}
function onPointerMove(e) {
  const rect = canvas.value.getBoundingClientRect()
  const sx = e.clientX - rect.left, sy = e.clientY - rect.top
  if (panning) {
    const dx = (e.clientX - panning.x) / view.scale
    const dy = (e.clientY - panning.y) / view.scale
    if (Math.abs(e.clientX - panning.x) + Math.abs(e.clientY - panning.y) > 3) panning.moved = true
    view.ox = panning.ox - dx
    view.oy = panning.oy - dy
    draw()
    return
  }
  const hit = hitTest(sx, sy)
  hover.value = hit ? { x: sx, y: sy, ...tooltipFor(hit) } : null
  canvas.value.style.cursor = hit?.findingId !== undefined || (hit && hit.kind === 'finding') ? 'pointer' : 'grab'
}
function onPointerUp(e) {
  const wasPan = panning?.moved
  panning = null
  if (wasPan) return
  const rect = canvas.value.getBoundingClientRect()
  const hit = hitTest(e.clientX - rect.left, e.clientY - rect.top)
  emit('select', hit?.kind === 'finding' ? hit.f.id : '')
}
function onWheel(e) {
  e.preventDefault()
  const rect = canvas.value.getBoundingClientRect()
  const sx = e.clientX - rect.left, sy = e.clientY - rect.top
  const [wx, wy] = toWorld(sx, sy)
  view.scale *= Math.exp(-e.deltaY * 0.0012)
  view.scale = Math.min(Math.max(view.scale, 0.5), 400)
  view.ox = wx - sx / view.scale
  view.oy = wy - sy / view.scale
  draw()
}

// ---- lifecycle ----
let ro = null
onMounted(() => {
  for (const n of board.value.copperNames) layerVis[n] = true
  fit()
  draw()
  ro = new ResizeObserver(() => { draw() })
  ro.observe(canvas.value)
})
onBeforeUnmount(() => ro?.disconnect())

watch(() => props.report, () => {
  for (const n of board.value.copperNames)
    if (layerVis[n] === undefined) layerVis[n] = true
  fit()
  draw()
})
watch(() => props.selectedId, id => {
  const f = findings.value.find(x => x.id === id)
  if (f) zoomTo(f)
  else draw()
})
watch([layerVis, overlaysOn], () => draw())
watch(() => props.findings, () => draw())
</script>

<template>
  <div ref="wrap" class="boardwrap">
    <canvas ref="canvas" data-testid="board-canvas"
            @pointerdown="onPointerDown" @pointermove="onPointerMove"
            @pointerup="onPointerUp" @wheel="onWheel"
            @pointerleave="hover = null" />
    <div class="chips">
      <button v-for="(name, cu) in board.copperNames" :key="name" class="lchip"
              :class="{ off: !layerVis[name] }"
              :style="{ '--c': layerColor(cu) }"
              @click="layerVis[name] = !layerVis[name]">{{ name }}</button>
      <button class="lchip risk" :class="{ off: !overlaysOn }" data-testid="overlay-toggle"
              @click="overlaysOn = !overlaysOn">risk overlay</button>
    </div>
    <div v-if="hover" class="tooltip" data-testid="board-tooltip"
         :style="{ left: Math.min(hover.x + 14, 9999) + 'px', top: hover.y + 14 + 'px' }">
      <div v-for="(l, i) in hover.lines" :key="i" :class="{ head: i === 0 }">{{ l }}</div>
    </div>
  </div>
</template>

<style scoped>
.boardwrap { position: relative; min-height: 0; overflow: hidden; }
canvas { width: 100%; height: 100%; display: block; touch-action: none; }

.chips {
  position: absolute; top: 10px; left: 10px;
  display: flex; gap: 6px; flex-wrap: wrap;
}
.lchip {
  font-family: var(--mono); font-size: 11px;
  padding: 3px 10px; border-radius: 999px;
  border: 1px solid var(--c, var(--tin)); color: var(--c, var(--tin));
  background: rgba(16, 22, 19, 0.82);
}
.lchip.off { opacity: 0.35; border-style: dashed; }
.lchip.risk { --c: var(--heat-high); }

.tooltip {
  position: absolute; pointer-events: none; z-index: 10;
  background: rgba(23, 31, 26, 0.96);
  border: 1px solid var(--resin-edge); border-radius: 5px;
  padding: 7px 10px; max-width: 340px;
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
  box-shadow: 0 4px 18px rgba(0, 0, 0, 0.5);
}
.tooltip .head { color: var(--silk); margin-bottom: 2px; }
</style>
