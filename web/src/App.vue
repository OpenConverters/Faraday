<script setup>
import { ref, computed, watch, onMounted, provide } from 'vue'
import BoardView from './components/BoardView.vue'
import FindingsList from './components/FindingsList.vue'
import BenchPanel from './components/BenchPanel.vue'
import EmissionsPanel from './components/EmissionsPanel.vue'
import NearFieldPanel from './components/NearFieldPanel.vue'
import PdnPanel from './components/PdnPanel.vue'
import ImpedancePanel from './components/ImpedancePanel.vue'
import GlossaryPanel from './components/GlossaryPanel.vue'
import StackupPanel from './components/StackupPanel.vue'
import PartPanel from './components/PartPanel.vue'
import { unzip } from './zip.js'
import { sweepBoard, isPart, measuredParts, valueRows } from './parts.js'

// ── GUIDED vs ADVANCED ────────────────────────────────────────────────────
// The same review, two vocabularies. Advanced is everything this tool knows:
// dB, frequencies, confidence tiers, the sliders that drive the physics.
// Guided says the same findings in the language of the person who drew the
// board — one line for what was found, one for what to do — and puts every
// number one click away rather than in the way. It is NOT a reduced review:
// the identical engine runs, the identical findings rank, nothing is
// suppressed. A first visit lands in guided, because a screen full of
// unexplained decibels reads as "generic" to anyone who did not write it; the
// choice is remembered from then on.
const view = ref(localStorage.getItem('faraday.view') || 'guided')
const basic = computed(() => view.value === 'guided')
watch(view, v => localStorage.setItem('faraday.view', v))
provide('basic', basic)
// the ref itself, so a modal panel can offer the same switch: the header is
// behind the scrim while a panel is open, and "show me the numbers" has to be
// reachable from the place that made you want them
provide('view', view)

const engine = ref(null)
const boardText = ref('')
// A Gerber board is a SET of files ({name, text}); non-empty means the set
// path (analyzeSet) instead of the single-file one.
const boardFiles = ref([])
// when the engine names the copper count ("choose default-6layer"), the
// stackup card offers exactly that button
const stackupSuggest = ref('')
const suggestCopper = computed(
  () => Number((stackupSuggest.value.match(/\d+/) || [0])[0]))
const fileName = ref('')
const report = ref(null)
const error = ref('')
const needStackup = ref(false)
// true while the board is being screened on a dielectric NOBODY chose. The
// layer count is read off the board, so the board opens immediately; this flag
// is what keeps the assumption loud until the user replaces it.
const stackupAssumed = ref(false)
const stackupChoice = ref('')   // '' = from board file
const selectedId = ref('')
const dragOver = ref(false)
// The part inspector: clicking a component's body on the board opens it —
// the board's facts about the part, its catalogue record and datasheet, and
// ranked cross-references, all read in the browser.
const partRef = ref('')
// ── the catalogue overlay ────────────────────────────────────────────────
// The parts layer draws every component; this answers which of them the
// catalogue can resolve, and it is a question only the catalogue can answer —
// so it costs the family shards this board needs and is never run unasked.
const partIndex = ref(null)      // { ref: {state, …} } once swept
// Librarian answers this session already has, by part number. The inspector is
// destroyed on close, and a sourced record that dies with it costs a real
// distributor call to see again — so it is kept here, beside the board it
// belongs to, and handed back when the same part is opened.
const sourcedParts = ref({})
// The catalogue summary is read once and then in the way. Hidden per sweep, not
// per board: asking again is a new answer and deserves to be seen.
const catBarHidden = ref(false)
// The tally is stated, not remembered: sourcing a part changes it, and a line
// that still said "3 unmatched" after two of them were sourced would be wrong.
function retally() {
  if (!partIndex.value) return
  const vals = Object.values(partIndex.value)
  const n = s => vals.filter(v => v.state === s).length
  const parts = [`${n('exact')} identified`,
                 `${n('candidates')} with candidates`,
                 `${n('none')} unmatched`,
                 `${n('unlookupable')} unnamed by the board`]
  if (n('sourced')) parts.splice(1, 0, `${n('sourced')} sourced from the web`)
  sweepNote.value = `${vals.length} parts: ` + parts.join(', ') + '.'
}
// The catalogue's measured figures, into the physics. Identifying a part was
// only ever a label until this: Faraday assumed 0.015 ohm of ESR for every
// capacitor on every board, and that constant reaches the conducted-emissions
// maths through the input branch. An exactly-matched part publishes the real
// one, so the board is re-screened with it.
//
// Exact matches only, and re-screened only when something actually changed —
// a sweep that finds nothing measurable must not churn the report.
const measuredNote = ref('')
async function adoptMeasured() {
  if (!engine.value || !partIndex.value) return
  const parts = measuredParts(partIndex.value)
  if (!parts.length) { measuredNote.value = ''; return }
  try {
    const out = JSON.parse(engine.value.applyPartData(JSON.stringify(parts)))
    if (out.error) { measuredNote.value = out.error; return }

    // …and the VALUE itself, for every model that reads one. The PDN can take a
    // capacitance straight from the catalogue, but the Y-capacitor rule and the
    // conducted input branch parse the component's value string, and an Altium
    // board has none — so a board could be fully identified and still be told
    // its models were "quiet until the values arrive". applyValues never
    // overwrites a value the board itself carries, so this fills silence only.
    const csv = valueRows(partIndex.value)
    let filled = 0
    if (csv) {
      const v = JSON.parse(engine.value.applyValues(csv))
      if (!v.error) filled = v.applied ?? 0
    }
    measuredNote.value =
      `${out.parts} part(s) now carry their datasheet figures ` +
      `(${out.fields} measured value(s))` +
      (filled ? `, and ${filled} took their value from the catalogue` : '') +
      ` instead of Faraday's assumptions — the PDN, the input branch and the ` +
      `emissions estimate are re-run with them.`
    await reanalyze()
  } catch (e) {
    measuredNote.value = 'the catalogue values could not be applied: ' + String(e.message || e)
  }
}

function rememberSourced({ mpn, refdes, answer }) {
  if (!mpn || !answer) return
  sourcedParts.value = { ...sourcedParts.value, [mpn]: answer }
  // the catalogue overlay asked "is this part in Kelvin?" — it is not, but it
  // is no longer unknown either, and the overlay must stop saying it is
  if (answer.found && partIndex.value?.[refdes])
    partIndex.value = { ...partIndex.value,
                        [refdes]: { ...partIndex.value[refdes], state: 'sourced',
                                    sourcedMpn: mpn } }
  retally()
}
const sweeping = ref(false)
const sweepNote = ref('')
let sweepAbort = null

const boardParts = computed(() => {
  const b = report.value?.board
  if (!b) return []
  const byRef = new Map()
  for (const p of b.pads) {
    if (!byRef.has(p.component)) byRef.set(p.component, [])
    byRef.get(p.component).push(p)
  }
  return (b.components ?? []).filter(c => isPart(c, byRef.get(c.ref)))
})

// Ask the catalogue as soon as a board is on screen, rather than waiting to be
// asked. Identifying the parts is what puts real ESRs and a real Coss into the
// physics, so a board that is never swept is screened on assumptions — and
// nobody clicks a button whose value they cannot see yet.
//
// It runs ONCE per board and never fights the user: a sweep already running or
// already answered is left alone, and a manual dismissal of the overlay is not
// undone. The shards are a one-time download the browser keeps, so the second
// board costs nothing.
let autoSweptFor = ''
async function autoSweep() {
  if (!report.value || !engine.value) return
  const id = fileName.value + ':' + (report.value.board?.components?.length ?? 0)
  if (autoSweptFor === id) return
  autoSweptFor = id
  if (sweeping.value || partIndex.value) return
  if (!boardParts.value.length) return
  try { await toggleSweep() } catch { /* reported in sweepNote already */ }
}
// On the report, because that is when there are parts to ask about. NOT the
// place to un-hide the bar: adopting the catalogue's values re-screens the
// board, and that must not undo a dismissal the user just made.
watch(report, () => { autoSweep() })

async function toggleSweep() {
  if (sweeping.value) {                      // a second click cancels
    sweepAbort?.abort()
    sweeping.value = false
    sweepNote.value = 'stopped — partial answers are kept'
    return
  }
  if (partIndex.value) {                     // already answered: drop the layer
    partIndex.value = null
    sweepNote.value = ''
    return
  }
  const parts = boardParts.value
  if (!parts.length) { sweepNote.value = 'this board carries no components'; return }
  sweeping.value = true
  sweepNote.value = `asking the catalogue about ${parts.length} parts…`
  sweepAbort = new AbortController()
  const partial = {}
  try {
    const res = await sweepBoard(parts, {
      signal: sweepAbort.signal,
      onProgress: p => {
        if (p.phase === 'shard') sweepNote.value =
          `opening the ${p.family} catalogue — a one-time download the browser keeps (${p.done}/${p.total} settled)`
        else if (p.phase === 'family') sweepNote.value = `${p.done} of ${p.total} settled…`
        else if (p.phase === 'shardError') sweepNote.value =
          `the ${p.family} catalogue would not load: ${p.message}`
      },
    })
    partIndex.value = Object.fromEntries(res)
    await adoptMeasured()
    retally()
  } catch (e) {
    sweepNote.value = 'the catalogue could not be asked: ' + String(e.message || e)
    if (Object.keys(partial).length) partIndex.value = partial
  } finally {
    sweeping.value = false
    sweepAbort = null
  }
}
function gotoFinding(id) {
  partRef.value = ''
  selectedId.value = id
}
// A part the export left nameless (Altium's ODB++ writes part numbers, not
// values) can take its value from the catalogue record the inspector found.
// Same path as the CSV round trip: applyValues never overwrites a value the
// board already carries, and the board is re-screened with it.
async function adoptValue({ refdes, value }) {
  if (!engine.value) return
  const out = JSON.parse(engine.value.applyValues(`refdes,value\n${refdes},${value}\n`))
  if (out.error) { valuesNote.value = out.error; return }
  valuesNote.value = out.applied
    ? `${refdes} takes ${value} from its catalogue record`
    : `${refdes} already carries a value on the board — the layout outranks the catalogue`
  partRef.value = ''
  await reanalyze()
}

// The engine is a 650 kB WASM download and compile. On a fast local connection
// it is ready before anyone can pick a file; on a real one it is not, and a
// board dropped in that window used to hit `if (!engine) return` and vanish —
// no analysis, no error, no retry, forever. Holding the promise and awaiting it
// means a board is never silently dropped, whichever wins the race.
const enginePromise = window.createFaraday()
const engineLoading = ref(true)
// #load=<url>: fetch a board by URL. This is how the KiCad plugin hands the
// open board across (it serves the file from localhost with CORS), and how
// the demo button works — the fetch happens in the browser, so the privacy
// property is unchanged: nothing is uploaded anywhere.
// #op=<base64 json>: an operating point handed over by whatever DESIGNED the
// converter — Kirchhoff, Heaviside, or anything that can write a fragment.
// The reverse direction of the Hertz bridge, and private for the same reason:
// a URL fragment never reaches a server. Everything in it is a CIRCUIT
// property no layout can carry, which is exactly why it is worth accepting.
const operatingPoint = ref(null)
function loadOperatingPoint() {
  const m = location.hash.match(/[#&]op=([^&]+)/)
  if (!m) return
  try {
    const p = JSON.parse(decodeURIComponent(escape(atob(m[1]))))
    if (!p || typeof p !== 'object') return
    const num = (v, lo, hi) =>
      typeof v === 'number' && isFinite(v) && v >= lo && v <= hi ? v : undefined
    // Range-checked on the way in: a handoff is data from another program, and
    // a duty of 4 or a negative edge would throw inside the engine on the
    // first slider move rather than at the door.
    operatingPoint.value = {
      source: typeof p.source === 'string' ? p.source.slice(0, 40) : 'a design tool',
      label: typeof p.label === 'string' ? p.label.slice(0, 80) : '',
      currentA: num(p.currentA, 0.001, 1000),
      fSwKhz: num(p.fSwKhz, 1, 100000) ??
              (num(p.fSwHz, 1e3, 1e11) !== undefined ? p.fSwHz / 1e3 : undefined),
      duty: num(p.duty, 0.01, 0.99),
      riseNs: num(p.riseNs, 0.05, 5000),
      vBusV: num(p.vBusV, 0.1, 2000),
    }
  } catch { operatingPoint.value = null }
}

async function loadFromHash() {
  const m = location.hash.match(/^#load=(.+)$/)
  if (!m) return
  const url = decodeURIComponent(m[1])
  try {
    const res = await fetch(url)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    const txt = await res.text()
    fileName.value = url.split('/').pop() || 'board'
    boardText.value = txt
    boardFiles.value = []
    stackupChoice.value = ''
    customStackup.value = null
    await analyze()
  } catch (e) {
    error.value = `could not load ${url}: ${e.message || e}`
  }
}

onMounted(async () => {
  try {
    engine.value = await enginePromise
  } catch (e) {
    error.value = 'the analysis engine failed to load: ' + String(e)
  } finally {
    engineLoading.value = false
  }
  loadOperatingPoint()
  await loadFromHash()
  window.addEventListener('hashchange', () => { loadOperatingPoint(); loadFromHash() })
})

async function analyze() {
  if (!boardText.value && !boardFiles.value.length) return
  if (!engine.value) {
    // waiting on the engine is a state, not a failure — say so and continue
    engineLoading.value = true
    try { engine.value = await enginePromise } catch (e) {
      error.value = 'the analysis engine failed to load: ' + String(e)
      engineLoading.value = false
      return
    }
    engineLoading.value = false
  }
  error.value = ''
  needStackup.value = false
  const spec = stackupSpec()
  const out = boardFiles.value.length
    ? JSON.parse(engine.value.analyzeSet(JSON.stringify(
        { files: boardFiles.value, stackup: spec })))
    : JSON.parse(engine.value.analyze(boardText.value, spec))
  if (out.error) {
    report.value = null
    if (out.needStackup) {
      // The LAYER COUNT is not a guess — the engine read it off the board. So
      // rather than block on a card, adopt the matching default and open the
      // board; what stays unknown is the DIELECTRIC, and that is carried as an
      // explicit "assumed:" provenance everywhere it matters (banner, meta
      // strip, exported report) instead of being blocked on up front.
      stackupSuggest.value = `default-${out.copperCount}layer`
      hasSavedStackup.value = !!savedStackup()
      // Only ever retry when we had supplied NOTHING — a spec that was already
      // set and still came back "no stackup" is a real refusal (a stackup with
      // the wrong copper count), not something to answer again and loop on.
      if (spec) { error.value = out.error; return }
      const saved = savedStackup()
      if (saved) {                       // a stackup the user set for THIS board wins
        customStackup.value = saved
        stackupAssumed.value = false
      } else {
        stackupChoice.value = 'assumed:' + stackupSuggest.value
        stackupAssumed.value = true
      }
      return analyze()                   // stackupSpec() is now set
    }
    error.value = out.error
    return
  }
  report.value = out
  selectedId.value = ''
  promotedNets.value = []   // the engine reset its session on (re)import
}

async function onFiles(list) {
  const picked = Array.from(list || []).filter(Boolean)
  if (!picked.length) return
  error.value = ''
  try {
    let files = []
    for (const f of picked) {
      if (/\.zip$/i.test(f.name)) {
        files.push(...await unzip(await f.arrayBuffer()))
      } else {
        files.push({ name: f.name, text: await f.text() })
      }
    }
    if (files.length === 1) {
      // the single-file formats keep their fast path
      fileName.value = files[0].name
      boardText.value = files[0].text
      boardFiles.value = []
    } else {
      fileName.value = picked.length === 1
        ? picked[0].name : `${files.length} files (Gerber set)`
      boardText.value = ''
      boardFiles.value = files
    }
  } catch (e) {
    error.value = String(e.message || e)
    return
  }
  stackupChoice.value = ''
  stackupAssumed.value = false
  customStackup.value = null
  clearBaseline()
  fixResult.value = ''
  await analyze()
}

function onDrop(e) {
  dragOver.value = false
  onFiles(e.dataTransfer?.files)
}

function chooseStackup(name) {
  if (name === '__custom__') { stackupOpen.value = true; return }
  customStackup.value = null
  stackupChoice.value = name
  stackupAssumed.value = false   // chosen, not assumed, from here on
  analyze()
}

const meta = computed(() => report.value?.meta ?? null)
// The dv/dt copper: every square millimetre that swings with the switching
// edge, summed off the layout by the screener. It is the plate that drives
// common-mode current into the chassis, and handing it to the emissions panel
// is what lets the conducted estimate stop asking anyone to invent a C_stray.
const dvdtAreaMm2 = computed(() => meta.value?.dvdtCopper?.totalMm2 ?? 0)

// ── values a CAD export did not carry ────────────────────────────────────
// Altium's ODB++ writes the manufacturer part number in the component record
// and no value at all, so a board full of real capacitors arrives nameless and
// every model that needs a capacitance — the PDN, the input branch, the Y-cap
// rule — correctly refuses to invent one. This hands them back.
const valuesNote = ref('')
// The values card in three states: the full ask, one line, gone. Dismissing
// first collapses to the line — the models that need a capacitance stay quiet
// either way, and that is worth one sentence — and dismissing again removes it
// for this board. A board whose values are genuinely never coming should not
// have to scroll past the note forever, and the refusal is still stated where
// it bites: the PDN, the input branch and the Y-cap rule each say why they are
// empty when you open them.
const valuesCard = ref('full')   // 'full' | 'brief' | 'gone'
const partsMissing = computed(() => {
  if (!engine.value || !report.value) return 0
  try {
    const p = JSON.parse(engine.value.partsWithoutValues())
    return Array.isArray(p) ? p.length : 0
  } catch { return 0 }
})
async function loadValues(file) {
  if (!file || !engine.value) return
  try {
    const text = await file.text()
    const out = JSON.parse(engine.value.applyValues(text))
    if (out.error) { valuesNote.value = out.error; return }
    valuesNote.value = `filled ${out.applied} component value(s) from ` +
      `${file.name}` + (out.ignored ? ` — ${out.ignored} skipped, the board ` +
      `already carried a value` : '')
    // reanalyze, NOT analyze: analyze() re-imports the board text and would
    // build a fresh IR, throwing away the values just applied to this one.
    await reanalyze()
  } catch (e) {
    valuesNote.value = 'could not read that table: ' + String(e.message || e)
  }
}

// The parts list the table answers — offered as a download, so the round trip
// (ask the board, resolve the part numbers, hand them back) works from here.
function downloadParts() {
  if (!engine.value) return
  const p = JSON.parse(engine.value.partsWithoutValues())
  const csv = 'refdes,part\n' + p.map(x => `${x.refdes},${x.part}`).join('\n') + '\n'
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }))
  a.download = (fileName.value || 'board') + '-parts.csv'
  a.click()
  URL.revokeObjectURL(a.href)
}
// ── back to the start screen ─────────────────────────────────────────────
// The header is the way out of a review. Everything a session accumulated goes
// with the board it was about: the report, the findings and what was hidden or
// dismissed in them, the overlays, the panels, the promoted switch nodes and
// the shields somebody drew. Leaving any of it behind would decorate the NEXT
// board with the last one's answers.
//
// What survives is what is not about this board: the guided/advanced choice
// (a preference), and the per-file stackup in localStorage (it belongs to the
// board, and re-dropping the same file should not ask again).
//
// Anything with unsaved work in it asks first. A dropped board is cheap to
// drop again, but a promoted switch node, a drawn shield or a custom stackup
// is a decision someone made and cannot get back with a re-drop.
const unsavedWork = computed(() => {
  if (!report.value) return []
  const bits = []
  if (promotedNets.value.length) bits.push(`${promotedNets.value.length} promoted switch node(s)`)
  if (shields.value.length) bits.push(`${shields.value.length} shield can(s)`)
  if (customStackup.value) bits.push('a stackup you entered')
  if (hiddenIds.value.size) bits.push(`${hiddenIds.value.size} dismissed finding(s)`)
  if (baselineReport.value) bits.push('a revision comparison')
  return bits
})

function goHome() {
  const lose = unsavedWork.value
  if (lose.length &&
      !window.confirm(`Close ${fileName.value || 'this board'}?\n\n` +
                      `This review carries ${lose.join(', ')}, which the board file ` +
                      `does not, so re-opening it will not bring them back.`))
    return
  // Navigate to the site's root rather than clearing thirty refs by hand. Two
  // things this gets that the hand reset did not: the URL's #load= hash goes
  // with it — leaving it behind meant a reload brought the closed board
  // straight back — and nothing new can be forgotten here when a later feature
  // adds state, which is how a "clear everything" list rots.
  //
  // The root of THIS origin, not a hardcoded address: in production that is
  // https://faraday.openconverters.com/, and on a dev server it is the dev
  // server, which is what anyone running one means by home.
  window.location.assign(new URL('/', window.location.href).href)
}

const findings = computed(() => report.value?.findings ?? [])

// Rule filter: dense boards render 200 overlapping overlays, so let the reader
// mute whole rules. Muting hides them from BOTH the list and the board.
const hiddenRules = ref(new Set())
const ruleCounts = computed(() => {
  const m = new Map()
  for (const f of findings.value) m.set(f.rule, (m.get(f.rule) ?? 0) + 1)
  return [...m.entries()].sort((a, b) => b[1] - a[1])
})
const visibleFindings = computed(() =>
  findings.value.filter(f => !hiddenRules.value.has(f.rule) &&
                             !hiddenIds.value.has(f.id) &&
                             (!onlyChanges.value || diffMap.value[f.id])))
// The bench: the deep tier, opened on one finding. Only findings that carry a
// cross-section can be solved — a run with no reference plane has no
// cross-section, and the list says so rather than offering a dead button.
const benchId = ref('')
const benchFinding = computed(() =>
  findings.value.find(f => f.id === benchId.value && f.solve) ?? null)
const netName = id =>
  report.value?.board.nets.find(n => n.id === id)?.name || (id >= 0 ? `net ${id}` : '')
const layerName = cu => report.value?.board.copperNames?.[cu] ?? `layer ${cu}`

// Radiated emissions: offered on findings that carry an enclosed loop area,
// which today means commutation loops. The loop area is the input nobody else
// can supply, and it comes straight off the copper.
// The return-path layer: effective loop height per segment, geometry only.
// It replaced the "radiation attribution" once a measurement showed that
// ranking with the assumed current was 97% a restatement of the switch-node
// rule — the current is gone, and what remains is what the layout proves.
const returnPath = ref(null)
const rpError = ref('')
function runReturnPath() {
  try {
    const out = JSON.parse(engine.value.returnPath(JSON.stringify({})))
    if (out.error) { rpError.value = out.error; return }
    rpError.value = ''
    returnPath.value = out
  } catch (e) { rpError.value = String(e) }
}
function toggleReturnPath() {
  if (returnPath.value) { returnPath.value = null; return }
  runReturnPath()
}
watch(report, () => { returnPath.value = null; rpError.value = '' })
// A different board is a different question: it must ask again, even if the
// last one was told to be quiet.
watch(fileName, () => { valuesCard.value = 'full'; catBarHidden.value = false })

// The component near-field map: a different regime from the far-field
// attribution, so it is a separate panel with its own units and its own caveat.
const nearField = ref(null)
const nfError = ref('')
const nfDetail = ref(false)
// The excitation the overlay is computed at. Held here rather than in the
// panel because the board overlay needs it too.
const nfParams = ref({ ringCurrentA: 2, ringMhz: 130, probeHeightMm: 3, victimAreaMm2: 4 })
// Shield cans the user has drawn on the near-field layer. A can attenuates
// coupling only when it separates aggressor from victim, and the SE comes from
// its own material, wall and contact pitch at the ring frequency.
const shields = ref([])
const shieldSpec = ref({ material: 'tinsteel', wallMm: 0.2, seamPitchMm: 5, muR: 0 })
const drawingShield = ref(false)
function addShield(rect) {
  shields.value = [...shields.value, rect]
  drawingShield.value = false
  runNearField()
}
function clearShields() { shields.value = []; runNearField() }
watch(shieldSpec, () => { if (nearField.value) runNearField() }, { deep: true })
function runNearField() {
  try {
    const out = JSON.parse(engine.value.nearField(JSON.stringify({
      ...nfParams.value,
      shields: shields.value.map(s => ({ ...s, ...shieldSpec.value })),
    })))
    if (out.error) { nfError.value = out.error; nearField.value = null; return }
    nfError.value = ''
    nearField.value = out
  } catch (e) { nfError.value = String(e) }
}
function toggleNearField() {
  if (nearField.value) {
    nearField.value = null; nfError.value = ''; drawingShield.value = false
    return
  }
  runNearField()
}
function setNfParams(p) { nfParams.value = { ...nfParams.value, ...p }; runNearField() }
watch(report, () => { nearField.value = null; nfError.value = ''; nfDetail.value = false })

// The near-field map is built around switching aggressors, so on a board with
// none it has nothing to say. Better to disable the chip with the reason than
// to let it error on click.
const hasSwitchNode = computed(() => (meta.value?.switchNodes?.length ?? 0) > 0)

// Candidate switch nodes (ABT #408/#410): nets that LOOK like a monolithic
// converter (wound part + active silicon, no shunt cap, two filtered rails)
// but are externally isomorphic to a linear regulator's LC harness. The
// engine will not guess; the user can promote one — screened with
// switchNodeSource "user" so the exported report carries the provenance.
const swCandidates = computed(() => meta.value?.switchNodeCandidates ?? [])
const promotedNets = ref([])
async function promoteNet(net) {
  promotedNets.value = [...promotedNets.value, net]
  await reanalyze()
}
async function demoteNet(net) {
  promotedNets.value = promotedNets.value.filter(n => n !== net)
  await reanalyze()
}
async function reanalyze() {
  // re-screen the engine's cached board with the session's promotions —
  // no re-import, so this is instant even on multi-MB boards
  const out = JSON.parse(engine.value.reanalyze(
    JSON.stringify({ userSwitchNets: promotedNets.value })))
  if (out.error) { error.value = out.error; return }
  report.value = out
  selectedId.value = ''
}
const ruleCountMap = computed(() => Object.fromEntries(ruleCounts.value))

const pdnOpen = ref(false)
const glossaryOpen = ref(false)
const stackupOpen = ref(false)
// A user-entered stackup (layers array). When set it wins over stackupChoice
// and is persisted per board file name, because a stackup belongs to a board.
const customStackup = ref(null)
function stackupKey() { return 'faraday-stackup:' + fileName.value }
function savedStackup() {
  try { return JSON.parse(localStorage.getItem(stackupKey()) || 'null') }
  catch { return null }
}
function applyStackup(layers) {
  customStackup.value = layers
  stackupOpen.value = false
  stackupAssumed.value = false
  try { localStorage.setItem(stackupKey(), JSON.stringify(layers)) } catch {}
  analyze()
}
function useSavedStackup() {
  const l = savedStackup()
  if (l) { customStackup.value = l; stackupAssumed.value = false; analyze() }
}
const stackupSpec = () => customStackup.value
  ? JSON.stringify({ layers: customStackup.value })
  : stackupChoice.value
const hasSavedStackup = ref(false)

// ---- revision diff ("did this change make EMC worse?") ----
// baseline = an exported report JSON, or another revision's board file
// (analysed on the spot with the SAME stackup spec)
const baselineReport = ref(null)
const baselineName = ref('')
const diff = ref(null)
const onlyChanges = ref(false)
const baselineInput = ref(null)
const fileInput = ref(null)
// stitching-fix result line shown under the return-path bar
const fixResult = ref('')
function fixStitching() {
  fixResult.value = ''
  try {
    const out = JSON.parse(engine.value.fixStitching(boardText.value, stackupSpec()))
    if (out.error) { fixResult.value = out.error; return }
    if (out.text) {
      const base = fileName.value.replace(/\.kicad_pcb$/i, '')
      const blob = new Blob([out.text], { type: 'text/plain' })
      const a = document.createElement('a')
      a.href = URL.createObjectURL(blob)
      a.download = `${base}-stitched.kicad_pcb`
      a.click()
      URL.revokeObjectURL(a.href)
      fixResult.value = `${out.vias.length} stitching via(s) added — review the`
        + ` downloaded file in KiCad; your original is untouched`
    } else {
      fixResult.value = out.notes[0] || 'nothing to stitch'
    }
  } catch (e) { fixResult.value = String(e.message || e) }
}
function computeDiff() {
  diff.value = baselineReport.value && report.value
    ? JSON.parse(engine.value.diffReports(
        JSON.stringify(baselineReport.value), JSON.stringify(report.value)))
    : null
  if (diff.value?.error) { error.value = diff.value.error; diff.value = null }
}
async function onBaselineFile(file) {
  if (!file || !report.value) return
  try {
    const text = await file.text()
    if (/^\s*\{/.test(text)) {
      const j = JSON.parse(text)
      if (!j.findings || !j.meta)
        throw new Error('not a Faraday report (missing findings/meta)')
      baselineReport.value = j
    } else {
      // another revision's board: analyse it, then re-analyse the current
      // board so the engine's held board (overlays, PDN) stays the CURRENT one
      const out = JSON.parse(engine.value.analyze(text, stackupSpec()))
      if (out.error) throw new Error(out.error)
      baselineReport.value = out
      await analyze()
    }
    baselineName.value = file.name
    computeDiff()
  } catch (e) {
    error.value = 'baseline: ' + String(e.message || e)
  }
}
function clearBaseline() {
  baselineReport.value = null; baselineName.value = ''
  diff.value = null; onlyChanges.value = false
}
watch(report, computeDiff)
const diffMap = computed(() => {
  const m = {}
  if (diff.value) {
    for (const e of diff.value.added) m[e.id] = 'new'
    for (const e of diff.value.worsened) m[e.id] = 'worse'
  }
  return m
})
// Individually dismissed findings. Per board, not persisted: a dismissal is a
// review decision for THIS review.
const hiddenIds = ref(new Set())
function hideFinding(id) {
  const s2 = new Set(hiddenIds.value)
  s2.add(id)
  hiddenIds.value = s2
  if (selectedId.value === id) selectedId.value = ''
}
function unhideAll() { hiddenIds.value = new Set() }
function loadDemo() { window.location.hash = '#load=/demo.kicad_pcb' }
watch(report, () => { hiddenIds.value = new Set() })
const calcOpen = ref(false)
watch(report, () => { pdnOpen.value = false })

// Report export: a single self-contained HTML file built from the report the
// engine already produced. A design review you cannot hand to a colleague is
// half a review.
function exportReport() {
  const rep = report.value
  if (!rep) return
  const esc = t => String(t ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;')
  const rows = rep.findings.map(f => `<tr class="${esc(f.severityLabel)}">
    <td>${esc(f.id)}</td><td>${esc(f.rule)}</td><td>${esc(f.severityLabel)}</td>
    <td>${esc(f.title)}</td><td>${esc(f.detail)}<br><b>Fix:</b> ${esc(f.remediation)}</td></tr>`).join('')
  const planes = (rep.meta.planes ?? []).map(p =>
    `${esc(p.layer)}: ${p.isPlane ? 'plane' : 'signal'}`).join(' · ')
  const html = `<!doctype html><html><head><meta charset="utf-8">
<title>Faraday EMC review — ${esc(fileName.value)}</title>
<style>
 body{font:14px/1.5 system-ui;margin:2rem auto;max-width:70rem;color:#1c2422;padding:0 1rem}
 h1{font-size:1.4rem} .meta{color:#5a6d67;font-size:.85rem;margin-bottom:1.5rem}
 table{border-collapse:collapse;width:100%;font-size:.85rem}
 th,td{border:1px solid #d5ddd9;padding:.4rem .6rem;text-align:left;vertical-align:top}
 tr.high td:first-child{border-left:4px solid #d33}
 tr.medium td:first-child{border-left:4px solid #d90}
 .caveat{background:#f4f1ea;border:1px solid #d5ddd9;padding:.8rem 1rem;margin:1.2rem 0;font-size:.85rem}
</style></head><body>
<h1>Faraday EMC design review — ${esc(fileName.value)}</h1>
<p class="meta">Generated ${new Date().toISOString().slice(0, 10)} ·
 format ${esc(rep.format)} · stackup ${esc(rep.meta.stackupSource)} · ${planes} ·
 ${rep.findings.length} findings</p>
<div class="caveat"><b>Screening estimates, not compliance predictions.</b>
 Faraday ranks EMC risk from layout geometry with stated physics and stated
 error bars. Findings marked screening-estimate carry roughly ±6 dB; a clean
 report is not a chamber pass. Analysis ran locally in the browser — the
 layout never left the machine.</div>
<table><thead><tr><th>id</th><th>rule</th><th>severity</th><th>finding</th>
<th>detail</th></tr></thead><tbody>${rows}</tbody></table>
</body></html>`
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([html], { type: 'text/html' }))
  a.download = (fileName.value || 'board').replace(/\.[^.]+$/, '') + '-faraday-review.html'
  a.click()
  URL.revokeObjectURL(a.href)
}

const emitId = ref('')
const emitFinding = computed(() =>
  findings.value.find(f => f.id === emitId.value && f.emit) ?? null)

function toggleRule(rule) {
  const s = new Set(hiddenRules.value)
  s.has(rule) ? s.delete(rule) : s.add(rule)
  hiddenRules.value = s
  if (selectedId.value && !visibleFindings.value.some(f => f.id === selectedId.value))
    selectedId.value = ''
}
</script>

<template>
  <div class="shell" @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
       @drop.prevent="onDrop">
    <header class="topbar">
      <!-- The wordmark is the way back to the start screen once a board is
           open; with nothing loaded there is nowhere to go, so it is not a
           button and does not pretend to be one. -->
      <button v-if="report || needStackup || error || fileName" class="brandbtn"
              data-testid="go-home"
              title="Close this board and go back to the start screen"
              @click="goHome">
        <span class="brand">FARADAY</span>
        <span class="tagline">EMC design review — runs in your browser, nothing is uploaded</span>
      </button>
      <template v-else>
        <h1 class="brand">FARADAY</h1>
        <span class="tagline">EMC design review — runs in your browser, nothing is uploaded</span>
      </template>
      <div class="spacer" />
      <!-- One switch, always in the same place: the review does not change,
           only how much of its vocabulary is on screen. -->
      <div class="viewtoggle" data-testid="view-toggle" role="group"
           aria-label="Level of detail">
        <button :class="{ on: basic }" data-testid="view-guided"
                :aria-pressed="basic" @click="view = 'guided'"
                title="Plain language: what was found and what to do about it. Every number is still one click away.">
          guided</button>
        <button :class="{ on: !basic }" data-testid="view-advanced"
                :aria-pressed="!basic" @click="view = 'advanced'"
                title="Everything: decibels, frequencies, confidence tiers, and the sliders that drive the physics.">
          advanced</button>
      </div>
      <label class="filebtn">
        <input ref="fileInput" data-testid="file-input" type="file" multiple
               accept=".kicad_pcb,.hyp,.HYP,.xml,.zip,.gbr,.gtl,.gbl,.g1,.g2,.g3,.g4,.gm1,.gko,.drl,.xln,.txt"
               @change="e => onFiles(e.target.files)" />
        {{ fileName || 'Open board file' }}
      </label>
      <button v-if="report" class="filebtn" data-testid="export-report"
              @click="exportReport">export report</button>
      <button v-if="report && !baselineReport" class="filebtn" data-testid="compare"
              @click="baselineInput.click()"
              title="compare against another revision — a board file or an exported report">
        compare rev…</button>
      <input ref="baselineInput" type="file" style="display:none"
             data-testid="baseline-input"
             accept=".json,.kicad_pcb,.hyp,.HYP,.xml"
             @change="e => { onBaselineFile(e.target.files[0]); e.target.value = '' }" />
      <!-- the dielectric was ASSUMED (layer count is read off the board, the
           dielectric is not in the file). One compact header button instead
           of a banner: the assumption stays visible without eating a row,
           the meta strip carries the full "assumed:" provenance, and the
           fix is still one click. -->
      <button v-if="stackupAssumed && report" class="filebtn warn"
              data-testid="stackup-assumed" @click="stackupOpen = true"
              title="Screened on an assumed dielectric: default FR4, 1.6 mm. The copper layer count is read from your board; the dielectric is not in the file, and Z₀, coupling and every dB depend on it. Click to enter the real stackup.">
        ⚠ assumed stackup — set real</button>
      <select v-if="report || needStackup" data-testid="stackup-select" class="stackup"
              :value="customStackup ? '__active__' : stackupChoice"
              @change="e => chooseStackup(e.target.value)"
              aria-label="Stackup">
        <option v-if="customStackup" value="__active__">
          Stackup: custom ({{ customStackup.filter(l => l.kind === 'copper').length }} layers)</option>
        <option value="">Stackup: from board file</option>
        <option value="default-2layer">Stackup: default 2-layer FR4</option>
        <option value="default-4layer">Stackup: default 4-layer FR4</option>
        <option value="__custom__">Stackup: enter the real one…</option>
      </select>
    </header>

    <div v-if="error" class="banner error" data-testid="error-banner">{{ error }}</div>

    <div v-if="engineLoading && fileName" class="banner wait" data-testid="engine-loading">
      Loading the analysis engine…</div>
    <div v-if="rpError" class="banner error" data-testid="rp-error">{{ rpError }}</div>

    <div v-if="nfError" class="banner error" data-testid="nf-error">{{ nfError }}</div>

    <!-- Some exports carry part numbers and no values (Altium's ODB++ does).
         Say so once, where it is actionable, rather than only inside whichever
         panel refuses first. -->
    <div v-if="report && partsMissing > 0 && valuesCard === 'full'"
         class="banner ask" data-testid="values-card">
      <p><b>{{ partsMissing }} component(s) carry a part number and no value</b> —
         this export wrote part numbers instead. Every model that needs a
         capacitance (PDN impedance, the conducted input branch, the Y-capacitor
         rule) refuses to guess one, so they are quiet until the values arrive.</p>
      <button class="chip" data-testid="parts-download" @click="downloadParts">
        download the part list (refdes, part number)</button>
      <label class="chip real">
        <input type="file" accept=".csv,.txt" style="display:none"
               data-testid="values-input"
               @change="e => { loadValues(e.target.files[0]); e.target.value = '' }" />
        load component values (refdes, value)…</label>
      <button class="x" data-testid="values-dismiss" aria-label="Dismiss"
              title="Hide this. The models that need a value stay quiet."
              @click="valuesCard = 'brief'">✕</button>
    </div>

    <!-- One line: the count and the way back, for a board that has been told
         once. Dismissed again it goes entirely — the models still refuse in
         their own panels, so nothing silently becomes a guess. -->
    <div v-if="report && partsMissing > 0 && valuesCard === 'brief'"
         class="banner wait brief" data-testid="values-dismissed">
      {{ partsMissing }} component(s) still have no value — the models that need one
      stay quiet.
      <label class="chip">
        <input type="file" accept=".csv,.txt" style="display:none"
               data-testid="values-input-dismissed"
               @change="e => { loadValues(e.target.files[0]); e.target.value = '' }" />
        load component values…</label>
      <button class="chip" data-testid="values-restore"
              @click="valuesCard = 'full'">show the full note</button>
      <button class="x" data-testid="values-dismiss-fully" aria-label="Dismiss for this board"
              title="Hide this for this board. The models that need a value still refuse."
              @click="valuesCard = 'gone'">✕</button>
    </div>

    <!-- Outside the card on purpose: filling the values RETIRES the card, and
         the confirmation of what just happened must outlive the thing that
         asked for it. -->
    <!-- The catalogue changed the physics: say so, because a report whose ESR
         came from a datasheet and one whose ESR came from a constant look
         identical otherwise. -->
    <div v-if="measuredNote" class="banner wait" data-testid="measured-note">
      {{ measuredNote }}
    </div>

    <div v-if="valuesNote" class="banner wait" data-testid="values-note">
      {{ valuesNote }}
      <label class="chip">
        <input type="file" accept=".csv,.txt" style="display:none"
               data-testid="values-input-again"
               @change="e => { loadValues(e.target.files[0]); e.target.value = '' }" />
        load another table…</label>
    </div>

    <div v-if="needStackup" class="banner ask" data-testid="stackup-card">
      <p><b>{{ suggestCopper }} copper layers</b> — read from the board. What the file does
         not carry is the dielectric: no thicknesses, no εr, and every Z₀ and coupling
         figure stands on them. Choose what the board is built on (the report will state
         your choice):</p>
      <button class="chip" data-testid="stackup-suggest"
              @click="chooseStackup(stackupSuggest)">
        Default {{ suggestCopper }}-layer FR4 (1.6 mm)</button>
      <button class="chip real" data-testid="stackup-custom"
              @click="stackupOpen = true">
        Enter the real stackup — every Z₀ and coupling figure stands on it</button>
      <button v-if="hasSavedStackup" class="chip" data-testid="stackup-saved"
              @click="useSavedStackup">
        Use the stackup you saved for this board</button>
    </div>

    <main class="work" v-if="report">
      <!-- The board cell is a fixed grid cell; the return-path / near-field
           summaries float over its LEFT edge instead of sitting above the
           board, so toggling an overlay never moves or rescales the copper —
           the point of toggling is comparing, and comparing needs a still
           board. -->
      <div class="boardcell">
        <BoardView :report="report" :findings="visibleFindings" :selected-id="selectedId"
                   :return-path="returnPath" :near-field="nearField"
                   :shields="shields" :drawing-shield="drawingShield"
                   :has-switch-node="hasSwitchNode"
                   :sw-candidate-count="swCandidates.length"
                   :selected-part="partRef"
                   :part-index="partIndex"
                   :sweeping="sweeping"
                   @sweep="toggleSweep"
                   @part="r => partRef = r"
                   @select="id => selectedId = id"
                   @toggle-return-path="toggleReturnPath"
                   @near-field="toggleNearField"
                   @shield="addShield"
                   @pdn="pdnOpen = true" />
      <!-- while drawing a shield the bars go click-through and faded — a
           floating card must never steal the drag that draws underneath it -->
      <!-- ghost while a shield is being drawn, exactly as the bars below. This
           bar used to appear only when someone asked for it, so it never
           overlapped a drag; now it is on every board, and without this it
           steals the pointer that draws the can. -->
      <div v-if="sweepNote && !catBarHidden" class="overlaybars"
           :class="{ ghost: drawingShield }">
        <div class="radbar cat" data-testid="catalogue-bar">
          <b>Catalogue</b>
          <span>{{ sweepNote }}</span>
          <span v-if="partIndex" class="key">
            <i class="k-exact" /><span>in the catalogue, by part number</span>
            <i class="k-cand" /><span>candidates to choose from</span>
            <i class="k-none" /><span>nothing in the catalogue fits</span>
            <i class="k-unk" /><span>the board never named it</span>
          </span>
          <!-- The tally and its legend are worth reading once and then in the
               way. Dismissing hides the BAR; the overlay it explains stays on,
               and the chip that toggles the overlay brings it back. -->
          <button class="barx" data-testid="catalogue-bar-dismiss"
                  aria-label="Hide the catalogue summary"
                  title="Hide this. The overlay stays; the colours mean the same."
                  @click="catBarHidden = true">✕</button>
        </div>
      </div>
      <div class="overlaybars" :class="{ ghost: drawingShield }"
           v-if="nearField || returnPath">
      <div v-if="nearField" class="radbar nf" data-testid="nf-bar">
        <b>Near field</b>
        <span>|H| at {{ nfParams.probeHeightMm.toFixed(1) }} mm ·
          {{ nfParams.ringCurrentA.toFixed(1) }} A ring at {{ nfParams.ringMhz.toFixed(0) }} MHz ·
          λ/2π = {{ nearField.lambdaOver2PiMm.toFixed(0) }} mm, the whole board is inside it</span>
        <span v-if="nearField.victims?.length" class="top">
          worst: <b>{{ nearField.victims[0].component }}</b>
          ({{ nearField.victims[0].class }})
          {{ nearField.victims[0].ratio >= 10
             ? (20 * Math.log10(nearField.victims[0].ratio)).toFixed(0) + ' dB over'
             : (nearField.victims[0].ratio * 100).toFixed(0) + '% of' }} threshold</span>
        <span v-if="nearField.shieldedVictims" class="top" data-testid="nf-shielded">
          can separates <b>{{ nearField.shieldedVictims }}</b> victim(s) from the
          aggressor — an upper bound, the can is five-sided</span>
        <button class="detail" data-testid="nf-shield-draw"
                @click="drawingShield = !drawingShield">
          {{ drawingShield ? 'click-drag on the board…' : 'draw a shield can' }}</button>
        <button v-if="shields.length" class="detail" data-testid="nf-shield-clear"
                @click="clearShields">clear {{ shields.length }}</button>
        <label v-if="shields.length" class="spec">pitch
          <input type="range" min="1" max="40" step="0.5" data-testid="nf-shield-pitch"
                 v-model.number="shieldSpec.seamPitchMm" />
          <b>{{ shieldSpec.seamPitchMm.toFixed(1) }} mm</b></label>
        <label v-if="shields.length" class="spec">µ′
          <select v-model.number="shieldSpec.muR" data-testid="nf-can-mur"
                  title="permeability grade — µ′ from the shield material's datasheet at the ring frequency; vendors sell several">
            <option :value="0">default</option>
            <option v-for="g in [40, 60, 120, 220, 300, 1000]" :key="g" :value="g">{{ g }}</option>
          </select></label>
        <button class="detail" data-testid="nf-detail" @click="nfDetail = true">
          victims &amp; shielding →</button>
        <span v-if="basic" class="cav" data-testid="nf-cav-plain"><b>Which parts sit in
          the switching field.</b> Warmer colour means a stronger magnetic field at that
          spot. It is not a "will it pass" number — it is where to move a sensitive part
          away from, or where a shield can would earn its place.</span>
        <span v-if="!basic" class="cav"><b>What couples on the board.</b> Quasi-static induction
          at component scale, in A/m — not a radiation map, and only switching
          loops are modelled as sources. No dBµV/m, no limit line: there is no
          reliable near-field to far-field transform. The ring current is your
          assumption. The return-path layer shows where returns detour; the
          emissions panel covers what leaves the board.</span>
      </div>

      <div v-if="returnPath" class="radbar" data-testid="rp-bar">
        <b>Return path</b>
        <span>effective loop height {{ returnPath.minEffHeightMm.toFixed(2) }}–{{
          returnPath.maxEffHeightMm.toFixed(2) }} mm over {{ returnPath.counted }} segments
          — geometry only, no assumed currents</span>
        <span v-if="returnPath.overVoidCount" class="warn" data-testid="rp-void">
          {{ returnPath.overVoidCount }} segment(s) over a plane void or split</span>
        <span v-if="returnPath.layerChangeCount" class="top">
          {{ returnPath.layerChangeCount }} layer change(s),
          <b :class="returnPath.unstitchedCount ? 'bad' : ''">{{ returnPath.unstitchedCount }}
          with no stitching via in reach</b></span>
        <span class="top" v-for="w in returnPath.worst.slice(0, 3)" :key="w.net">
          {{ w.net || '(unnamed)' }} <b>{{ w.areaMm2.toFixed(0) }} mm²</b><template
          v-if="w.unstitched"> · unstitched</template><template
          v-else-if="w.overVoid"> · over void</template></span>
        <button v-if="report.format === 'kicad' && returnPath.unstitchedCount"
                class="detail" data-testid="rp-fix"
                @click="fixStitching">
          generate stitching vias → new file</button>
        <span v-if="fixResult" class="top" data-testid="rp-fix-result">{{ fixResult }}</span>
        <span v-if="basic" class="cav" data-testid="rp-cav-plain"><b>How far each track's
          return current has to travel to get back.</b> Bigger numbers mean bigger loops,
          and bigger loops both radiate and pick up more. Everything here is measured off
          your copper — nothing is assumed about your currents.</span>
        <span v-if="!basic" class="cav"><b>How far away each trace's return current really is</b> —
          the dielectric height where the plane is solid beneath it, the detour where
          it is not, the hop at every layer change. Every number here is a geometric
          fact of the layout. For a defensible far-field figure with a limit line, use
          the emissions panel on a commutation-loop finding; for coupling into nearby
          parts, the near-field layer.</span>
      </div>
      </div>
      </div>
      <FindingsList :findings="visibleFindings" :report="report" :selected-id="selectedId"
                    :rules="ruleCounts" :hidden-rules="hiddenRules" :total="findings.length"
                    @select="id => selectedId = selectedId === id ? '' : id"
                    :hidden-count="hiddenIds.size"
                    :diff="diff" :diff-map="diffMap" :baseline-name="baselineName"
                    :only-changes="onlyChanges"
                    @toggle-changes="onlyChanges = !onlyChanges"
                    @clear-baseline="clearBaseline"
                    @toggle-rule="toggleRule"
                    @bench="id => benchId = id"
                    @emissions="id => emitId = id"
                    @hide="hideFinding"
                    @unhide-all="unhideAll"
                    @glossary="glossaryOpen = true" />
    </main>

    <div v-else-if="!needStackup" class="empty" :class="{ over: dragOver }">
      <div class="board-ghost" aria-hidden="true" />
      <p class="invite">Drop a board here</p>
      <p class="formats"><code>.kicad_pcb</code> · <code>.hyp</code> · <code>IPC-2581 .xml</code>
         · <code>Gerber X2 set</code> · <code>ODB++ job</code> (files, or one zip)</p>
      <button class="chip" data-testid="open-calc" @click="calcOpen = true">
        impedance calculator — no board needed</button>
      <button class="chip" data-testid="load-demo" @click="loadDemo"
              title="LibreSolar MPPT 2420 HC, a 20 A solar charge controller (CERN-OHL-W-2.0) — a real synchronous buck with switch nodes, a derived commutation loop and a near-field map">
        try the demo board — a real solar MPPT converter</button>
      <button class="chip" data-testid="open-glossary" @click="glossaryOpen = true">
        rule glossary — what Faraday checks, and the physics</button>
      <p class="sub">Faraday screens the whole board for coupled runs, return-path breaks,
         via and open stubs, decoupling reach, and — on converters — switch nodes and
         commutation-loop area. It ranks the risk and renders it on the copper.
         Analysis runs here, in your browser — your layout never leaves this machine.</p>
    </div>

    <!-- Must stay OUTSIDE the work/empty v-if chain. Sitting between them broke
         the chain: the empty state's v-else-if bound to this element instead of
         to <main v-if="report">, so the drop zone rendered underneath a loaded
         board and split the flex height with it. -->
    <BenchPanel v-if="benchFinding && engine" :engine="engine" :finding="benchFinding"
                :title-a="netName(benchFinding.netA)" :title-b="netName(benchFinding.netB)"
                :layer="layerName(benchFinding.cuA)"
                @close="benchId = ''" />

    <EmissionsPanel v-if="emitFinding && engine" :engine="engine"
                    :finding="emitFinding"
                    :dvdt-area-mm2="dvdtAreaMm2"
                    :operating-point="operatingPoint" @close="emitId = ''" />

    <NearFieldPanel v-if="nfDetail && nearField && engine" :engine="engine"
                    :result="nearField" :params="nfParams"
                    :board="report?.board"
                    @close="nfDetail = false" @params="setNfParams" />

    <PdnPanel v-if="pdnOpen && engine && report" :engine="engine"
              @close="pdnOpen = false" />

    <ImpedancePanel v-if="calcOpen && engine" :engine="engine"
                    @close="calcOpen = false" />

    <GlossaryPanel v-if="glossaryOpen" :rule-counts="ruleCountMap"
                   :hidden-rules="hiddenRules"
                   @toggle-rule="toggleRule" @close="glossaryOpen = false" />

    <PartPanel v-if="partRef && report" :report="report" :refdes="partRef"
               :findings="findings" :sourced="sourcedParts"
               @close="partRef = ''" @adopt="adoptValue" @goto="gotoFinding"
               @sourced="rememberSourced" />

    <StackupPanel v-if="stackupOpen" :initial="customStackup"
                  :copper-hint="suggestCopper"
                  @apply="applyStackup" @close="stackupOpen = false" />

    <footer v-if="meta" class="metastrip" data-testid="meta-strip">
      <!-- Guided keeps ONE line of it: what was read and what it was screened
           on. The rest is provenance — essential when you are defending a
           number, noise when you are looking for what to fix. Switch-node
           candidates stay in both, because a heuristic's negative has to be as
           visible as its positive (ABT #410) and promoting one changes the
           review. -->
      <span v-if="basic" class="m" data-testid="meta-plain">
        Screened a <b>{{ report.format }}</b> board on <b>{{ meta.stackupSource }}</b><template
          v-if="meta.switchNodes?.length"> · <b>{{ meta.switchNodes.length }}</b>
          switching net(s) found</template><template
          v-if="dvdtAreaMm2 > 0"> · <b>{{ dvdtAreaMm2.toFixed(0) }} mm²</b> of switching copper</template>
      </span>
      <span v-if="operatingPoint" class="m sw" data-testid="meta-op">
        operating point: <b>{{ operatingPoint.source }}</b><template
          v-if="operatingPoint.label"> · {{ operatingPoint.label }}</template></span>
      <span v-if="!basic" class="m">format: <b>{{ report.format }}</b></span>
      <span v-if="!basic" class="m">stackup: <b>{{ meta.stackupSource }}</b></span>
      <span v-for="p in meta.planes" :key="p.layer" v-show="!basic" class="m">
        {{ p.layer }} <b>{{ p.isPlane ? 'plane' : 'signal' }}</b>
        <template v-if="p.zoneCoverage > 0"> · {{ Math.round(p.zoneCoverage * 100) }}% pour</template>
      </span>
      <span v-if="meta.switchNodes?.length && !basic" class="m sw" data-testid="meta-switchnodes">
        switch nodes:
        <template v-for="(n, i) in meta.switchNodes" :key="n">
          <template v-if="i"> · </template><b>{{ n }}</b><template
            v-if="meta.switchNodeSource?.[n] === 'user'"><i class="prov">user</i><button
              class="demote" data-testid="demote-sw"
              :title="`stop screening ${n} as a switch node`"
              @click="demoteNet(n)">×</button></template>
        </template></span>
      <!-- ABT #410: a heuristic's NEGATIVE must be as visible as its positive.
           These nets look like a converter but a linear regulator's LC filter
           has the identical external shape — the engine will not guess. -->
      <span v-if="swCandidates.length" class="m cand" data-testid="meta-sw-candidates">
        candidate switch node(s):
        <button v-for="c in swCandidates" :key="c.net" class="candbtn"
                :data-testid="`promote-${c.net}`"
                :title="`${c.wound.join('+')} with ${c.active.join('+')} looks like a `
                  + `monolithic converter — but a linear regulator + LC filter has the `
                  + `same shape, so Faraday won't guess. If this IS a switcher, click to `
                  + `screen it (recorded as user-declared in the report).`"
                @click="promoteNet(c.net)">⊕ {{ c.net }}</button></span>
      <span v-if="meta.diffPairsRecognized && !basic" class="m">{{ meta.diffPairsRecognized }} diff pair(s) recognized</span>
      <span v-if="meta.polygonOnlyNets?.length && !basic" class="m">
        {{ meta.polygonOnlyNets.length }} zone-routed net(s) judged by pour outline</span>
      <span v-if="meta.crossingCheckSkippedPlanes?.length" class="m warn">
        void check skipped on {{ meta.crossingCheckSkippedPlanes.join(', ') }} (no fill)</span>
      <span v-if="meta.approximatedArcs" class="m warn">{{ meta.approximatedArcs }} arc(s) chord-approximated</span>
      <span v-if="meta.droppedBelowFloorDb && !basic" class="m">{{ meta.droppedBelowFloorDb }} pairs below {{ meta.reportFloorDb }} dB floor</span>
      <span v-if="meta.droppedByFindingCap" class="m warn">{{ meta.droppedByFindingCap }} findings over cap — tighten scope</span>
      <span v-if="!report.board.bboxFromOutline" class="m warn">no Edge.Cuts outline — extents from geometry</span>
      <!-- import-time plausibility gate: the fatal tier never gets here (it
           refuses the board), so anything shown is the "odd, look at it" tier -->
      <span v-for="(n, i) in report.board.plausibilityNotes || []" :key="i"
            class="m warn" data-testid="plausibility-note">{{ n }}</span>
    </footer>
  </div>
</template>

<style scoped>
.shell { height: 100%; display: flex; flex-direction: column; }

.topbar {
  display: flex; align-items: center; gap: 14px;
  padding: 10px 16px;
  border-bottom: 1px solid var(--resin-edge);
  background: var(--resin);
}
.brand {
  font-family: var(--display); font-weight: 700; font-size: 22px;
  letter-spacing: 0.14em; color: var(--copper);
}
.tagline { color: var(--tin); font-size: 12.5px; }
/* The wordmark is a button when a board is open, but it should still read as
   the wordmark: no chrome until you point at it, and the same baseline as the
   heading it replaces. */
.brandbtn {
  display: flex; align-items: baseline; gap: 14px;
  padding: 2px 8px; margin-left: -8px; border-radius: 5px;
  border: 1px solid transparent; text-align: left;
}
.brandbtn:hover { border-color: var(--resin-edge); background: rgba(217, 139, 95, 0.07); }
.brandbtn:hover .brand { color: #f0a877; }
.brandbtn:focus-visible { outline: 2px solid var(--copper); outline-offset: 1px; }
@media (max-width: 900px) { .brandbtn .tagline { display: none; } }
.spacer { flex: 1; }

.filebtn {
  border: 1px solid var(--resin-edge); border-radius: 4px;
  padding: 6px 12px; color: var(--silk); cursor: pointer;
  font-family: var(--mono); font-size: 12.5px;
  background: var(--bare-fr4);
}
.filebtn:hover { border-color: var(--copper); }
.filebtn.warn { color: #e8b34a; border-color: #8a6a2a; }
.filebtn.warn:hover { border-color: #e8b34a; }
.filebtn input { position: absolute; width: 1px; height: 1px; opacity: 0; }

.stackup {
  background: var(--bare-fr4); color: var(--silk);
  border: 1px solid var(--resin-edge); border-radius: 4px;
  padding: 6px 8px; font-family: var(--mono); font-size: 12.5px;
}

.banner { padding: 12px 16px; font-size: 13.5px; }
.banner.error { background: #3a1a1e; color: #ffb3b8; font-family: var(--mono); }
.banner.wait { background: var(--resin); color: var(--tin); font-family: var(--mono); }
.banner.ask { background: #2a2418; color: var(--silk); display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.radbar .barx { margin-left: 8px; background: none; border: 0; color: var(--tin);
                font-size: 14px; cursor: pointer; padding: 0 4px; line-height: 1; }
.radbar .barx:hover { color: var(--silk); }
.banner.brief { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.banner.ask .x, .banner.brief .x { margin-left: auto; background: none; border: 0;
                 color: var(--tin); font-size: 15px; cursor: pointer;
                 padding: 2px 6px; line-height: 1; }
.banner.ask .x:hover, .banner.brief .x:hover { color: var(--silk); }
.viewtoggle {
  display: inline-flex; border: 1px solid var(--resin-edge); border-radius: 999px;
  overflow: hidden; margin-right: 4px;
  flex: none; white-space: nowrap;   /* a crowded header must not clip the switch */
}
.viewtoggle button {
  font-family: var(--mono); font-size: 11px; letter-spacing: 0.04em;
  padding: 3px 12px; color: var(--tin); background: transparent;
}
.viewtoggle button:hover { color: var(--copper); }
.viewtoggle button.on { background: var(--copper); color: var(--bare-fr4); }
.chip {
  border: 1px solid var(--heat-med); color: var(--heat-med);
  border-radius: 999px; padding: 5px 14px; font-size: 13px;
}
.chip:hover { background: var(--heat-med); color: var(--bare-fr4); }

.work { flex: 1; display: grid; grid-template-columns: 1fr 380px; min-height: 0; }
@media (max-width: 900px) { .work { grid-template-columns: 1fr; grid-template-rows: 55% 45%; } }

.empty {
  flex: 1; display: flex; flex-direction: column; align-items: center;
  justify-content: center; gap: 14px; text-align: center; padding: 24px;
  border: 2px dashed transparent; transition: border-color 0.15s;
}
.empty.over { border-color: var(--copper); }
.board-ghost {
  width: 220px; height: 140px; border-radius: 6px;
  border: 1.5px solid var(--resin-edge);
  background:
    linear-gradient(90deg, transparent 48%, var(--resin-edge) 48%, var(--resin-edge) 52%, transparent 52%),
    var(--resin);
}
.invite { font-family: var(--display); font-size: 26px; font-weight: 500; }
.invite code { font-family: var(--mono); color: var(--copper); font-size: 22px; }
.sub { max-width: 520px; color: var(--tin); font-size: 13.5px; }

.boardcell { position: relative; display: grid; min-width: 0; min-height: 0; }
.overlaybars {
  /* below the layer chips (top: 10px + chip height), clear of the toggles */
  position: absolute; left: 10px; top: 46px; bottom: 44px; z-index: 6;
  width: min(320px, 44%); display: flex; flex-direction: column; gap: 10px;
  overflow-y: auto; pointer-events: none;
}
.overlaybars.ghost .radbar { pointer-events: none; opacity: 0.25; }
.radbar {
  pointer-events: auto; flex: none;
  display: flex; gap: 6px; flex-direction: column; align-items: flex-start;
  padding: 10px 13px; border: 1px solid var(--resin-edge); border-radius: 8px;
  background: rgba(16, 22, 19, 0.9); backdrop-filter: blur(3px);
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
}
.radbar > b { color: var(--copper); letter-spacing: 0.06em; }
.radbar.nf > b { color: var(--heat-low); }
.radbar .detail {
  border: 1px solid var(--heat-low); border-radius: 3px; padding: 2px 9px;
  font-size: 11px; color: var(--heat-low);
}
.radbar .detail:hover { background: var(--heat-low); color: var(--bare-fr4); }
.radbar .warn { color: var(--heat-med); }
.radbar .top b { color: var(--silk); }
.radbar .top b.bad { color: var(--heat-high); }
.radbar .spec { display: flex; align-items: center; gap: 6px; }
.radbar .spec input { width: 90px; accent-color: var(--copper); }
.radbar .spec select { background: var(--resin); color: var(--silk);
  font-family: var(--mono); font-size: 11px; border: 1px solid var(--resin-edge);
  border-radius: 3px; padding: 2px 5px; }
.radbar .spec select:hover { border-color: var(--copper); }
.radbar .cav { flex: 1 1 100%; opacity: 0.75; font-family: var(--sans); font-size: 11px; }

.radbar.cat { border-color: #58c79a; }
.radbar.cat > b { color: #58c79a; }
.radbar.cat .key {
  display: grid; grid-template-columns: auto 1fr; gap: 3px 7px;
  align-items: center; opacity: 0.85; margin-top: 3px; font-size: 11px;
}
.radbar.cat .key i {
  width: 10px; height: 10px; border-radius: 2px; display: block;
}
.k-exact { background: rgba(88,199,154,0.5); border: 1px solid #58c79a; }
.k-cand { background: rgba(255,180,84,0.4); border: 1px solid #ffb454; }
.k-none { background: rgba(255,93,93,0.3); border: 1px solid rgba(255,93,93,0.65); }
.k-unk { background: rgba(6,9,8,0.62); border: 1px solid rgba(120,134,129,0.45); }

.metastrip {
  display: flex; gap: 18px; flex-wrap: wrap;
  padding: 7px 16px; border-top: 1px solid var(--resin-edge);
  background: var(--resin);
  font-family: var(--mono); font-size: 11.5px; color: var(--tin);
}
.m b { color: var(--silk); font-weight: 500; }
.m.warn { color: var(--heat-med); }
.m.sw b { color: var(--copper); }
.m.sw .prov {
  font-style: normal; font-size: 10px; color: #9ecbff;
  border: 1px solid #3a5a7a; border-radius: 3px; padding: 0 3px; margin-left: 4px;
}
.m.sw .demote {
  background: none; border: none; color: var(--tin); cursor: pointer;
  font-size: 12px; padding: 0 2px;
}
.m.sw .demote:hover { color: #ff8a8a; }
.m.cand .candbtn {
  background: none; cursor: pointer; font-size: 11.5px;
  color: #58c79a; border: 1px solid #2a5a46; border-radius: 4px;
  padding: 1px 6px; margin-left: 4px;
}
.m.cand .candbtn:hover { border-color: #58c79a; }
</style>
