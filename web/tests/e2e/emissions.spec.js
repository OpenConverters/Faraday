// Radiated-emission estimation, end to end. The loop area comes off the copper
// of a real converter board, so this covers the whole chain: import, the
// commutation-loop rule, the WASM prediction, and the chart.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Loading a 1.16 MB board means downloading a 656 kB WASM engine and parsing
// the layout. Locally both are cached; against a deployed site neither is, and
// under a serialized suite that legitimately exceeds 30 s. One constant so the
// budget cannot drift between specs.
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')

async function openEmissions(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  // This board carries no stackup, so Faraday asks before analysing. Reading
  // the file and running the screener is async, so the card has not rendered
  // yet the instant setInputFiles resolves — wait for EITHER outcome rather
  // than assuming which one won the race.
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })

  const ids = await page.evaluate(() =>
    [...document.querySelectorAll('[data-testid^="finding-F-"]')]
      .map(e => e.dataset.testid.replace('finding-', '')))
  for (const id of ids) {
    await page.getByTestId(`finding-${id}`).click()
    if (await page.getByTestId(`emit-${id}`).count()) {
      await page.getByTestId(`emit-${id}`).click()
      await expect(page.getByTestId('emissions')).toBeVisible()
      return id
    }
    await page.getByTestId(`finding-${id}`).click()
  }
  throw new Error('no finding on this board offered a loop area')
}

const margin = async page => {
  const t = await page.getByTestId('emissions-verdict').textContent()
  return Number(t.match(/([+-][\d.]+)\s*dB/)[1])
}

test('a converter loop predicts a margin against the limit line', async ({ page }) => {
  const consoleErrors = []
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()) })
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await openEmissions(page)
  await expect(page.getByTestId('emissions-error')).toHaveCount(0)

  const v = page.getByTestId('emissions-verdict')
  await expect(v).toContainText('dBµV/m limit')
  await expect(v).toContainText(/MHz/)
  // a margin has to be a real number in a believable range
  const m = await margin(page)
  expect(m).toBeGreaterThan(-80)
  expect(m).toBeLessThan(80)

  // the chart is drawn, not blank
  const colours = await page.getByTestId('emissions-chart').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    const seen = new Set()
    for (let i = 0; i < d.length; i += 4) seen.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
    return seen.size
  })
  expect(colours).toBeGreaterThan(12)

  expect(consoleErrors).toEqual([])
})

test('the caveat is never hidden', async ({ page }) => {
  // A radiated-emission number without its limits is worse than no number.
  await openEmissions(page)
  const c = page.getByTestId('emissions-caveat')
  await expect(c).toBeVisible()
  await expect(c).toContainText('not a compliance prediction')
  await expect(c).toContainText('common-mode')
})

test('the levers move the margin the way the physics says', async ({ page }) => {
  await openEmissions(page)
  const before = await margin(page)

  // halving the switched current is 6 dB of margin back
  const cur = page.getByTestId('emissions-current')
  const now = Number(await cur.inputValue())
  await cur.fill(String(now / 2))
  await cur.dispatchEvent('input')
  await expect.poll(() => margin(page)).toBeCloseTo(before + 6.02, 0)

  // and a slower edge buys the same
  await cur.fill(String(now))
  await cur.dispatchEvent('input')
  const rise = page.getByTestId('emissions-rise')
  const r0 = Number(await rise.inputValue())
  await rise.fill(String(r0 * 2))
  await rise.dispatchEvent('input')
  await expect.poll(() => margin(page)).toBeCloseTo(before + 6.02, 0)
})

test('switching standards changes the limit that is applied', async ({ page }) => {
  await openEmissions(page)
  const b = await margin(page)
  // Class A is specified at 10 m rather than 3 m, so the same source has more
  // margin against it — the distance must be carried through, not ignored.
  await page.getByTestId('emissions-limit').selectOption('cispr32a')
  await expect(page.getByTestId('emissions-verdict')).toContainText('dB')
  const a = await margin(page)
  expect(a).toBeGreaterThan(b)
})

test('findings that carry a deep tool are marked in the collapsed row',
  async ({ page }) => {
    // The buttons live inside the detail, so without a mark in the row you have
    // to open findings one by one to discover which ones support anything.
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles(MPPT)
    const card = page.getByTestId('stackup-card')
    await expect(card.or(page.getByTestId('finding-F-0001')).first())
      .toBeVisible({ timeout: LOAD_MS })
    if (await card.count()) await card.getByText('Default 4-layer').click()
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })

    await expect(page.getByTestId('tools-legend')).toBeVisible()

    // F-0002 is the commutation loop, so it must carry the emissions mark
    await expect(page.getByTestId('tools-F-0002')).toBeVisible()
    await expect(page.getByTestId('tools-F-0002')).toHaveAttribute(
      'title', /Emissions/)

    // and a return-path break carries neither, so it gets no mark at all
    await expect(page.getByTestId('tools-F-0001')).toHaveCount(0)

    // every marked row must actually open the tool it advertises
    const marked = await page.evaluate(() =>
      [...document.querySelectorAll('[data-testid^="tools-F-"]')]
        .map(e => e.dataset.testid.replace('tools-', '')).slice(0, 4))
    expect(marked.length).toBeGreaterThan(0)
    for (const id of marked) {
      const title = await page.getByTestId(`tools-${id}`).getAttribute('title')
      await page.getByTestId(`finding-${id}`).click()
      if (/Field solve/.test(title))
        await expect(page.getByTestId(`bench-${id}`)).toBeVisible()
      if (/Emissions/.test(title))
        await expect(page.getByTestId(`emit-${id}`)).toBeVisible()
      await page.getByTestId(`finding-${id}`).click()
    }
  })

test('the common-mode budget answers the caveat with a number', async ({ page }) => {
  await openEmissions(page)
  const b = page.getByTestId('cm-budget')
  await expect(b).toBeVisible()
  // the whole point is that it lands in microamps, not milliamps
  await expect(b).toContainText('µA')
  const ua = async () =>
    Number((await page.getByTestId('cm-tightest').textContent()).match(/([\d.]+)/)[1])
  const one = await ua()
  expect(one).toBeGreaterThan(0.2)
  expect(one).toBeLessThan(50)

  // A SHORTER cable radiates less, so it is allowed more current.
  const len = page.getByTestId('cm-cable')
  await len.fill('0.2')
  await len.dispatchEvent('input')
  await expect.poll(ua).toBeGreaterThan(one)

  // But past a quarter wave, length stops mattering: the tightest point sits at
  // 230 MHz where lambda/4 is 0.33 m, so a 1 m and a 3 m cable are capped to
  // the same effective length and get the same budget. Worth pinning — it is
  // the least obvious consequence of the model and a real design insight.
  await len.fill('3')
  await len.dispatchEvent('input')
  await expect.poll(ua).toBeCloseTo(one, 1)

  await len.fill('1')
  await len.dispatchEvent('input')
  await expect.poll(ua).toBeCloseTo(one, 1)
  await page.getByTestId('emissions-limit').selectOption('cispr32a')
  await expect.poll(ua).toBeGreaterThan(one)
})
