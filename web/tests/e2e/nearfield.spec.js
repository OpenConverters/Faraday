// The component near-field map, end to end. This is the panel most at risk of
// being mistaken for a compliance tool, so the caveats are asserted as hard as
// the numbers.
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')

async function openNearField(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: 30000 })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: 30000 })
  await page.getByTestId('nf-toggle').click()
  await expect(page.getByTestId('nearfield')).toBeVisible()
}

test('a converter board maps its component near field', async ({ page }) => {
  const errs = []
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })
  page.on('pageerror', e => errs.push(String(e)))

  await openNearField(page)
  await expect(page.getByTestId('nf-error')).toHaveCount(0)

  // the regime context must be on screen: the whole board is inside lambda/2pi,
  // which is the reason this is not the radiation map
  await expect(page.getByTestId('nf-context')).toContainText('λ/2π')

  // the field picture is actually drawn
  const colours = await page.getByTestId('nf-canvas').evaluate(cv => {
    const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
    const s = new Set()
    for (let i = 0; i < d.length; i += 4) s.add(`${d[i]},${d[i+1]},${d[i+2]}`)
    return s.size
  })
  expect(colours).toBeGreaterThan(12)

  expect(errs).toEqual([])
})

test('the panel refuses to look like a compliance verdict', async ({ page }) => {
  // The single most important assertion in this file. A near-field map that
  // implies chamber compliance is worse than no map.
  await openNearField(page)
  const c = page.getByTestId('nf-caveat')
  await expect(c).toBeVisible()
  await expect(c).toContainText('not a radiation map')
  await expect(c).toContainText('no dBµV/m')
  await expect(c).toContainText('your assumption')
  await expect(c).toContainText('Cable common-mode')

  // No standard may be NAMED and no field-strength unit quoted. Matching bare
  // words like "limit line" is wrong — the caveat says "no limit line", and an
  // assertion that cannot tell a denial from a claim is worse than none.
  const body = await page.getByTestId('nearfield').textContent()
  expect(body, 'no standard may be named in a near-field panel')
    .not.toMatch(/CISPR|EN ?55|FCC|Class [AB]\b/i)
  expect(body, 'dBuV/m is a far-field unit and has no meaning here')
    .not.toMatch(/dB[µu]V\/m(?!\s+here)/i)
  // the units it DOES use are the near-field ones
  expect(body).toMatch(/dB[µu]A\/m|A\/m/)
})

test('raising the probe lowers the field, steeply', async ({ page }) => {
  await openNearField(page)
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
  await openNearField(page)
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
  await openNearField(page)
  const rows = await page.getByTestId('nf-victims').evaluate(t =>
    [...t.querySelectorAll('tbody tr')].map(r =>
      [...r.querySelectorAll('td')].map(d => d.textContent.trim())))
  expect(rows.length).toBeGreaterThan(0)
  for (const r of rows) {
    expect(r[3], 'every victim must get a field').toMatch(/dB[µu]A\/m/)
    expect(r[4], 'every victim must get an induced voltage').toMatch(/[µm]V/)
  }
  // and the dipole caveat is context, not a refusal
  const body = await page.getByTestId('nearfield').textContent()
  if (/\*/.test(body))
    await expect(page.getByTestId('nf-caveat')).toContainText('still get a real number')
})
