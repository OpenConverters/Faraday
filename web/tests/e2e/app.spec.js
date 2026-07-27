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

  // findings appear, ranked (fixture: 3W, coupled-run, 2 plane crossings)
  await expect(page.getByTestId('finding-count')).toHaveText('4')
  await expect(page.getByTestId('finding-F-0001')).toContainText('3W violation')
  await expect(page.getByTestId('finding-F-0002')).toContainText('CLK')
  await expect(page.getByTestId('board-canvas')).toBeVisible()

  // meta strip states the stackup source and plane classification
  const meta = page.getByTestId('meta-strip')
  await expect(meta).toContainText('board-file')
  await expect(meta).toContainText('B.Cu')
  await expect(meta).toContainText('plane')

  // selecting a finding opens its detail with remediation
  await page.getByTestId('finding-F-0002').click()
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

test('tooltip appears when hovering routed copper', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-count')).toHaveText('4')

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
