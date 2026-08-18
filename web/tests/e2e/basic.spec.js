// GUIDED mode — the review in the language of the person who drew the board,
// and the conducted answer that used to live one site away.
//
// What is pinned here is the promise the toggle makes: the SAME findings, the
// same engine, fewer words. So the guided assertions are not "something is on
// screen" — they check that the plain sentence belongs to the same finding the
// advanced text describes, and that switching back produces the engineering
// text on the same row.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000

async function loadBoard(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
}

test('a first visit lands in guided, and the toggle is in the header', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('view-toggle')).toBeVisible()
  await expect(page.getByTestId('view-guided')).toHaveAttribute('aria-pressed', 'true')
  await expect(page.getByTestId('view-advanced')).toHaveAttribute('aria-pressed', 'false')
})

test('guided says what to do; advanced says how it was measured — same finding',
     async ({ page }) => {
  await loadBoard(page)

  // guided: a risk word instead of a decibel, plain sentences in the detail
  await expect(page.getByTestId('risk-F-0001')).toBeVisible()
  await expect(page.getByTestId('rule-filters')).toHaveCount(0)
  await page.getByTestId('finding-F-0001').click()
  const plain = page.getByTestId('plain-says')
  await expect(plain).toBeVisible()
  const plainText = await plain.textContent()
  await expect(page.getByTestId('finding-detail')).toContainText('What to do:')

  // the same row in advanced: the engineering text, the rule and confidence
  // tags, and the plain sentence gone
  await page.getByTestId('view-advanced').click()
  await expect(page.getByTestId('plain-says')).toHaveCount(0)
  const detail = page.getByTestId('finding-detail')
  await expect(detail).toContainText('Fix:')
  await expect(page.getByTestId('rule-filters')).toBeVisible()
  const engText = await detail.textContent()
  expect(engText).not.toContain(plainText.slice(0, 40))

  // and the choice survives a reload — it is a preference, not a mode you
  // have to re-pick on every board
  await page.reload()
  await expect(page.getByTestId('view-advanced')).toHaveAttribute('aria-pressed', 'true')
})

test('guided emissions: pick the converter, get a verdict in words', async ({ page }) => {
  await loadBoard(page)
  const ids = await page.evaluate(() =>
    [...document.querySelectorAll('[data-testid^="finding-F-"]')]
      .map(e => e.dataset.testid.replace('finding-', '')))
  let opened = false
  for (const id of ids) {
    await page.getByTestId(`finding-${id}`).click()
    if (await page.getByTestId(`emit-${id}`).count()) {
      await page.getByTestId(`emit-${id}`).click()
      opened = true
      break
    }
  }
  expect(opened).toBe(true)
  await expect(page.getByTestId('emissions')).toBeVisible()

  // presets instead of four sliders, and the assumption behind the preset is
  // printed rather than hidden
  await expect(page.getByTestId('emissions-presets')).toBeVisible()
  await expect(page.getByTestId('emissions-current')).toHaveCount(0)
  await expect(page.getByTestId('preset-assumed')).toContainText('Assuming')
  await page.getByTestId('preset-gan').click()
  await expect(page.getByTestId('preset-assumed')).toContainText('5.0 ns')

  await expect(page.getByTestId('verdict-plain')).toBeVisible()
  await expect(page.getByTestId('caveat-plain')).toContainText('not a test report')

  // and the panel carries its own switch, because the header is behind the
  // scrim exactly when someone decides they want the numbers after all
  await page.getByTestId('panel-view-toggle').click()
  await expect(page.getByTestId('emissions-current')).toBeVisible()
  await expect(page.getByTestId('emissions-caveat')).toBeVisible()
})

test('the conducted answer names the mode, the frequency and the dB', async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('faraday.view', 'advanced'))
  await loadBoard(page)
  const ids = await page.evaluate(() =>
    [...document.querySelectorAll('[data-testid^="finding-F-"]')]
      .map(e => e.dataset.testid.replace('finding-', '')))
  for (const id of ids) {
    await page.getByTestId(`finding-${id}`).click()
    if (await page.getByTestId(`emit-${id}`).count()) {
      await page.getByTestId(`emit-${id}`).click()
      break
    }
  }
  await expect(page.getByTestId('emissions')).toBeVisible()

  const verdict = page.getByTestId('conducted-verdict')
  await expect(verdict).toBeVisible()
  // "N dB OVER the limit at F, and it is common-mode/differential-mode noise"
  await expect(verdict).toContainText(/\d+ dB (OVER|under) the limit/)
  await expect(verdict).toContainText(/(common|differential)-mode/)
  await expect(page.getByTestId('conducted-chart')).toBeVisible()
  await expect(page.getByTestId('conducted-required')).toContainText('CM stage needs')

  // the standard is a choice, and changing it moves the verdict
  const before = await verdict.textContent()
  await page.getByTestId('conducted-limit').selectOption('cispr32a-qp')
  await expect(verdict).not.toHaveText(before)
})

test('C_stray is derived from this board\'s switching copper, not invented',
     async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('faraday.view', 'advanced'))
  await loadBoard(page)
  const ids = await page.evaluate(() =>
    [...document.querySelectorAll('[data-testid^="finding-F-"]')]
      .map(e => e.dataset.testid.replace('finding-', '')))
  for (const id of ids) {
    await page.getByTestId(`finding-${id}`).click()
    if (await page.getByTestId(`emit-${id}`).count()) {
      await page.getByTestId(`emit-${id}`).click()
      break
    }
  }
  const derived = page.getByTestId('cstray-derived')
  await expect(derived).toBeVisible()
  await expect(derived).toContainText('derived')
  await expect(derived).toContainText('mm² of switching copper')

  // the plate is fixed by the layout; halving the distance doubles the
  // capacitance, which is the only lever the mounting has
  const pf = async () => {
    const t = await derived.textContent()
    return parseFloat(t.match(/([\d.]+) pF/)[1])
  }
  await page.getByTestId('bridge-gap').fill('20')
  const far = await pf()
  await page.getByTestId('bridge-gap').fill('10')
  const near = await pf()
  expect(near / far).toBeGreaterThan(1.8)
  expect(near / far).toBeLessThan(2.2)

  // bolting the board against the metalwork instead of spacing it off is a
  // 4.5x jump — the laminate's permittivity, not a fudge factor
  await page.getByTestId('bridge-mounting').selectOption('laminate')
  expect(await pf() / near).toBeGreaterThan(4.0)
})

test('a simulated run reads back into the panel, beside the seed it checks',
  async ({ page }) => {
    // ABT #809: the trapezoid is a seed; a SPICE run of this same board — its
    // measured parasitics, a real device model, a LISN — is the better-founded
    // source for the same two curves. The panel must show both and compare
    // them, never quietly replace one with the other.
    await page.addInitScript(() => localStorage.setItem('faraday.view', 'advanced'))
    await loadBoard(page)
    const ids = await page.evaluate(() =>
      [...document.querySelectorAll('[data-testid^="finding-F-"]')]
        .map(e => e.dataset.testid.replace('finding-', '')))
    for (const id of ids) {
      await page.getByTestId(`finding-${id}`).click()
      if (await page.getByTestId(`emit-${id}`).count()) {
        await page.getByTestId(`emit-${id}`).click()
        break
      }
    }
    await expect(page.getByTestId('emissions')).toBeVisible()
    await expect(page.getByTestId('sim-row')).toBeVisible()

    // the real thing: hertz/scripts/read_simulated.py's export of a
    // faraday_emi_sim run on this very board
    await page.getByTestId('sim-input').setInputFiles(
      new URL('./fixtures-simulated-run.json', import.meta.url).pathname)
    await expect(page.getByTestId('sim-note')).toContainText('Simulated')
    // and it says how the two independent paths compare at the seed's worst
    // point, in dB, rather than picking one
    await expect(page.getByTestId('sim-vs-seed')).toContainText('the seed reads')
    await expect(page.getByTestId('sim-vs-seed')).toContainText('dBµV')

    // a file that is not a run is refused with a reason, not silently ignored
    await page.getByTestId('sim-input').setInputFiles({
      name: 'nope.json', mimeType: 'application/json',
      buffer: Buffer.from('{"hello":true}'),
    })
    await expect(page.getByTestId('sim-error')).toContainText('not a simulated-run export')
  })
