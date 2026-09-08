<script setup>
import { ref, reactive, computed, watch, onMounted, onBeforeUnmount } from 'vue'
import { isPart, footprintName } from '../parts.js'
import { palette, onThemeChange } from '../theme.js'

const props = defineProps({
  report: { type: Object, required: true },
  // per-segment return-path quality (effective loop height), or null
  returnPath: { type: Object, default: null },
  // component near-field result, or null. Rendered as a heat wash UNDER the
  // copper: the field is what is in the air above the board, and putting the
  // copper on top is what lets you see which parts sit in the hot region.
  nearField: { type: Object, default: null },
  shields: { type: Array, default: () => [] },
  drawingShield: { type: Boolean, default: false },
  // The near-field map is built around switching aggressors. Without one it
  // has nothing to say, and the chip should show that rather than erroring.
  hasSwitchNode: { type: Boolean, default: true },
  swCandidateCount: { type: Number, default: 0 },
  // already filtered by the rule chips — the board shows exactly what the list shows
  findings: { type: Array, required: true },
  selectedId: { type: String, default: '' },
  // the part whose inspector is open, drawn brighter than the rest
  selectedPart: { type: String, default: '' },
  // ref -> {state} from the catalogue sweep, or null when it has not been run.
  // Turning this on recolours the parts by what the catalogue can resolve.
  partIndex: { type: Object, default: null },
  sweeping: { type: Boolean, default: false },
})
const emit = defineEmits(['select', 'toggleReturnPath', 'nearField', 'shield', 'pdn', 'part',
                          'sweep'])

// Radiation attribution, decoded once per map into a byte per segment. Null
// when the map is off, and every draw path checks for that rather than
// branching on a mode flag in five places.
const heat = computed(() => {
  const b64 = props.returnPath?.heat
  if (!b64) return null
  const bin = atob(b64)
  const a = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i)
  return a
})

// Cool copper through to white-hot. Below a tenth of the peak a trace is not
// part of the answer, so it stays dark rather than tinting the whole board.
// The quiet end has to stay VISIBLE. A ramp that fades to near-black hides the
// copper it is describing, and the reader cannot tell "this trace is quiet"
// from "this trace was not analysed" — which is how the first version of this
// map came out looking like nothing had happened.
// Four stops at 0 / 0.5 / 0.8 / 1. The shape of the ramp is the same in both
// themes — quiet teal, warming, hot — but its far end runs AWAY from the
// substrate: white-hot over a dark board, ember-dark over a pale one. Ending
// pale on a pale ground would make the loudest traces the hardest to see.
const RAMP = {
  dark:  [[78, 148, 132], [138, 170, 110], [217, 139, 95], [255, 237, 232]],
  light: [[26, 112, 88],  [86, 122, 52],   [170, 84, 18],  [136, 12, 24]],
}
function radColour(v, light) {
  const st = light ? RAMP.light : RAMP.dark
  const t = v / 255
  const [i, u] = t < 0.5 ? [0, t / 0.5]
               : t < 0.8 ? [1, (t - 0.5) / 0.3]
                         : [2, (t - 0.8) / 0.2]
  const a = st[i], b = st[i + 1]
  return `rgb(${Math.round(a[0] + (b[0] - a[0]) * u)},` +
         `${Math.round(a[1] + (b[1] - a[1]) * u)},` +
         `${Math.round(a[2] + (b[2] - a[2]) * u)})`
}

const wrap = ref(null)
const canvas = ref(null)
const view = reactive({ scale: 1, ox: 0, oy: 0 })
const layerVis = reactive({})
const overlaysOn = ref(true)
// The parts layer: every component as a body over its pads. The IR carries
// no courtyard (no importer reads one), so a body is the union of the part's
// pads plus a small margin — a black box in the literal sense, and enough to
// be pointed at. Parts with no pads (holes, fiducials, logos) are not parts.
const partsOn = ref(true)
// Shield-can rubber band. Declared with the rest of the reactive state rather
// than beside its handlers, because draw() reads it and a `const` further down
// the file is in the temporal dead zone when the first draw fires.
const drag = reactive({ on: false, x0: 0, y0: 0, x1: 0, y1: 0 })
const hover = ref(null) // { x, y, kind, lines: [...] , findingId? }

const board = computed(() => props.report.board)
const findings = computed(() => props.findings)

const parts = computed(() => {
  const b = board.value
  const byRef = new Map()
  for (const p of b.pads) {
    if (!byRef.has(p.component)) byRef.set(p.component, [])
    byRef.get(p.component).push(p)
  }
  const out = []
  const last = b.copperNames.length - 1
  for (const c of b.components ?? []) {
    const pads = byRef.get(c.ref) ?? []
    if (!isPart(c, pads)) continue
    let x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30
    let top = 0, bottom = 0, th = 0
    for (const p of pads) {
      x1 = Math.min(x1, p.x - p.w / 2); y1 = Math.min(y1, p.y - p.h / 2)
      x2 = Math.max(x2, p.x + p.w / 2); y2 = Math.max(y2, p.y + p.h / 2)
      if (p.th) th++
      else if (p.cu === 0) top++
      else if (p.cu === last) bottom++
    }
    const m = 0.15
    const side = th && !top && !bottom ? 'through' : bottom > top ? 'bottom' : 'top'
    out.push({ ...c, pads, side, x1: x1 - m, y1: y1 - m, x2: x2 + m, y2: y2 + m,
               area: (x2 - x1 + 2 * m) * (y2 - y1 + 2 * m) })
  }
  // smallest first, so a capacitor sitting inside a connector's box is the
  // one under the pointer, not the connector
  out.sort((a, b) => a.area - b.area)
  return out
})
const partVisible = p =>
  p.side === 'bottom' ? layerVis[board.value.copperNames[board.value.copperNames.length - 1]]
                      : layerVis[board.value.copperNames[0]]

// severity and rule identity: the meaning never changes, the value does — see
// palette() and the --heat-*/--cu-* tokens in style.css
// The catalogue overlay's four answers. "unlookupable" is deliberately its own
// colour and not a failure: a part the EXPORT never described is not a part the
// catalogue is missing, and colouring them alike would blame the wrong thing.
// The four verdicts keep their own hues in both themes — an answer does not
// change colour because the lights came on. Only the two "no answer yet"
// states follow the ground, because they ARE the ground: a part nothing can be
// said about is left as substrate.
const catColors = pal => ({
  exact:        { fill: pal.low(0.30),  line: pal.heat.low },
  candidates:   { fill: pal.med(0.24),  line: pal.heat.medium },
  none:         { fill: pal.high(0.16),   line: pal.high(0.65) },
  unlookupable: { fill: pal.wash,                 line: pal.washLine(0.45) },
  pending:      { fill: pal.wash,                 line: pal.washLine(0.35) },
  // not in the catalogue, but the librarian read it and staged it: a different
  // answer from "none", and the part must stop reading as unknown
  sourced:      { fill: 'rgba(111,159,196,0.26)', line: pal.cool },
  // the reader said which part this is. Answered, so it reads as answered —
  // its own state rather than "exact", because a person deciding is not the
  // catalogue matching a part number and the tally must not claim it was.
  chosen:       { fill: pal.low(0.30),  line: pal.heat.low },
})
// intentional coupling and identified aggressors read as their own thing, not
// as heat: diff pairs cool blue-grey, switch nodes copper (the board's own hue)
const ruleColors = pal => ({ 'diff-pair': pal.cool, 'switch-node': pal.copper,
                             'commutation-loop': pal.loop })
const colorFor = (f, pal) =>
  ruleColors(pal)[f.rule] ?? pal.heat[f.severityLabel] ?? pal.heat.info

// the layer chips are CSS, so they take the variable itself and follow a
// theme flip without a repaint
function layerVar(cu) {
  const names = board.value.copperNames
  if (cu === 0) return 'var(--cu-front)'
  if (cu === names.length - 1) return 'var(--cu-back)'
  return `var(--cu-in${((cu - 1) % 4) + 1})`
}
function layerColor(cu, pal) {
  const names = board.value.copperNames
  if (cu === 0) return pal.cuFront            // F.Cu: copper
  if (cu === names.length - 1) return pal.cuBack // B.Cu: tinned
  return pal.inner[(cu - 1) % pal.inner.length]
}

const z0map = computed(() => {
  const m = new Map()
  for (const e of props.report.z0Table ?? [])
    m.set(`${e.cu}:${e.widthMm.toFixed(4)}`, e.z0Ohm)
  return m
})

// ---- view transform ----
const toScreen = (x, y) => [(x - view.ox) * view.scale, (y - view.oy) * view.scale]
const invScreen = (sx, sy) => [sx / view.scale + view.ox, sy / view.scale + view.oy]
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
// |H| from a filamentary polygon loop, A/m. Mirrors nf::h_loop in
// NearField.hpp, which is pinned against the point-dipole limit.
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

// The near-field heat wash, drawn before any copper.
// The field image depends only on (result object, transform, size) — during
// a rubber-band drag none of those change, so the grid must not recompute
// per mousemove (user: "why is it so slow drawing the can?").
const nfCache = { key: null, nfd: null, img: null }
let themeOff = null

function drawNearField(ctx, w, h, toScreen, invScreen, pal) {
  const nfd = props.nearField
  if (!nfd || !nfd.aggressors?.length) return
  const cacheKey = `${view.scale}|${view.ox}|${view.oy}|${w}|${h}`
  if (nfCache.nfd === nfd && nfCache.key === cacheKey && nfCache.img) {
    ctx.imageSmoothingEnabled = true
    ctx.imageSmoothingQuality = 'high'
    ctx.drawImage(nfCache.img, 0, 0, w, h)
    drawNearFieldVectors(ctx, toScreen, pal)
    return
  }
  const step = 5
  const nx = Math.ceil(w / step), ny = Math.ceil(h / step)
  const z = nfd.probeHeightMm
  const cur = nfd.ringCurrentA ?? 2
  // Drawn cans attenuate the MAP by the same rule as the victim table (a can
  // between point and aggressor applies its SE; around both or neither it
  // does nothing) — the colours and the numbers must tell one story.
  const cans = nfd.shields ?? []
  const inCan = (c, x, y) =>
    x >= Math.min(c.x1, c.x2) && x <= Math.max(c.x1, c.x2) &&
    y >= Math.min(c.y1, c.y2) && y <= Math.max(c.y1, c.y2)
  const centroid = a => {
    // an aggressor with no hull contributes no field (hLoop returns 0) — since
    // ABT #798 every aggressor carries one, including inductors, so this is
    // now a guard rather than a routine case
    if (!a.hull || a.hull.length < 3) return null
    let cx = 0, cy = 0
    for (const [hx, hy] of a.hull) { cx += hx; cy += hy }
    return [cx / a.hull.length, cy / a.hull.length]
  }
  const aggC = nfd.aggressors.map(centroid)
  const f = new Float64Array(nx * ny)
  let peak = 0
  for (let iy = 0; iy < ny; iy++)
    for (let ix = 0; ix < nx; ix++) {
      const [mx, my] = invScreen(ix * step, iy * step)
      let p = 0
      for (let ai = 0; ai < nfd.aggressors.length; ai++) {
        // per-aggressor current: a commutation loop carries the ring current,
        // an inductor carries it derated by its construction. One current for
        // everything drew the inductor as loud as the loop (ABT #798).
        const acur = nfd.aggressors[ai].currentA ?? cur
        let v = hLoop(nfd.aggressors[ai].hull, mx, my, z, acur)
        let se = 0
        if (aggC[ai])
          for (const c of cans)
            if (inCan(c, mx, my) !== inCan(c, aggC[ai][0], aggC[ai][1]))
              se = Math.max(se, c.seDb)
        if (se > 0) v *= Math.pow(10, -se / 20)
        p += v * v
      }
      const v = Math.sqrt(p)
      f[iy * nx + ix] = v
      if (v > peak) peak = v
    }
  if (!(peak > 0)) return
  const img = ctx.createImageData(nx, ny)
  const floor = peak * 1e-3
  for (let i = 0; i < nx * ny; i++) {
    const v = f[i]
    const t = v > floor ? Math.log(v / floor) / Math.log(peak / floor) : 0
    let r, g, b
    if (t < 0.5) { const u = t / 0.5; r = 14 + 40 * u; g = 20 + 78 * u; b = 18 + 70 * u }
    else if (t < 0.8) { const u = (t - 0.5) / 0.3; r = 54 + 163 * u; g = 98 + 41 * u; b = 88 + 7 * u }
    else { const u = (t - 0.8) / 0.2; r = 217 + 38 * u; g = 139 + 98 * u; b = 95 + 137 * u }
    const o = i * 4
    img.data[o] = r; img.data[o + 1] = g; img.data[o + 2] = b; img.data[o + 3] = 255
  }
  const off = document.createElement('canvas')
  off.width = nx; off.height = ny
  off.getContext('2d').putImageData(img, 0, 0)
  nfCache.nfd = nfd; nfCache.key = cacheKey; nfCache.img = off
  // test hook: how many times the full Biot-Savart grid was computed — the
  // cache invariant is "a 40-move drag adds at most one", load-independent
  canvas.value.dataset.nfComputes =
    String(1 + Number(canvas.value.dataset.nfComputes || 0))
  ctx.imageSmoothingEnabled = true
  ctx.imageSmoothingQuality = 'high'
  ctx.drawImage(off, 0, 0, w, h)
  drawNearFieldVectors(ctx, toScreen, pal)
}

// the loops the field is integrated over, and the victims sitting in it —
// cheap vector work, drawn fresh on every frame on top of the cached image
function drawNearFieldVectors(ctx, toScreen, pal) {
  const nfd = props.nearField
  for (const a of nfd.aggressors) {
    if (!a.hull || a.hull.length < 3) continue
    ctx.strokeStyle = pal.glare(0.75)
    ctx.lineWidth = 1.4
    ctx.beginPath()
    a.hull.forEach(([hx, hy], i) => {
      const [sx, sy] = toScreen(hx, hy)
      i ? ctx.lineTo(sx, sy) : ctx.moveTo(sx, sy)
    })
    ctx.closePath(); ctx.stroke()
  }
  for (const v of (nfd.victims ?? []).slice(0, 20)) {
    const [sx, sy] = toScreen(v.xMm, v.yMm)
    ctx.strokeStyle = v.level === 'over' ? pal.heat.high
                    : v.level === 'watch' ? pal.heat.medium : pal.heat.low
    ctx.lineWidth = 2
    ctx.beginPath(); ctx.arc(sx, sy, 6, 0, 7); ctx.stroke()
  }
}

function draw() {
  const el = canvas.value
  if (!el) return
  const pal = palette(el)
  const cat = catColors(pal)
  // deterministic hook for tests: the current board->CSS-pixel transform
  el.dataset.view = JSON.stringify({ scale: view.scale, ox: view.ox, oy: view.oy })
  // and the risk ramp's own ends, which no pixel count can isolate — the hot
  // end covers too little copper to show up in a histogram, and the property
  // that matters (it runs away from the substrate) is a statement about the
  // ramp, not about this board
  el.dataset.ramp = JSON.stringify(
    { quiet: radColour(0, pal.light), hot: radColour(255, pal.light),
      board: pal.board })
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
  ctx.fillStyle = pal.board
  ctx.strokeStyle = pal.edge
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.roundRect(bx1, by1, bx2 - bx1, by2 - by1, 3)
  ctx.fill()
  ctx.stroke()

  // Near-field heat wash goes down FIRST, so the copper draws on top of it and
  // you can see which parts sit in the hot region — that is the whole point of
  // putting it on the board rather than in a panel of its own.
  if (props.nearField) {
    ctx.save()
    ctx.beginPath()
    ctx.rect(bx1, by1, bx2 - bx1, by2 - by1)
    ctx.clip()
    drawNearField(ctx, el.clientWidth, el.clientHeight, toScreen, invScreen, pal)
    ctx.restore()
  }

  // shield cans, drawn over everything so they read as an enclosure
  const paintShields = () => {
    const boxes = [...props.shields]
    if (drag.on) boxes.push({ x1: drag.x0, y1: drag.y0, x2: drag.x1, y2: drag.y1 })
    for (const sh of boxes) {
      const [ax, ay] = toScreen(Math.min(sh.x1, sh.x2), Math.min(sh.y1, sh.y2))
      const [cx, cy] = toScreen(Math.max(sh.x1, sh.x2), Math.max(sh.y1, sh.y2))
      const w2 = cx - ax, h2 = cy - ay
      // a translucent grey lid, so it reads as metal over the copper rather
      // than as a selection rectangle
      ctx.save()
      ctx.beginPath(); ctx.rect(ax, ay, w2, h2); ctx.clip()
      ctx.fillStyle = 'rgba(150,158,162,0.32)'
      ctx.fillRect(ax, ay, w2, h2)
      // brushed-metal hatch
      ctx.strokeStyle = pal.glare(0.16)
      ctx.lineWidth = 1
      for (let x = ax - h2; x < cx; x += 7) {
        ctx.beginPath(); ctx.moveTo(x, cy); ctx.lineTo(x + h2, ay); ctx.stroke()
      }
      // lid highlight along the top edge
      const grad = ctx.createLinearGradient(ax, ay, ax, ay + Math.min(18, h2))
      grad.addColorStop(0, pal.glare(0.22))
      grad.addColorStop(1, pal.glare(0))
      ctx.fillStyle = grad
      ctx.fillRect(ax, ay, w2, Math.min(18, h2))
      ctx.restore()
      ctx.strokeStyle = pal.glare(0.7)
      ctx.lineWidth = 1.6
      ctx.strokeRect(ax, ay, w2, h2)
    }
  }

  const nCu = b.copperNames.length
  // bottom -> top so F.Cu renders on top
  for (let cu = nCu - 1; cu >= 0; --cu) {
    if (!layerVis[b.copperNames[cu]]) continue
    const col = layerColor(cu, pal)
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
    const h = heat.value
    for (let i = 0; i < b.segments.length; i++) {
      const s = b.segments[i]
      if (s.cu !== cu) continue
      // In return-path mode the copper is coloured by its effective loop
      // height — how far away its return current really is — drawn a little
      // heavier so the hot traces read at board zoom.
      if (h) {
        ctx.strokeStyle = radColour(h[i] ?? 0, pal.light)
        ctx.lineWidth = Math.max(s.w * view.scale, h[i] > 150 ? 2.2 : 1.1)
      } else {
        ctx.lineWidth = Math.max(s.w * view.scale, 0.6)
      }
      ctx.beginPath()
      const [ax, ay] = toScreen(s.x1, s.y1)
      const [ex, ey] = toScreen(s.x2, s.y2)
      ctx.moveTo(ax, ay)
      ctx.lineTo(ex, ey)
      ctx.stroke()
    }
    ctx.strokeStyle = col
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
    ctx.fillStyle = pal.ink
    ctx.beginPath(); ctx.arc(vx, vy, (v.size / 2) * view.scale, 0, 7); ctx.fill()
    ctx.fillStyle = pal.inkInv
    ctx.beginPath(); ctx.arc(vx, vy, (v.drill / 2) * view.scale, 0, 7); ctx.fill()
  }

  // parts: a body over the pads, the reference on it once there is room
  let drawn = 0
  if (partsOn.value) {
    ctx.font = '10px ' + getComputedStyle(el).getPropertyValue('--mono')
    ctx.textAlign = 'center'
    ctx.textBaseline = 'middle'
    for (const p of parts.value) {
      if (!partVisible(p)) continue
      const [ax, ay] = toScreen(p.x1, p.y1)
      const [cx, cy] = toScreen(p.x2, p.y2)
      const w = cx - ax, h = cy - ay
      const sel = p.ref === props.selectedPart
      // In catalogue mode a part is coloured by what the catalogue can say
      // about it; otherwise by which side it is on.
      const idx = props.partIndex ? props.partIndex[p.ref] : null
      let fill = pal.wash
      let line = p.side === 'bottom' ? 'rgba(93,158,199,0.75)' : pal.washLine(0.6)
      if (props.partIndex) {
        // an unknown state must not take the board down with it: a new state
        // added upstream reads as "not answered yet" until it has a colour
        const c = cat[idx?.state] ?? cat.pending
        fill = c.fill
        line = c.line
      }
      ctx.fillStyle = sel ? pal.cu(0.28) : fill
      ctx.strokeStyle = sel ? pal.cuFront : line
      ctx.lineWidth = sel ? 2 : (props.partIndex && idx?.state === 'exact' ? 1.6 : 1)
      if (sel) { ctx.shadowColor = pal.cuFront; ctx.shadowBlur = 14 }
      ctx.beginPath()
      ctx.roundRect(ax, ay, w, h, Math.min(3, w / 4, h / 4))
      ctx.fill(); ctx.stroke()
      ctx.shadowBlur = 0
      if (w >= 22 && h >= 11) {
        ctx.fillStyle = sel ? pal.silk : pal.glare(0.85)
        ctx.save()
        ctx.beginPath(); ctx.rect(ax, ay, w, h); ctx.clip()
        ctx.fillText(p.ref, ax + w / 2, ay + h / 2)
        ctx.restore()
      }
      drawn++
    }
  }
  el.dataset.parts = String(drawn)

  paintShields()

  // risk overlays: heat on copper (the signature)
  if (overlaysOn.value) {
    for (const f of findings.value) {
      const col = colorFor(f, pal)
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
const CAT_SAYS = {
  exact: i => `in the catalogue as ${i.hit.row.mpn}`,
  candidates: i => i.by === 'mpn'
    ? `${i.count} catalogue part number(s) contain this one`
    : `${i.count} catalogue part(s) of this value fit this footprint`,
  none: i => i.why,
  unlookupable: i => i.why,
  pending: () => 'not looked up yet',
  sourced: i => `sourced from the web as ${i.sourcedMpn} — staged, not yet in the catalogue`,
}

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
  if (partsOn.value) {
    for (const p of parts.value)
      if (partVisible(p) && wx >= p.x1 && wx <= p.x2 && wy >= p.y1 && wy <= p.y2)
        return { kind: 'part', p }
  }
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
  if (hit.kind === 'part') {
    const p = hit.p
    const lines = [
      `${p.ref} · ${p.value || p.partNumber || 'no value in the export'}`,
      `${footprintName(p.footprint) || 'no footprint name'} · ${p.pads.length} pin(s) · ${p.side}`,
    ]
    const idx = props.partIndex ? props.partIndex[p.ref] : null
    if (props.partIndex) lines.push(CAT_SAYS[idx?.state ?? 'pending'](idx))
    lines.push('click to inspect the part')
    return { part: p.ref, lines }
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
  // while arming/drawing a shield the crosshair must win — the inline
  // pointer/grab assignment below was overriding the CSS class (user: "it
  // looks like a hand and it is weird")
  const hit = hitTest(sx, sy)
  hover.value = hit ? { x: sx, y: sy, ...tooltipFor(hit) } : null
  canvas.value.style.cursor =
    hit?.findingId !== undefined || (hit && (hit.kind === 'finding' || hit.kind === 'part'))
      ? 'pointer' : 'grab'
}
function onPointerUp(e) {
  const wasPan = panning?.moved
  panning = null
  if (wasPan) return
  const rect = canvas.value.getBoundingClientRect()
  const hit = hitTest(e.clientX - rect.left, e.clientY - rect.top)
  if (hit?.kind === 'part') { emit('part', hit.p.ref); return }
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
// ---- drawing a shield can ----
// Panning and drawing are mutually exclusive: sharing the pointer meant a drag
// panned the board while the rubber band was measured against a moving
// transform, so the rectangle landed somewhere else entirely.
function shieldDown(e) {
  if (!props.drawingShield) return
  // Capture the pointer, or a drag that leaves the canvas stops delivering
  // moves here and the rectangle collapses to a point, which the size guard
  // then silently rejects.
  try { e.currentTarget.setPointerCapture(e.pointerId) } catch { /* not fatal */ }
  const r = canvas.value.getBoundingClientRect()
  const [mx, my] = invScreen(e.clientX - r.left, e.clientY - r.top)
  drag.on = true; drag.x0 = mx; drag.y0 = my; drag.x1 = mx; drag.y1 = my
}
function shieldMove(e) {
  if (!drag.on) return
  const r = canvas.value.getBoundingClientRect()
  const [mx, my] = invScreen(e.clientX - r.left, e.clientY - r.top)
  drag.x1 = mx; drag.y1 = my
  draw()
}
function shieldUp(e) {
  if (!drag.on) return
  drag.on = false
  try { e?.currentTarget?.releasePointerCapture?.(e.pointerId) } catch { /* fine */ }
  if (Math.abs(drag.x1 - drag.x0) > 1 && Math.abs(drag.y1 - drag.y0) > 1)
    emit('shield', { x1: drag.x0, y1: drag.y0, x2: drag.x1, y2: drag.y1 })
  draw()
}

onMounted(() => {
  for (const n of board.value.copperNames) layerVis[n] = true
  fit()
  draw()
  ro = new ResizeObserver(() => { draw() })
  ro.observe(canvas.value)
  themeOff = onThemeChange(draw)
})
onBeforeUnmount(() => { ro?.disconnect(); themeOff?.() })

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
watch([layerVis, overlaysOn, partsOn, () => props.returnPath, () => props.nearField,
       () => props.selectedPart, () => props.partIndex, () => props.sweeping],
      () => draw())
watch(() => props.findings, () => draw())
// entering draw mode must drop the hover handler's inline cursor, or the
// stale 'grab'/'pointer' overrides the crosshair class
watch(() => props.drawingShield, on => {
  if (canvas.value) canvas.value.style.cursor = on ? '' : canvas.value.style.cursor
  if (on) hover.value = null
})
</script>

<template>
  <div ref="wrap" class="boardwrap">
    <canvas ref="canvas" data-testid="board-canvas"
            :class="{ drawing: drawingShield }"
            @pointerdown="e => drawingShield ? shieldDown(e) : onPointerDown(e)"
            @pointermove="e => drawingShield ? shieldMove(e) : onPointerMove(e)"
            @pointerup="e => drawingShield ? shieldUp(e) : onPointerUp(e)"
            @wheel="onWheel"
            @pointerleave="hover = null" />
    <div class="chips">
      <button v-for="(name, cu) in board.copperNames" :key="name" class="lchip"
              :class="{ off: !layerVis[name] }"
              :style="{ '--c': layerVar(cu) }"
              @click="layerVis[name] = !layerVis[name]">{{ name }}</button>
      <button class="lchip risk" :class="{ off: !overlaysOn }" data-testid="overlay-toggle"
              @click="overlaysOn = !overlaysOn">risk overlay</button>
      <button class="lchip" :class="{ off: !partsOn }" data-testid="parts-toggle"
              :style="{ '--c': 'var(--silk)' }"
              title="every component as a body over its pads — click one for its board facts, its catalogue record and datasheet, and cross-references"
              @click="partsOn = !partsOn">parts</button>
      <button class="lchip" :class="{ off: !partIndex, busy: sweeping }"
              data-testid="catalogue-toggle" :style="{ '--c': 'var(--heat-low)' }"
              :title="partIndex
                ? 'each part coloured by what the catalogue can resolve — click again to drop back to the plain parts layer'
                : 'ask the catalogue about every part on this board: green in the catalogue by part number, amber candidates to choose from, red nothing that fits. Downloads the catalogue families this board needs.'"
              @click="emit('sweep')">{{ sweeping ? 'asking…' : 'in catalogue' }}</button>
      <button class="lchip rad" :class="{ off: !returnPath }" data-testid="rp-toggle"
              :style="{ '--c': 'var(--heat-med)' }"
              title="effective loop height of every trace — where the return current really flows. Geometry only, no assumed currents"
              @click="emit('toggleReturnPath')">return path</button>
      <button class="lchip rad" :class="{ off: !nearField, dis: !hasSwitchNode }"
              data-testid="nf-toggle" :style="{ '--c': 'var(--heat-low)' }"
              :disabled="!hasSwitchNode"
              :title="hasSwitchNode
                ? 'quasi-static field at component scale — what couples ON the board'
                : swCandidateCount
                  ? `no switch node identified \u2014 ${swCandidateCount} candidate(s) in the strip below the board; promoting one enables this`
                  : 'no switching node on this board, so there is no near-field aggressor to model'"
              @click="emit('nearField')">near field</button>
      <button class="lchip rad off" data-testid="pdn-toggle"
              :style="{ '--c': 'var(--cool)' }"
              title="power-distribution impedance: every decoupling cap as a measured R-L-C branch"
              @click="emit('pdn')">pdn</button>
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
  background: rgba(var(--scrim), 0.82);
}
/* element selector: the canvas carries only the conditional 'drawing' class,
   so '.canvas.drawing' never matched and a stale inline 'grab' cursor (the
   pan hand) survived into drawing mode */
canvas.drawing { cursor: crosshair; }
.lchip.dis { opacity: 0.35; cursor: not-allowed; }
.lchip.off { opacity: 0.35; border-style: dashed; }
.lchip.busy { opacity: 1; border-style: dotted; }
.lchip.risk { --c: var(--heat-high); }

.tooltip {
  position: absolute; pointer-events: none; z-index: 10;
  background: rgba(var(--panel-float), 0.96);
  border: 1px solid var(--resin-edge); border-radius: 5px;
  padding: 7px 10px; max-width: 340px;
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
  box-shadow: var(--shadow);
}
.tooltip .head { color: var(--silk); margin-bottom: 2px; }
</style>
