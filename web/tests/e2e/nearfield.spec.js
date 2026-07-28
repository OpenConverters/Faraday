// The component near-field map, end to end. This is the panel most at risk of
// being mistaken for a compliance tool, so the caveats are asserted as hard as
// the numbers.
import { test, expect } from '@playwright/test'
import path from 'node:path'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

// Loading a 1.16 MB board means downloading a 656 kB WASM engine and parsing
// the layout. Locally both are cached; against a deployed site neither is, and
// under a serialized suite that legitimately exceeds 30 s. One constant so the
// budget cannot drift between specs.
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')

async function openNearField(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
  await page.getByTestId('nf-toggle').click()
  await expect(page.getByTestId('nf-bar')).toBeVisible()
}

async function openDetail(page) {
  await openNearField(page)
  await page.getByTestId('nf-detail').click()
  await expect(page.getByTestId('nf-panel')).toBeVisible()
}

test('a converter board maps its component near field', async ({ page }) => {
  const errs = []
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })
  page.on('pageerror', e => errs.push(String(e)))

  // off by default, then rendered ON THE BOARD like the radiation layer
  await page.goto('/')
  await expect(page.getByTestId('nf-bar')).toHaveCount(0)

  await openNearField(page)
  await expect(page.getByTestId('nf-error')).toHaveCount(0)
  await expect(page.getByTestId('nf-bar')).toContainText('λ/2π')

  // the field is washed onto the board canvas itself
  const colours = await page.getByTestId('board-canvas').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    const s = new Set()
    for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i+1]},${d[i+2]}`)
    return s.size
  })
  expect(colours).toBeGreaterThan(12)

  // and it toggles back off
  await page.getByTestId('nf-toggle').click()
  await expect(page.getByTestId('nf-bar')).toHaveCount(0)

  expect(errs).toEqual([])
})

test('the panel refuses to look like a compliance verdict', async ({ page }) => {
  // The single most important assertion in this file. A near-field map that
  // implies chamber compliance is worse than no map.
  await openNearField(page)
  const c = page.getByTestId('nf-bar')
  await expect(c).toContainText('not a radiation map')
  await expect(c).toContainText('no limit line')
  await expect(c).toContainText('your assumption')

  // No standard may be NAMED and no field-strength unit quoted. Matching bare
  // words like "limit line" is wrong — the caveat says "no limit line", and an
  // assertion that cannot tell a denial from a claim is worse than none.
  const body = await page.getByTestId('nf-bar').textContent()
  expect(body, 'no standard may be named in a near-field panel')
    .not.toMatch(/CISPR|EN ?55|FCC|Class [AB]\b/i)
  // A dBuV/m VALUE is forbidden; the bar's own denial of it ("No dBµV/m") is
  // the opposite of a claim, so match a number in front of the unit rather
  // than the bare words.
  expect(body, 'no dBuV/m value may be quoted in a near-field panel')
    .not.toMatch(/[\d.]+\s*dB[µu]V\/m/i)
  // the units it DOES use are the near-field ones
  expect(body).toMatch(/dB[µu]A\/m|A\/m/)
})

test('raising the probe lowers the field, steeply', async ({ page }) => {
  await openDetail(page)
  // Read the CELL, not the table's textContent: cells concatenate without a
  // separator, so "36.5" + "126 dBµA/m" reads as "5126" to a naive regex.
  const worstH = () => page.getByTestId('nf-victims').evaluate(t => {
    const c = t.querySelector('tbody tr td:nth-child(4)')
    if (!c) return null
    const m = c.textContent.match(/(-?[\d.]+)/)
    return m ? Number(m[1]) : null
  })
  const before = await worstH()
  test.skip(before === null, 'this board reports no in-range victim')

  // Height enters the exact integral through the 3-D distance. The slope is
  // between 1/r and 1/r^3 depending on how far inside the loop the point is,
  // so what is asserted is the direction and that it is STEEP — not a fixed
  // 18 dB, which would only hold for a point dipole.
  const h = page.getByTestId('nf-height')
  await h.fill('12')
  await h.dispatchEvent('input')
  await expect.poll(worstH).toBeLessThan(before - 4)
})

test('current scales the map and is presented as an assumption', async ({ page }) => {
  await openDetail(page)
  const level = () => page.getByTestId('nf-victims').evaluate(t => {
    const c = t.querySelector('tbody tr td:nth-child(4)')
    if (!c) return null
    const m = c.textContent.match(/(-?[\d.]+)/)
    return m ? Number(m[1]) : null
  })
  const before = await level()
  test.skip(before === null, 'no in-range victim on this board')

  // The slider is the RING current — the HF amplitude at the resonance, not
  // the switched DC current. Doubling it is +6 dB exactly, because the field
  // is linear in it.
  const cur = page.getByTestId('nf-current')
  const now = Number(await cur.inputValue())
  await cur.fill(String(now * 2))
  await cur.dispatchEvent('input')
  await expect.poll(level).toBeCloseTo(before + 6, 0)
})

test('close-in parts get a real number from the exact integral', async ({ page }) => {
  // The point dipole would have to blank everything within ~46 mm of a
  // 267 mm2 loop — most of a 100 mm board, and every part beside the switcher.
  // Biot-Savart over the loop polygon answers at any distance, so EVERY row
  // must carry a field.
  await openDetail(page)
  const rows = await page.getByTestId('nf-victims').evaluate(t =>
    [...t.querySelectorAll('tbody tr')].map(r =>
      [...r.querySelectorAll('td')].map(d => d.textContent.trim())))
  expect(rows.length).toBeGreaterThan(0)
  for (const r of rows) {
    expect(r[3], 'every victim must get a field').toMatch(/dB[µu]A\/m/)
    // column 4 is cos(theta) now; induced moved to 5
    expect(Number(r[4].replace('*', ''))).toBeGreaterThanOrEqual(0)
    expect(r[5], 'every victim must get an induced voltage').toMatch(/[µm]V/)
  }
  // and the dipole caveat is context, not a refusal
  const body = await page.getByTestId('nf-panel').textContent()
  expect(body).toMatch(/dB[µu]A\/m/)
})

test('a shield can is offered as a conditional, with the binding regime named',
  async ({ page }) => {
    await openDetail(page)
    const sh = page.getByTestId('nf-shield')
    await expect(sh).toBeVisible()

    // At the default 130 MHz ring the wall is opaque, so the SEAM must bind —
    // that is the whole point: above a few MHz the alloy on the datasheet is
    // not the variable that matters.
    await expect(page.getByTestId('nf-limited')).toHaveText('seam')

    // and widening the contact pitch must cost shielding effectiveness
    const se = () => sh.evaluate(el => {
      const m = el.textContent.match(/delivered\s*(-?[\d.]+) dB/)
      return m ? Number(m[1]) : null
    })
    const before = await se()
    expect(before).not.toBeNull()
    const pitch = page.getByTestId('nf-shield-seam')
    await pitch.fill('20')
    await pitch.dispatchEvent('input')
    await expect.poll(se).toBeLessThan(before)

    // the honest limits are stated, not buried
    await expect(sh).toContainText('contact pitch')
    await expect(sh).toContainText('nothing')
    await expect(sh).toContainText('common-mode')
  })

test('a drawn can attenuates the victims it separates from the aggressor',
  async ({ page }) => {
    await openNearField(page)
    const worst = () => page.getByTestId('nf-bar').evaluate(el => {
      const m = el.textContent.match(/worst: (\S+)/)
      return m ? m[1] : null
    })
    const before = await page.getByTestId('nf-bar').textContent()

    // Drag starts at 15% in — the layer-chip row overlays the canvas's very
    // top-left, and a drag that begins on a chip never reaches the canvas.
    await page.getByTestId('nf-shield-draw').click()
    const box = await page.getByTestId('board-canvas').boundingBox()
    await page.mouse.move(box.x + box.width * 0.15, box.y + box.height * 0.2)
    await page.mouse.down()
    await page.mouse.move(box.x + box.width * 0.55, box.y + box.height * 0.65,
                          { steps: 8 })
    await page.mouse.up()

    // the can exists and the map was re-run
    await expect(page.getByTestId('nf-shield-clear')).toBeVisible()
    // if it separated a pair, the bar must carry the honesty markers
    const bar = await page.getByTestId('nf-bar').textContent()
    if (/can separates/.test(bar)) {
      expect(bar).toContain('upper bound')
      expect(bar).toContain('five-sided')
    }

    // clearing restores the unshielded state
    await page.getByTestId('nf-shield-clear').click()
    await expect(page.getByTestId('nf-shield-clear')).toHaveCount(0)
    expect(await page.getByTestId('nf-bar').textContent()).toBe(before)
    expect(await worst()).not.toBeNull()
  })

test('a drawn can dims the FIELD MAP outside it — colours agree with numbers',
  async ({ page }) => {
    await openNearField(page)
    const canvas = page.getByTestId('board-canvas')

    // the engine names the aggressor hulls; the canvas names its transform —
    // no guessed screen fractions anywhere
    const boardText = fs.readFileSync(MPPT, 'utf8')
    const hulls = await page.evaluate(async (board) => {
      const eng = await window.createFaraday()
      JSON.parse(eng.analyze(board, 'default-4layer'))
      const r = JSON.parse(eng.nearField(JSON.stringify(
        { probeHeightMm: 2, ringCurrentA: 2 })))
      return r.aggressors.filter(a => a.hull && a.hull.length >= 3)
        .map(a => a.hull)
    }, boardText)
    expect(hulls.length).toBeGreaterThan(1)

    // wrap the SECOND aggressor's whole hull in the can, and probe 3.5 mm
    // beyond its rightmost edge — off the white hull outline, outside the
    // can (so no grey tint), where that aggressor's field dominates
    const xs = hulls[1].map(p => p[0]), ys = hulls[1].map(p => p[1])
    const can = { x1: Math.min(...xs) - 1, y1: Math.min(...ys) - 1,
                  x2: Math.max(...xs) + 1, y2: Math.max(...ys) + 1 }
    const probeMm = [can.x2 + 3.5, (can.y1 + can.y2) / 2]
    // the point must owe its light to the encircled aggressor: every vertex
    // of the OTHER hull stays at least twice as far away
    const d2 = pts => Math.min(...pts.map(p => Math.hypot(p[0] - probeMm[0],
                                                          p[1] - probeMm[1])))
    expect(d2(hulls[0])).toBeGreaterThan(2 * d2(hulls[1]))

    const view = JSON.parse(await canvas.getAttribute('data-view'))
    const toScreen = ([x, y]) =>
      [(x - view.ox) * view.scale, (y - view.oy) * view.scale]
    const probe = () => canvas.evaluate((el, [x, y]) => {
      const dpr = window.devicePixelRatio || 1
      const d = el.getContext('2d')
        .getImageData(x * dpr - 4, y * dpr - 4, 8, 8).data
      let s = 0
      for (let i = 0; i < d.length; i += 4) s += d[i] + d[i + 1] + d[i + 2]
      return s / (d.length / 4)
    }, toScreen(probeMm))
    await expect.poll(probe, { timeout: 15000 }).toBeGreaterThan(40)
    const before = await probe()

    await page.getByTestId('nf-shield-draw').click()
    const box = await canvas.boundingBox()
    const [cx1, cy1] = toScreen([can.x1, can.y1])
    const [cx2, cy2] = toScreen([can.x2, can.y2])
    await page.mouse.move(box.x + cx1, box.y + cy1)
    await page.mouse.down()
    await page.mouse.move(box.x + cx2, box.y + cy2, { steps: 8 })
    await page.mouse.up()
    await expect(page.getByTestId('nf-shield-clear')).toBeVisible()

    // The drop must be the FIELD attenuation, not incidental repainting.
    // 15%: the probe still catches the OTHER aggressor's unshielded field
    // (its nearest edge is ~1.3x the distance, and 1/1.3^3 of the light
    // legitimately stays), while tint/antialias noise measures under 5%.
    await expect.poll(probe, { timeout: 15000 }).toBeLessThan(before * 0.85)
  })
