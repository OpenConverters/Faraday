// The whole-board radiation attribution, end to end on a real converter.
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')

async function load(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: 30000 })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: 30000 })
}

test('the radiation layer attributes the board and recolours the copper',
  async ({ page }) => {
    const errs = []
    page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })
    page.on('pageerror', e => errs.push(String(e)))

    await load(page)
    // off by default: it recolours the board, so it must be asked for
    await expect(page.getByTestId('rad-bar')).toHaveCount(0)

    const before = await page.getByTestId('board-canvas').evaluate(cv => {
      const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
      const s = new Set()
      for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
      return s.size
    })

    await page.getByTestId('rad-toggle').click()
    await expect(page.getByTestId('rad-error')).toHaveCount(0)
    const bar = page.getByTestId('rad-bar')
    await expect(bar).toBeVisible()
    await expect(bar).toContainText('dBµV/m total')
    // the caveat is not optional on a picture this easy to mistake for a solve
    await expect(bar).toContainText('not a chamber')

    // the copper actually changed colour
    const after = await page.getByTestId('board-canvas').evaluate(cv => {
      const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
      const s = new Set()
      for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i + 1]},${d[i + 2]}`)
      return s.size
    })
    expect(after).not.toBe(before)

    // and it toggles back off
    await page.getByTestId('rad-toggle').click()
    await expect(page.getByTestId('rad-bar')).toHaveCount(0)

    expect(errs).toEqual([])
  })

test('the attribution names the copper responsible', async ({ page }) => {
  await load(page)
  await page.getByTestId('rad-toggle').click()
  const bar = page.getByTestId('rad-bar')
  await expect(bar).toBeVisible()
  // top contributors are listed with a share, and the shares are real
  const shares = (await bar.textContent()).match(/(\d+\.\d)%/g)
  expect(shares).not.toBeNull()
  expect(shares.length).toBeGreaterThan(0)
  for (const s of shares) {
    const v = Number(s.replace('%', ''))
    expect(v).toBeGreaterThanOrEqual(0)
    expect(v).toBeLessThanOrEqual(100)
  }
})

test('the map is fast enough to recompute over a whole board', async ({ page }) => {
  // The engine keeps the imported board so this does not re-parse 4 MB of
  // s-expressions on every call. If that regresses, this is what notices.
  await load(page)
  const t0 = Date.now()
  await page.getByTestId('rad-toggle').click()
  await expect(page.getByTestId('rad-bar')).toBeVisible()
  const elapsed = Date.now() - t0
  expect(elapsed, `radiation map took ${elapsed} ms over the whole board`)
    .toBeLessThan(2000)
})
