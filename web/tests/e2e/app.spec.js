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

test('board without stackup gets the explicit stackup card, never a silent default', async ({ page }) => {
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
  const card = page.getByTestId('stackup-card')
  await expect(card).toBeVisible()
  await card.getByText('Default 2-layer FR4').click()
  await expect(page.getByTestId('finding-count')).toBeVisible()
  await expect(page.getByTestId('meta-strip')).toContainText('user:default-2layer')
})

test('power converter: switch node identified and shown in the meta strip', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await page.goto('/')
  // KiCad 5 power board, carries no stackup -> explicit card first
  await page.getByTestId('file-input').setInputFiles(
    path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb'))
  await page.getByTestId('stackup-card').getByRole('button', { name: /4-layer/ }).click()

  await expect(page.getByTestId('finding-count')).toBeVisible()
  // the converter's switch node, found by connectivity not by name
  await expect(page.getByTestId('meta-switchnodes')).toContainText('SW_NODE')
  await expect(page.getByTestId('meta-strip')).toContainText('user:default-4layer')
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
