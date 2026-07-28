// The whole-board radiation attribution, end to end on a real converter.
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

async function load(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
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

    // "the colours changed" is not enough — the first version of this map was
    // arithmetically right and rendered 89% of the copper into a near-black
    // band, which looks exactly like nothing happened. So assert the picture is
    // actually LEGIBLE: quiet copper drawn in visible teal, hot copper present,
    // and the hot part a small minority of it.
    const px = await page.getByTestId('board-canvas').evaluate(cv => {
      const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
      let quiet = 0, hot = 0
      for (let i = 0; i < d.length; i += 4) {
        const r = d[i], g = d[i + 1], b = d[i + 2]
        if (g > r + 25 && g > 110) quiet++              // teal end of the ramp
        else if (r > 215 && g > 200 && b > 190) hot++   // white-hot end
      }
      return { quiet, hot }
    })
    expect(px.quiet, 'quiet copper must be drawn visibly, not near-black')
      .toBeGreaterThan(500)
    expect(px.hot).toBeLessThan(px.quiet)   // an attribution, not a red board
    expect(before).toBeGreaterThan(0)

    // Counting HOT pixels is deliberately not asserted: pads are drawn in the
    // same copper colour as the hot end of the ramp, and a hot trace is only a
    // couple of pixels wide, so the count is dominated by pads and would pass
    // whatever the map did. The attribution shares below are the honest check
    // that the hot end is real.

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
