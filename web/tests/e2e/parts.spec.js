// The parts layer and the part inspector: every component drawn as a body
// over its pads; a click opens the board's facts about it, then Kelvin's —
// the catalogue record, the datasheet link, the cross-references.
//
// The catalogue half needs the Kelvin data set. The dev server proxies
// /kelvin.js and /kelvin/* from kelvin.openconverters.com (prod serves them
// same-origin off the shared /cache), so these tests reach the REAL
// catalogue over the network and download a family's shard the first time
// a browser context opens it. That is the product, not a stub, and when the
// catalogue is unreachable the tests fail and say so — the inspector must
// never pretend to have looked something up.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const PARTS = path.join(here, 'fixtures/parts.kicad_pcb')
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000
// a shard is a real download (the capacitor family is ~34 MB)
const CATALOGUE_MS = 180000

async function loadFixture(page, file) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(file)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('board-canvas')).first()).toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('board-canvas')).toBeVisible({ timeout: LOAD_MS })
}

// click the board at a WORLD coordinate, through the canvas's own transform
async function clickWorld(page, x, y) {
  const canvas = page.getByTestId('board-canvas')
  const view = JSON.parse(await canvas.getAttribute('data-view'))
  const box = await canvas.boundingBox()
  await page.mouse.click(box.x + (x - view.ox) * view.scale, box.y + (y - view.oy) * view.scale)
}

test('every component is drawn as a body, and the chip turns the layer off', async ({ page }) => {
  await loadFixture(page, MPPT)
  const canvas = page.getByTestId('board-canvas')
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBeGreaterThan(100)
  await page.getByTestId('parts-toggle').click()
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBe(0)
  await page.getByTestId('parts-toggle').click()
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBeGreaterThan(100)
})

test('a mounting hole is not a part; a capacitor is, with its pins and nets', async ({ page }) => {
  await loadFixture(page, PARTS)
  const canvas = page.getByTestId('board-canvas')
  // C1, Q1, R1, Q2 — and not H1, which has no pads
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBe(4)

  await clickWorld(page, 10, 20)
  const panel = page.getByTestId('part-panel')
  await expect(panel).toBeVisible()
  await expect(panel.getByRole('heading', { name: 'C1' })).toBeVisible()
  await expect(page.getByTestId('part-value')).toHaveText('100n')
  const pins = page.getByTestId('part-pins')
  await expect(pins).toContainText('CLK')
  await expect(pins).toContainText('GND')
  await expect(pins).toContainText('F.Cu')
})

test('no part number: the value and package are matched against the catalogue', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  const errors = []
  page.on('pageerror', e => errors.push(String(e)))
  await loadFixture(page, PARTS)
  await clickWorld(page, 10, 20)
  await expect(page.getByTestId('part-panel')).toBeVisible()
  await expect(page.getByTestId('part-error')).toHaveCount(0)

  const byValue = page.getByTestId('part-by-value')
  await expect(byValue).toBeVisible({ timeout: CATALOGUE_MS })
  await expect(byValue).toContainText('catalogue part(s) match')
  await expect(byValue).toContainText('100 nF')
  await expect(byValue).toContainText('0603')
  // a real match list, and picking one makes it the original for the record
  const first = byValue.locator('button.lnk').first()
  await expect(first).toBeVisible()
  await first.click()
  await expect(page.getByTestId('part-record')).toBeVisible({ timeout: CATALOGUE_MS })
  await expect(page.getByTestId('part-ident')).toContainText('chosen from the matches below')
  expect(errors).toEqual([])
})

test('a catalogued part number: exact match, datasheet, cross-references', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  const errors = []
  page.on('pageerror', e => errors.push(String(e)))
  await loadFixture(page, PARTS)
  await clickWorld(page, 25, 20)
  const panel = page.getByTestId('part-panel')
  await expect(panel).toBeVisible()
  await expect(panel.getByRole('heading', { name: 'Q1' })).toBeVisible()

  const ident = page.getByTestId('part-ident')
  await expect(ident).toContainText('EPC2019', { timeout: CATALOGUE_MS })
  await expect(ident).toContainText('exact part-number match')
  await expect(page.getByTestId('part-error')).toHaveCount(0)

  // the record, with the vendor's own datasheet link
  const ds = page.getByTestId('part-datasheet')
  await expect(ds).toBeVisible({ timeout: CATALOGUE_MS })
  expect(await ds.getAttribute('href')).toMatch(/^https?:\/\//)
  // the board's footprint reads as SOT-23 and the catalogue says BGA — said, not hidden
  await expect(page.getByTestId('part-record')).toContainText('case')

  // cross-references: Kelvin's ranker over every other manufacturer
  const table = page.getByTestId('part-xref-table')
  await expect(table).toBeVisible({ timeout: CATALOGUE_MS })
  expect(await table.locator('tbody tr:not(.notes)').count()).toBeGreaterThan(0)
  await expect(page.getByTestId('part-xref-error')).toHaveCount(0)
  expect(errors).toEqual([])
})

test('an unknown part number says which catalogues were searched, and offers the rest',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    await loadFixture(page, PARTS)
    await clickWorld(page, 25, 6)
    const ident = page.getByTestId('part-ident')
    await expect(ident).toContainText('is not in the', { timeout: CATALOGUE_MS })
    await expect(ident).toContainText('XYZ9999ABC')
    await expect(ident).toContainText('mosfet')
    await expect(page.getByTestId('part-search-more')).toBeVisible()
    await expect(page.getByTestId('part-search-more')).toContainText('families')
  })
