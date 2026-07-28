// The bench, end to end in a real browser: the deep tier runs IN THE PAGE, so
// these assertions cover the whole chain — WASM field solve, transient, field
// map transport, canvas render, and the slider re-solve.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const FIXTURE = path.join(here, '../../../cpp/tests/fixtures/fixture_2layer.kicad_pcb')
const REAL = path.join(here, '../../../cpp/tests/fixtures/real/hackrf-one.kicad_pcb')

async function openBench(page, file = FIXTURE) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(file)
  await page.getByTestId('finding-F-0001').click()
  await page.getByTestId('bench-F-0001').click()
  await expect(page.getByTestId('bench')).toBeVisible()
  await expect(page.getByTestId('bench-verdict')).toBeVisible()
}

test('a finding solves to a real cross-section, waveform and verdict', async ({ page }) => {
  const consoleErrors = []
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()) })
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await openBench(page)
  await expect(page.getByTestId('bench-error')).toHaveCount(0)

  // the verdict is stated in millivolts against a named receiver, not in dB
  const verdict = page.getByTestId('bench-verdict')
  await expect(verdict).toContainText('mV')
  await expect(verdict).toContainText('LVCMOS 3.3 V')
  await expect(verdict).toContainText('% of budget')

  // extracted line parameters are physical
  const rlgc = page.getByTestId('bench-rlgc')
  const z0 = Number((await rlgc.textContent()).match(/([\d.]+) Ω/)[1])
  expect(z0).toBeGreaterThan(20)
  expect(z0).toBeLessThan(200)

  // both canvases actually have pixels drawn on them
  for (const id of ['bench-field', 'bench-wave']) {
    const painted = await page.getByTestId(id).evaluate(cv => {
      const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
      const seen = new Set()
      for (let i = 0; i < d.length; i += 4) seen.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
      return seen.size
    })
    expect(painted, `${id} should render more than a flat fill`).toBeGreaterThan(12)
  }

  expect(consoleErrors).toEqual([])
})

test('the solve is fast enough to sit behind a slider', async ({ page }) => {
  await openBench(page)
  // measure the engine directly: this is the call the slider makes on every
  // input event, and the feature is only honest if it really is interactive
  const ms = await page.evaluate(async () => {
    const eng = await window.createFaraday()
    const req = JSON.stringify({
      mode: 'microstrip', w1Mm: 0.25, w2Mm: 0.25, gapMm: 0.2, hMm: 0.2,
      tMm: 0.035, epsR: 4.3, lengthMm: 60, riseNs: 0.5, field: false, fix: false,
    })
    eng.solvePair(req)
    const t0 = performance.now()
    for (let i = 0; i < 10; i++) eng.solvePair(req)
    return (performance.now() - t0) / 10
  })
  expect(ms, `solvePair took ${ms.toFixed(1)} ms in the browser`).toBeLessThan(60)
})

test('widening the separation lowers the noise, and the fix keeps its promise',
  async ({ page }) => {
    await openBench(page)
    // Return null rather than throwing when the panel is mid-render:
    // expect.poll retries a value, but a helper that dereferences a null match
    // takes the whole test down on the first unlucky frame. Cost one flake on
    // a cold-deployed site, where every solve arrives a little later.
    const peak = async () => {
      const t = await page.getByTestId('bench-verdict').textContent()
      const m = t && t.match(/([\d.]+)\s*mV/)
      return m ? Number(m[1]) : null
    }
    const before = await peak()

    const slider = page.getByTestId('bench-gap')
    await slider.fill('1.4')
    await slider.dispatchEvent('input')
    await expect.poll(peak).toBeLessThan(before)

    // and the offered fix has to survive being taken
    await slider.fill('0.08')
    await slider.dispatchEvent('input')
    const fix = page.getByTestId('bench-fix')
    if (await fix.count()) {
      const ft = await fix.textContent()
      const fm = ft && ft.match(/→\s*(\d+)%/)
      expect(fm, 'the fix button must state the percentage it promises').not.toBeNull()
      const promised = Number(fm[1])
      await fix.click()
      await expect.poll(async () => {
        const t = await page.getByTestId('bench-verdict').textContent()
        const m = t && t.match(/([\d.]+)% of budget/)
        return m ? Number(m[1]) : null
      }).toBeLessThanOrEqual(promised + 2)
    }
  })

test('a faster edge and a tighter receiver both cost budget', async ({ page }) => {
  await openBench(page)
  const pct = async () => {
    const t = await page.getByTestId('bench-verdict').textContent()
    return Number(t.match(/([\d.]+)% of budget/)[1])
  }
  await page.getByTestId('bench-gap').fill('0.15')
  await page.getByTestId('bench-gap').dispatchEvent('input')
  await page.waitForTimeout(300)
  const base = await pct()

  // same coupling, less margin at the receiver
  await page.getByTestId('bench-family').selectOption('lvcmos12')
  await expect.poll(pct).toBeGreaterThan(base)
})

test('the field view switches between |E| and potential', async ({ page }) => {
  await openBench(page)
  const snap = () => page.getByTestId('bench-field').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    let sum = 0
    for (let i = 0; i < d.length; i += 4) sum += d[i] + d[i + 1] + d[i + 2]
    return sum
  })
  const e = await snap()
  await page.getByRole('button', { name: 'potential' }).click()
  await expect.poll(snap).not.toBe(e)
})

test('a real board opens the bench on a real coupled pair', async ({ page }) => {
  test.setTimeout(60000)
  const consoleErrors = []
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()) })
  page.on('pageerror', e => consoleErrors.push(String(e)))

  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(REAL)
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: 30000 })

  // HackRF's highest-ranked findings are return-path breaks, which have no
  // cross-section to solve — the bench is offered only where there is one, so
  // walk down to the first coupled run rather than assuming it leads.
  const ids = await page.evaluate(() =>
    [...document.querySelectorAll('[data-testid^="finding-F-"]')]
      .map(e => e.dataset.testid.replace('finding-', '')))
  let opened = ''
  for (const id of ids) {
    await page.getByTestId(`finding-${id}`).click()
    if (await page.getByTestId(`bench-${id}`).count()) {
      await page.getByTestId(`bench-${id}`).click()
      opened = id
      break
    }
    await page.getByTestId(`finding-${id}`).click()   // collapse and move on
  }
  expect(opened, 'no finding on this board offered a cross-section').not.toBe('')

  await expect(page.getByTestId('bench-verdict')).toContainText('mV')
  await expect(page.getByTestId('bench-error')).toHaveCount(0)
  // the run must be cheap enough that nobody waits for it
  const cost = await page.getByTestId('bench-cost').textContent()
  expect(Number(cost.match(/([\d.]+) ms/)[1])).toBeLessThan(500)
  expect(consoleErrors).toEqual([])
})
