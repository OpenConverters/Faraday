// The return-path layer, end to end: geometry only, and it must never dress
// itself in field units — that was the failure that killed its predecessor.
import { test, expect } from '@playwright/test'


// the pre-MPPT demo board, kept for the stitching-refusal path
const TINY_2LAYER = `(kicad_pcb (version 20221018) (generator pcbnew)
  (general (thickness 1.58))
  (paper "A4")
  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (36 "B.SilkS" user "B.Silkscreen")
    (44 "Edge.Cuts" user)
  )
  (setup
    (stackup
      (layer "F.SilkS" (type "Top Silk Screen"))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "core") (thickness 1.51) (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02))
      (layer "B.Cu" (type "copper") (thickness 0.035))
      (layer "B.SilkS" (type "Bottom Silk Screen"))
    )
    (pad_to_mask_clearance 0)
  )
  (net 0 "")
  (net 1 "GND")
  (net 2 "CLK")
  (net 3 "DATA")
  (gr_rect (start 0 0) (end 50 30) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))
  (segment (start 5 10) (end 45 10) (width 0.3) (layer "F.Cu") (net 2))
  (segment (start 5 10.5) (end 45 10.5) (width 0.3) (layer "F.Cu") (net 3))
  (segment (start 5 25) (end 20 25) (width 0.5) (layer "F.Cu") (net 1))
  (via (at 45 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
  (footprint "Resistor_SMD:R_0603_1608Metric" (layer "F.Cu")
    (at 10 20 90)
    (property "Reference" "R1" (at 0 -1.43 90) (layer "F.SilkS") (effects (font (size 1 1))))
    (property "Value" "100n" (at 0 1.43 90) (layer "F.Fab") (effects (font (size 1 1))))
    (pad "1" smd roundrect (at -0.7875 0 90) (size 0.9 0.95) (layers "F.Cu" "F.Paste" "F.Mask") (net 2 "CLK"))
    (pad "2" smd roundrect (at 0.7875 0 90) (size 0.9 0.95) (layers "F.Cu" "F.Paste" "F.Mask") (net 1 "GND"))
  )
  (zone (net 1) (net_name "GND") (layer "B.Cu") (hatch edge 0.5)
    (connect_pads (clearance 0.2))
    (min_thickness 0.25)
    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 0 0) (xy 50 0) (xy 50 30) (xy 0 30)))
    (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 24 0) (xy 24 30) (xy 0 30)))
    (filled_polygon (layer "B.Cu") (pts (xy 26 0) (xy 50 0) (xy 50 30) (xy 26 30)))
  )
)
`
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// This suite exercises the ADVANCED view — the one that puts every decibel on
// screen. A first visit now lands in GUIDED (plain language, presets, no
// vocabulary), which has its own spec, so every suite declares the view it
// means to test instead of inheriting whichever happens to be the default.
test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('faraday.view', 'advanced'))
})


const here = path.dirname(fileURLToPath(import.meta.url))

// Loading a 1.16 MB board means downloading a 656 kB WASM engine and parsing
// the layout. Locally both are cached; against a deployed site neither is.
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000
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

test('the return-path layer maps loop height and recolours the copper',
  async ({ page }) => {
    const errs = []
    page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })
    page.on('pageerror', e => errs.push(String(e)))

    await load(page)
    await expect(page.getByTestId('rp-bar')).toHaveCount(0)   // off by default

    await page.getByTestId('rp-toggle').click()
    await expect(page.getByTestId('rp-error')).toHaveCount(0)
    const bar = page.getByTestId('rp-bar')
    await expect(bar).toBeVisible()
    await expect(bar).toContainText('geometry only')
    await expect(bar).toContainText('effective loop height')

    // the copper is legibly recoloured: quiet teal present, hot a minority
    const px = await page.getByTestId('board-canvas').evaluate(cv => {
      const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data
      let quiet = 0, hot = 0
      for (let i = 0; i < d.length; i += 4) {
        const r = d[i], g = d[i + 1], b = d[i + 2]
        if (g > r + 25 && g > 110) quiet++
        else if (r > 215 && g > 200 && b > 190) hot++
      }
      return { quiet, hot }
    })
    expect(px.quiet, 'quiet copper must be drawn visibly').toBeGreaterThan(500)
    expect(px.hot).toBeLessThan(px.quiet)

    await page.getByTestId('rp-toggle').click()
    await expect(page.getByTestId('rp-bar')).toHaveCount(0)
    expect(errs).toEqual([])
  })

test('the layer never claims a field: no dB of any kind on a geometry map',
  async ({ page }) => {
    // Its predecessor reported a dBuV/m total whose 45.7 dB ranking spread
    // came from one invented binary. The replacement must not quote ANY field
    // unit — millimetres and mm² are what a layout proves.
    await load(page)
    await page.getByTestId('rp-toggle').click()
    const body = await page.getByTestId('rp-bar').textContent()
    expect(body).not.toMatch(/dB/i)
    expect(body).not.toMatch(/CISPR|EN ?55|FCC/i)
    expect(body).toMatch(/mm/)
  })

test('voids, layer changes and worst nets are reported as facts',
  async ({ page }) => {
    await load(page)
    await page.getByTestId('rp-toggle').click()
    const bar = page.getByTestId('rp-bar')
    // MPPT has both — measured natively in the unit suite, asserted here so
    // the JSON plumbing cannot silently drop them
    await expect(page.getByTestId('rp-void')).toContainText('plane void')
    await expect(bar).toContainText('layer change')
    // worst nets carry an area in mm², not a percentage of an invented total
    await expect(bar).toContainText('mm²')
  })

test('the two overlays say what they are each for', async ({ page }) => {
  await load(page)
  await page.getByTestId('rp-toggle').click()
  await expect(page.getByTestId('rp-bar')).toContainText('return current')
  await expect(page.getByTestId('rp-bar')).toContainText('emissions panel')

  await page.getByTestId('nf-toggle').click()
  await expect(page.getByTestId('nf-bar')).toContainText('What couples on the board')
  await expect(page.getByTestId('nf-bar')).toContainText('return-path layer')
})

test('stitching fix: a 4-layer board gets a new file, a 2-layer the honest reason',
  async ({ page }) => {
    // synthetic 4-layer with one unstitched layer change (mirrors the C++ test)
    const revB = `(kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 5 10) (end 30 10) (width 0.25) (layer "F.Cu") (net 1))
      (via (at 30 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (segment (start 30 10) (end 55 10) (width 0.25) (layer "B.Cu") (net 1))
      (via (at 90 40) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (zone (net 2) (net_name "GND") (layer "In1.Cu")
        (polygon (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45)))
        (filled_polygon (layer "In1.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45))))
      (zone (net 2) (net_name "GND") (layer "In2.Cu")
        (polygon (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45)))
        (filled_polygon (layer "In2.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45))))
    )`
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles(
      { name: 'four.kicad_pcb', mimeType: 'text/plain', buffer: Buffer.from(revB) })
    const card = page.getByTestId('stackup-card')
    await expect(card.or(page.getByTestId('finding-F-0001')).first())
      .toBeVisible({ timeout: LOAD_MS })
    if (await card.count()) await card.getByText('Default 4-layer').click()
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
    await page.getByTestId('rp-toggle').click()
    await expect(page.getByTestId('rp-bar')).toBeVisible({ timeout: LOAD_MS })

    const download = page.waitForEvent('download')
    await page.getByTestId('rp-fix').click()
    expect((await download).suggestedFilename()).toBe('four-stitched.kicad_pcb')
    await expect(page.getByTestId('rp-fix-result'))
      .toContainText('1 stitching via(s) added')
    await expect(page.getByTestId('rp-fix-result')).toContainText('original is untouched')

    // a 2-layer board, unstitched but single-plane — the honest refusal.
    // (This WAS the served demo; the demo is now the LibreSolar MPPT, so the
    // tiny board lives inline to keep exercising the refusal path.)
    const tiny = TINY_2LAYER
    await page.getByTestId('file-input').setInputFiles(
      { name: 'tiny.kicad_pcb', mimeType: 'text/plain', buffer: Buffer.from(tiny) })
    await expect(page.getByTestId('finding-F-0001'))
      .toContainText('CLK', { timeout: LOAD_MS })
    await page.getByTestId('rp-toggle').click()
    await expect(page.getByTestId('rp-bar')).toBeVisible({ timeout: LOAD_MS })
    await page.getByTestId('rp-fix').click()
    // this board's actual blocker: not one reference via exists to copy a
    // style from — the generator refuses to invent pad/drill sizes
    await expect(page.getByTestId('rp-fix-result'))
      .toContainText('refusing to invent')
  })
