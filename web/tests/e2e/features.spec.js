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

test('glossary explains every rule and can hide types from inside it',
  async ({ page }) => {
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles(FIXTURE)
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })

    await page.getByTestId('open-glossary').click()
    await expect(page.getByTestId('glossary')).toBeVisible()
    // every screening rule has an entry with the honest confidence stated
    for (const id of ['coupled-run', 'plane-crossing', 'commutation-loop',
                      'via-stub', 'diff-skew', 'decoupling-distance'])
      await expect(page.getByTestId(`gloss-${id}`)).toContainText('confidence:')
    // the ±6 dB caveat lives in the glossary too, not only in findings
    await expect(page.getByTestId('gloss-coupled-run')).toContainText('6.5 dB optimistic')

    // hiding a type from the glossary drives the same filter as the chips
    const total = Number(await page.getByTestId('finding-count').textContent())
    await page.getByTestId('gloss-toggle-plane-crossing').click()
    await page.getByTestId('glossary').press('Escape')
    await expect(page.getByTestId('finding-count')).not.toHaveText(String(total))
  })

test('individual findings can be dismissed and restored', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
  const total = Number(await page.getByTestId('finding-count').textContent())

  await page.getByTestId('finding-F-0001').hover()
  await page.getByTestId('dismiss-F-0001').click()
  await expect(page.getByTestId('finding-F-0001')).toHaveCount(0)
  await expect(page.getByTestId('finding-count')).toHaveText(String(total - 1))

  await page.getByTestId('restore-hidden').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible()
  await expect(page.getByTestId('finding-count')).toHaveText(String(total))
})

test('a board loads from a URL hash — the demo and the KiCad bridge path',
  async ({ page }) => {
    await page.goto('/')
    await page.getByTestId('load-demo').click()
    // the demo board is served by the site itself and analysed locally
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
    await expect(page.getByTestId('meta-strip')).toContainText('format: kicad')
  })

test('a second aggressor doubles the symmetric victim noise', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
  await page.getByTestId('finding-F-0001').click()
  await page.getByTestId('bench-F-0001').click()
  await expect(page.getByTestId('bench-verdict')).toBeVisible()

  const peak = () => page.getByTestId('bench-verdict').evaluate(el => {
    const m = el.textContent.match(/([\d.]+)\s*mV/)
    return m ? Number(m[1]) : null
  })
  const one = await peak()
  await page.getByTestId('bench-triple').check()
  // in-phase superposition on a symmetric triple: +6 dB, i.e. exactly 2x
  await expect.poll(peak).toBeGreaterThan(one * 1.7)
  await expect.poll(peak).toBeLessThan(one * 2.3)
})

test('a gerber X2 zip imports as a set, asks for a stackup, and analyses',
  async ({ page }) => {
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles(
      path.join(here, '../../../cpp/tests/fixtures/gerber_set.zip'))
    // gerber carries no stackup — the card must appear, never a silent default
    await expect(page.getByTestId('stackup-card')).toBeVisible({ timeout: LOAD_MS })
    await page.getByTestId('stackup-card')
      .getByText('Default 2-layer FR4').click()
    await expect(page.getByTestId('meta-strip'))
      .toContainText('format: gerber-x2', { timeout: LOAD_MS })
    // and the analysis actually ran: the set's open-ended trace is found
    await expect(page.getByTestId('finding-F-0001')).toBeVisible()
  })

test('toggling the return-path overlay does not move the board', async ({ page }) => {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(FIXTURE)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
  const canvas = page.locator('.boardcell canvas').first()
  const before = await canvas.boundingBox()

  await page.getByTestId('rp-toggle').click()
  await expect(page.getByTestId('rp-bar')).toBeVisible({ timeout: LOAD_MS })
  const during = await canvas.boundingBox()
  // the summary floats over the board's left edge; the copper must not shift
  expect(during.y).toBe(before.y)
  expect(during.height).toBe(before.height)

  // toggling back off must not move it either
  await page.getByTestId('rp-toggle').click()
  await expect(page.getByTestId('rp-bar')).toHaveCount(0)
  const after = await canvas.boundingBox()
  expect(after.y).toBe(before.y)
  expect(after.height).toBe(before.height)
})
