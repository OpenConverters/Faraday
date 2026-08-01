// Faraday web e2e — headless always (house rule). The WASM engine must be
// built into web/public/ first (npm run wasm).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const FIXTURE = path.join(here, '../../../cpp/tests/fixtures/fixture_2layer.kicad_pcb')

test('board loads, findings rank, selection links list and canvas', async ({ page }) => {
  const consoleErrors = []
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()) })
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await page.goto('/')
  await expect(page.getByText('Drop a')).toBeVisible()

  await page.getByTestId('file-input').setInputFiles(FIXTURE)

  // findings appear, ranked: the quantified coupled run leads, its 3W
  // companion sits just below it. Assert by content, not by index, so adding
  // a rule does not break the test for the wrong reason.
  await expect(page.getByTestId('finding-F-0001')).toContainText('CLK')
  await expect(page.getByTestId('finding-F-0002')).toContainText('3W violation')
  await expect(page.getByTestId('board-canvas')).toBeVisible()

  // The drop zone must be GONE once a board is loaded. It is a v-else-if on the
  // work area, and an element inserted between the two silently re-chains it to
  // whatever now precedes it — legal Vue, no warning, and the empty state ends
  // up rendering underneath the board and splitting the flex height with it.
  await expect(page.getByText('Drop a board here')).toBeHidden()

  // meta strip states the stackup source and plane classification
  const meta = page.getByTestId('meta-strip')
  await expect(meta).toContainText('board-file')
  await expect(meta).toContainText('B.Cu')
  await expect(meta).toContainText('plane')

  // selecting a finding opens its detail with remediation
  await page.getByTestId('finding-F-0001').click()
  await expect(page.getByTestId('finding-detail')).toContainText('NEXT')
  await expect(page.getByTestId('finding-detail')).toContainText('Fix:')

  expect(consoleErrors).toEqual([])
})

// A board whose file carries no dielectric OPENS — the layer count is read off
// the board, so there is nothing to ask about that. What is assumed is the
// dielectric, and that assumption must be impossible to miss and one click
// from being replaced: an "assumed:" provenance in the meta strip (so an
// exported report never claims the user chose it) and a standing banner.
test('board without stackup opens on an assumed dielectric, and says so everywhere',
     async ({ page }) => {
  await page.goto('/')
  const bare = `(kicad_pcb
    (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
    (net 0 "") (net 1 "A") (net 2 "B")
    (segment (start 5 10) (end 25 10) (width 0.3) (layer "F.Cu") (net 1))
    (segment (start 5 10.5) (end 25 10.5) (width 0.3) (layer "F.Cu") (net 2))
  )`
  await page.getByTestId('file-input').setInputFiles({
    name: 'bare.kicad_pcb', mimeType: 'text/plain', buffer: Buffer.from(bare),
  })
  // no card, no click: the board is screened straight away
  await expect(page.getByTestId('finding-count')).toBeVisible()
  await expect(page.getByTestId('stackup-card')).toBeHidden()
  // provenance says ASSUMED, never "user:" — nobody chose this dielectric
  await expect(page.getByTestId('meta-strip')).toContainText('assumed:default-2layer')
  // a compact header button carries the assumption (a banner ate a full row);
  // the full wording lives in its tooltip, the fix is one click
  const btn = page.getByTestId('stackup-assumed')
  await expect(btn).toBeVisible()
  await expect(btn).toContainText('assumed stackup')
  await expect(btn).toHaveAttribute('title', /dielectric/)
  await btn.click()
  await expect(page.getByTestId('stackup-editor')).toBeVisible()
})

test('power converter: switch node identified and shown in the meta strip', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await page.goto('/')
  // KiCad 5 power board, carries no stackup -> opens on the assumed dielectric
  // at the copper count read off the board (4), with no click needed
  await page.getByTestId('file-input').setInputFiles(
    path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb'))

  await expect(page.getByTestId('finding-count')).toBeVisible()
  // the converter's switch node, found by connectivity not by name
  await expect(page.getByTestId('meta-switchnodes')).toContainText('SW_NODE')
  await expect(page.getByTestId('meta-strip')).toContainText('assumed:default-4layer')
  // and its commutation loop — the headline converter EMC metric
  const loop = page.locator('[data-testid^="finding-"]', { hasText: 'Commutation loop' }).first()
  await expect(loop).toContainText('mm²')
  await loop.click()
  await expect(page.getByTestId('finding-detail')).toContainText('discontinuous switching current')
  expect(consoleErrors).toEqual([])
})

test('rule filter mutes a rule in both the list and the board overlay', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('rule-filters')).toBeVisible()
  const total = Number(await page.getByTestId('finding-count').textContent())
  const breaks = page.locator('[data-testid^="finding-F-"]', { hasText: 'Return-path break' })
  const nBreaks = await breaks.count()
  expect(nBreaks).toBeGreaterThan(0)

  // muting a rule removes it from the list (and the board draws the same set)
  await page.getByTestId('rule-plane-crossing').click()
  await expect(page.getByTestId('finding-count')).toHaveText(String(total - nBreaks))
  await expect(breaks).toHaveCount(0)

  // toggling back restores them
  await page.getByTestId('rule-plane-crossing').click()
  await expect(page.getByTestId('finding-count')).toHaveText(String(total))
  await expect(breaks).toHaveCount(nBreaks)
})

test('HYP and IPC-2581 boards load, detected by content', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', e => consoleErrors.push(String(e)))
  await page.goto('/')

  for (const [file, format] of [['fixture_4layer.hyp', 'hyp'],
                                ['fixture_4layer.xml', 'ipc2581']]) {
    await page.getByTestId('file-input').setInputFiles(
      path.join(here, '../../../cpp/tests/fixtures/', file))
    await expect(page.getByTestId('finding-count')).toBeVisible()
    await expect(page.getByTestId('meta-strip')).toContainText(`format: ${format}`)
    // both fixtures describe the same CLK/DATA pair over a plane
    await expect(page.getByTestId('meta-strip')).toContainText('board-file')
    await expect(page.locator('[data-testid^="finding-F-"]',
                              { hasText: 'CLK' }).first()).toBeVisible()
  }
  expect(consoleErrors).toEqual([])
})

test('tooltip appears when hovering routed copper', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('board-canvas')).toBeVisible()

  // sweep a horizontal line across the canvas center to cross the CLK/DATA
  // traces regardless of the exact fitted transform
  const box = await page.getByTestId('board-canvas').boundingBox()
  let seen = false
  for (let fy = 0.2; fy <= 0.8 && !seen; fy += 0.02) {
    await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * fy)
    seen = await page.getByTestId('board-tooltip').isVisible()
  }
  expect(seen).toBe(true)
})

test('a board dropped before the engine has loaded is not silently lost',
  async ({ page }) => {
    // The engine is a 650 kB WASM download and compile. Locally it wins the
    // race against any human, so this path never runs — but over a real
    // network it loses, and the app used to hit `if (!engine) return` and drop
    // the board with no analysis, no error and no retry. Found by the live
    // suite: the FIRST test in every spec failed while the rest passed in ~2 s,
    // because only the first paid for a cold engine.
    await page.route('**/faraday.wasm', async route => {
      await new Promise(r => setTimeout(r, 3000))
      await route.continue()
    })
    await page.goto('/')
    // pick a board immediately — before the engine can possibly be ready
    await page.getByTestId('file-input').setInputFiles(FIXTURE)
    await expect(page.getByTestId('engine-loading')).toBeVisible()

    // and it must still analyse once the engine arrives, rather than sitting
    // there having quietly discarded the file
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: 30000 })
    await expect(page.getByTestId('engine-loading')).toHaveCount(0)
  })

test('the glossary is reachable from the empty state, before any board', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByText('Drop a board here')).toBeVisible()
  await page.getByTestId('open-glossary').click()
  const panel = page.getByTestId('glossary')
  await expect(panel).toBeVisible()
  // rule entries render with their physics even with zero findings loaded
  await expect(panel).toContainText('Commutation loop')
  await expect(panel).toContainText('Franz')
})

test('a candidate switch node is offered, promoted with provenance, demoted',
  async ({ page }) => {
    // A monolithic buck: switcher IC + inductor, no discrete FET. The engine
    // must NOT screen it on its own (an LDO + LC filter has the identical
    // external shape) — it must offer it, and promotion must carry
    // switchNodeSource "user" and enable the near-field toggle. ABT #408/#410.
    const buck = `(kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SWX") (net 2 "GND") (net 3 "VIN") (net 4 "VOUT")
      (segment (start 5 5) (end 12 5) (width 1.0) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
      (footprint "buck" (layer "F.Cu") (at 5 5)
        (property "Reference" "U1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VIN"))
        (pad "2" smd rect (at 1 0) (size 1 1) (layers "F.Cu") (net 1 "SWX"))
        (pad "3" smd rect (at 2 0) (size 1 1) (layers "F.Cu") (net 2 "GND"))
        (pad "4" smd rect (at 3 0) (size 1 1) (layers "F.Cu") (net 0 ""))
        (pad "5" smd rect (at 4 0) (size 1 1) (layers "F.Cu") (net 0 "")))
      (footprint "l" (layer "F.Cu") (at 12 5)
        (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 1.5 1.5) (layers "F.Cu") (net 1 "SWX"))
        (pad "2" smd rect (at 2 0) (size 1.5 1.5) (layers "F.Cu") (net 4 "VOUT")))
      (footprint "cin" (layer "F.Cu") (at 3 8)
        (property "Reference" "C1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VIN"))
        (pad "2" smd rect (at 1.5 0) (size 1 1) (layers "F.Cu") (net 2 "GND")))
      (footprint "cout" (layer "F.Cu") (at 16 8)
        (property "Reference" "C2")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 4 "VOUT"))
        (pad "2" smd rect (at 1.5 0) (size 1 1) (layers "F.Cu") (net 2 "GND")))
    )`
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles({
      name: 'buck.kicad_pcb', mimeType: 'text/plain', buffer: Buffer.from(buck),
    })
    await expect(page.getByTestId('finding-count')).toBeVisible()

    // offered, not screened; the near-field chip is disabled but says why
    await expect(page.getByTestId('meta-switchnodes')).toHaveCount(0)
    const cand = page.getByTestId('promote-SWX')
    await expect(cand).toBeVisible()
    await expect(cand).toHaveAttribute('title', /won't guess/)
    const nf = page.getByTestId('nf-toggle')
    await expect(nf).toBeDisabled()
    await expect(nf).toHaveAttribute('title', /candidate/)

    // promotion: screened, provenance "user", near field enabled
    await cand.click()
    const swMeta = page.getByTestId('meta-switchnodes')
    await expect(swMeta).toContainText('SWX')
    await expect(swMeta).toContainText('user')
    await expect(page.getByTestId('meta-sw-candidates')).toHaveCount(0)
    await expect(nf).toBeEnabled()

    // demotion: back to a candidate
    await page.getByTestId('demote-sw').click()
    await expect(page.getByTestId('meta-switchnodes')).toHaveCount(0)
    await expect(page.getByTestId('promote-SWX')).toBeVisible()
  })
