// The batch-six features: PDN, calculator, export, sweep. Headless always.
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const FIXTURE = path.join(here, '../../../cpp/tests/fixtures/fixture_2layer.kicad_pcb')

async function loadMppt(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
}

test('the PDN panel measures mounting inductance off the board', async ({ page }) => {
  const errs = []
  page.on('pageerror', e => errs.push(String(e)))
  await loadMppt(page)
  await page.getByTestId('pdn-toggle').click()
  await expect(page.getByTestId('pdn-panel')).toBeVisible()
  await expect(page.getByTestId('pdn-error')).toHaveCount(0)

  // the verdict speaks in impedance against a user-derived target
  await expect(page.getByTestId('pdn-verdict')).toContainText(/[mΩ]Ω/)
  await expect(page.getByTestId('pdn-verdict')).toContainText('target')

  // the cap table carries the measured mounting inductance — the point
  const caps = await page.getByTestId('pdn-caps').textContent()
  expect(caps).toMatch(/nH/)
  expect(caps).toMatch(/MHz/)

  // the chart is drawn
  const colours = await page.getByTestId('pdn-chart').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    const s = new Set()
    for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
    return s.size
  })
  expect(colours).toBeGreaterThan(8)
  expect(errs).toEqual([])
})

test('the impedance calculator works with no board at all', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('open-calc').click()
  await expect(page.getByTestId('calc-panel')).toBeVisible()
  await expect(page.getByTestId('calc-error')).toHaveCount(0)

  const z0 = async () => {
    const t = await page.getByTestId('calc-out').textContent()
    const m = t.match(/Z₀ \(single\)([\d.]+)/)
    return m ? Number(m[1]) : null
  }
  await expect.poll(z0).toBeGreaterThan(20)
  expect(await z0()).toBeLessThan(200)

  // a wider trace must be lower impedance — the physics, not a lookup
  const before = await z0()
  await page.getByTestId('calc-w').fill('1.2')
  await page.getByTestId('calc-w').dispatchEvent('input')
  await expect.poll(z0).toBeLessThan(before)

  // and the width finder converges on the target
  await page.getByTestId('calc-target').fill('50')
  await page.getByTestId('calc-find').click()
  await expect(page.getByTestId('calc-found')).toBeVisible()
  const found = await page.getByTestId('calc-found').textContent()
  const hit = Number(found.match(/→ ([\d.]+) Ω/)[1])
  expect(Math.abs(hit - 50)).toBeLessThan(1.0)
})

test('the exported report is a self-contained review', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })

  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByTestId('export-report').click(),
  ])
  const file = await download.path()
  const fs = await import('node:fs')
  const html = fs.readFileSync(file, 'utf8')
  expect(html).toContain('F-0001')
  expect(html).toContain('Screening estimates, not compliance predictions')
  expect(html).toContain('never left the machine')
  // self-contained: no external scripts or styles
  expect(html).not.toMatch(/src=|href=/)
})

test('the bench shows the whole separation curve, not one point', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
  await page.getByTestId('finding-F-0001').click()
  await page.getByTestId('bench-F-0001').click()
  await expect(page.getByTestId('bench-verdict')).toBeVisible()

  const painted = await page.getByTestId('bench-sweep').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    const s = new Set()
    for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
    return s.size
  })
  expect(painted, 'the sweep strip must be drawn').toBeGreaterThan(5)
})

test('victims carry a cos(theta) from their own routing', async ({ page }) => {
  await loadMppt(page)
  await page.getByTestId('nf-toggle').click()
  await expect(page.getByTestId('nf-bar')).toBeVisible()
  await page.getByTestId('nf-detail').click()
  await expect(page.getByTestId('nf-victims')).toBeVisible()
  const t = await page.getByTestId('nf-victims').textContent()
  expect(t).toContain('cosθ')
})
