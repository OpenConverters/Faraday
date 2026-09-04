// The parts layer and the part inspector: every component drawn as a body
// over its pads; a click opens the board's facts about it, then Kelvin's —
// the catalogue record, the datasheet link, the cross-references.
//
// The catalogue half needs the Kelvin data set. The dev server proxies
// /kelvin.js and /kelvin/* from kelvin.openconverters.com (prod serves them
// same-origin off the shared /cache), so these tests reach the REAL
// catalogue over the network and download a family's shard the first time
// a browser context opens it. That is the product, not a stub, and when the
// catalogue is unreachable the tests fail and say so — the inspector must
// never pretend to have looked something up.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const PARTS = path.join(here, 'fixtures/parts.kicad_pcb')
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000
// a shard is a real download (the capacitor family is ~34 MB)
const CATALOGUE_MS = 180000

async function loadFixture(page, file) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(file)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('board-canvas')).first()).toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('board-canvas')).toBeVisible({ timeout: LOAD_MS })
}

// click the board at a WORLD coordinate, through the canvas's own transform
async function clickWorld(page, x, y) {
  const canvas = page.getByTestId('board-canvas')
  const view = JSON.parse(await canvas.getAttribute('data-view'))
  const box = await canvas.boundingBox()
  await page.mouse.click(box.x + (x - view.ox) * view.scale, box.y + (y - view.oy) * view.scale)
}

test('every component is drawn as a body, and the chip turns the layer off', async ({ page }) => {
  await loadFixture(page, MPPT)
  const canvas = page.getByTestId('board-canvas')
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBeGreaterThan(100)
  await page.getByTestId('parts-toggle').click()
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBe(0)
  await page.getByTestId('parts-toggle').click()
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBeGreaterThan(100)
})

test('a mounting hole is not a part; a capacitor is, with its pins and nets', async ({ page }) => {
  await loadFixture(page, PARTS)
  const canvas = page.getByTestId('board-canvas')
  // C1, Q1, R1, Q2 — and not H1, which has no pads
  await expect.poll(async () => Number(await canvas.getAttribute('data-parts'))).toBe(4)

  await clickWorld(page, 10, 20)
  const panel = page.getByTestId('part-panel')
  await expect(panel).toBeVisible()
  await expect(panel.getByRole('heading', { name: 'C1' })).toBeVisible()
  await expect(page.getByTestId('part-value')).toHaveText('100n')
  const pins = page.getByTestId('part-pins')
  await expect(pins).toContainText('CLK')
  await expect(pins).toContainText('GND')
  await expect(pins).toContainText('F.Cu')
})

test('no part number: the value and package are matched against the catalogue', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  const errors = []
  page.on('pageerror', e => errors.push(String(e)))
  await loadFixture(page, PARTS)
  await clickWorld(page, 10, 20)
  await expect(page.getByTestId('part-panel')).toBeVisible()
  await expect(page.getByTestId('part-error')).toHaveCount(0)

  const byValue = page.getByTestId('part-by-value')
  await expect(byValue).toBeVisible({ timeout: CATALOGUE_MS })
  await expect(byValue).toContainText('catalogue part(s) match')
  await expect(byValue).toContainText('100 nF')
  await expect(byValue).toContainText('0603')
  // a real match list, and picking one makes it the original for the record
  const first = byValue.locator('button.lnk').first()
  await expect(first).toBeVisible()
  await first.click()
  await expect(page.getByTestId('part-record')).toBeVisible({ timeout: CATALOGUE_MS })
  await expect(page.getByTestId('part-ident')).toContainText('chosen from the matches below')
  expect(errors).toEqual([])
})

test('a catalogued part number: exact match, datasheet, cross-references', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  const errors = []
  page.on('pageerror', e => errors.push(String(e)))
  await loadFixture(page, PARTS)
  await clickWorld(page, 25, 20)
  const panel = page.getByTestId('part-panel')
  await expect(panel).toBeVisible()
  await expect(panel.getByRole('heading', { name: 'Q1' })).toBeVisible()

  const ident = page.getByTestId('part-ident')
  await expect(ident).toContainText('EPC2019', { timeout: CATALOGUE_MS })
  await expect(ident).toContainText('exact part-number match')
  await expect(page.getByTestId('part-error')).toHaveCount(0)

  // the record, with the vendor's own datasheet link
  const ds = page.getByTestId('part-datasheet')
  await expect(ds).toBeVisible({ timeout: CATALOGUE_MS })
  expect(await ds.getAttribute('href')).toMatch(/^https?:\/\//)
  // the board's footprint reads as SOT-23 and the catalogue says BGA — said, not hidden
  await expect(page.getByTestId('part-record')).toContainText('case')

  // cross-references: Kelvin's ranker over every other manufacturer
  const table = page.getByTestId('part-xref-table')
  await expect(table).toBeVisible({ timeout: CATALOGUE_MS })
  expect(await table.locator('tbody tr:not(.notes)').count()).toBeGreaterThan(0)
  await expect(page.getByTestId('part-xref-error')).toHaveCount(0)
  expect(errors).toEqual([])
})

test('an unknown part number says which catalogues were searched, and offers the rest',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    await loadFixture(page, PARTS)
    await clickWorld(page, 25, 6)
    const ident = page.getByTestId('part-ident')
    await expect(ident).toContainText('is not in the', { timeout: CATALOGUE_MS })
    await expect(ident).toContainText('XYZ9999ABC')
    await expect(ident).toContainText('mosfet')
    await expect(page.getByTestId('part-search-more')).toBeVisible()
    await expect(page.getByTestId('part-search-more')).toContainText('families')
  })


// ---------------------------------------------------------------------------
// The end of the catalogue: asking Heaviside's librarian to source the part.
//
// The route is STUBBED here on purpose. Heaviside's own suite
// (tests/unit/test_librarian_lookup.py) pins the server's behaviour against a
// fake distributor; what Faraday owns is the contract at the seam — that a
// hit renders as a record, that a miss is reported as a miss, and above all
// that "the distributor could not be asked" never reads as "the part does not
// exist". Pointing this at the real endpoint would spend a distributor API
// call per run and make the suite fail when someone else's token expires.
// ---------------------------------------------------------------------------

const SOURCED_MOSFET = {
  mpn: 'XYZ9999ABC',
  found: true,
  category: 'mosfets',
  source: 'digikey',
  stored: '/srv/staging/mosfets/digikey-XYZ9999ABC.json',
  storedReason: 'staged for review — a librarian applies it into TAS, and the next index build puts it in the catalogue',
  component: {
    semiconductor: {
      mosfet: {
        manufacturerInfo: {
          name: 'Example Semiconductor',
          reference: 'XYZ9999ABC',
          status: 'production',
          datasheetUrl: 'https://example.invalid/xyz9999abc.pdf',
          datasheetInfo: {
            part: { partNumber: 'XYZ9999ABC', technology: 'MOSFET', case: 'SOT-23' },
            electrical: { drainSourceVoltage: 100, continuousDrainCurrent: 5.4, onResistance: 0.042 },
            thermal: { junctionTemperatureMax: 150 },
            provenance: [{ source: 'distributor', sourceName: 'DigiKey', retrievedDate: '2026-09-04' }],
          },
        },
      },
    },
  },
}

async function stubLibrarian(page, handler) {
  await page.route('**/heaviside/librarian/lookup', handler)
}

test('an unknown part can be sourced through the librarian, and renders as a record',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    const errors = []
    page.on('pageerror', e => errors.push(String(e)))
    let sentBody = null
    await stubLibrarian(page, async route => {
      sentBody = JSON.parse(route.request().postData())
      await route.fulfill({ status: 200, contentType: 'application/json',
                            body: JSON.stringify(SOURCED_MOSFET) })
    })
    await loadFixture(page, PARTS)
    await clickWorld(page, 25, 6)          // Q2, the MPN no catalogue has
    await expect(page.getByTestId('part-ident')).toContainText('is not in the', { timeout: CATALOGUE_MS })

    const btn = page.getByTestId('part-source-btn')
    await expect(btn).toBeVisible()
    await expect(btn).toContainText('XYZ9999ABC')
    await btn.click()

    // the part number went, and the board's guess travelled only as a hint
    await expect.poll(() => sentBody).not.toBeNull()
    expect(sentBody.mpn).toBe('XYZ9999ABC')
    expect(sentBody.category).toBe('mosfet')

    // the answer says what happened and where it was parked
    const said = page.getByTestId('part-sourced')
    await expect(said).toBeVisible()
    await expect(said).toContainText('mosfets')
    await expect(said).toContainText('staged for review')
    // and it is honest that this is not yet a catalogue part
    await expect(said).toContainText('not in the catalogue yet')

    // it renders with the record code: the vendor's datasheet and the specs
    const rec = page.getByTestId('part-record')
    await expect(rec).toBeVisible()
    expect(await page.getByTestId('part-datasheet').getAttribute('href'))
      .toBe('https://example.invalid/xyz9999abc.pdf')
    await expect(rec).toContainText('drainSourceVoltage')
    expect(errors).toEqual([])
  })

test('a part the distributor really does not have is reported as a miss', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  await stubLibrarian(page, async route => {
    await route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ mpn: 'XYZ9999ABC', found: false,
                             reason: 'Digi-Key has no part with exactly this number' }) })
  })
  await loadFixture(page, PARTS)
  await clickWorld(page, 25, 6)
  await page.getByTestId('part-source-btn').click({ timeout: CATALOGUE_MS })
  const miss = page.getByTestId('part-sourced-miss')
  await expect(miss).toBeVisible()
  await expect(miss).toContainText('no part with exactly this number')
  // a miss is not an error, and must not be dressed as one
  await expect(page.getByTestId('part-source-error')).toHaveCount(0)
  await expect(page.getByTestId('part-record')).toHaveCount(0)
})

test('a librarian that cannot be reached says so, and never claims the part is absent',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    await stubLibrarian(page, async route => {
      await route.fulfill({ status: 502, contentType: 'application/json',
        body: JSON.stringify({ detail: 'could not reach the parts distributor: digikey 401' }) })
    })
    await loadFixture(page, PARTS)
    await clickWorld(page, 25, 6)
    await page.getByTestId('part-source-btn').click({ timeout: CATALOGUE_MS })
    const err = page.getByTestId('part-source-error')
    await expect(err).toBeVisible()
    await expect(err).toContainText('could not reach the parts distributor')
    // the distinction that matters: nothing anywhere says the part does not exist
    await expect(page.getByTestId('part-sourced-miss')).toHaveCount(0)
    await expect(page.getByTestId('part-sourced')).toHaveCount(0)
  })

test('a deployment without the librarian route says that, rather than failing silently',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    await stubLibrarian(page, route => route.fulfill({ status: 404, contentType: 'text/html', body: '<!doctype html>' }))
    await loadFixture(page, PARTS)
    await clickWorld(page, 25, 6)
    await page.getByTestId('part-source-btn').click({ timeout: CATALOGUE_MS })
    await expect(page.getByTestId('part-source-error'))
      .toContainText('not configured on this deployment')
  })

test('a part the catalogue already has is never offered for sourcing', async ({ page }) => {
  test.setTimeout(CATALOGUE_MS + LOAD_MS)
  await loadFixture(page, PARTS)
  await clickWorld(page, 25, 20)         // Q1 = EPC2019, an exact catalogue hit
  await expect(page.getByTestId('part-record')).toBeVisible({ timeout: CATALOGUE_MS })
  await expect(page.getByTestId('part-source-btn')).toHaveCount(0)
})


test('the package check reads the measured body, and outranks an ambiguous case string',
  async ({ page }) => {
    // The case string is genuinely ambiguous and vendors disagree: Murata ships
    // 1.0 x 0.5 mm capacitors whose case field reads "0603" — the METRIC code
    // for what everyone else calls an imperial 0402. Read as an imperial code
    // that part is offered as fitting a 1.6 mm land it is 0.6 mm too short for.
    // Driven through the page so this is the SHIPPED module, not a copy of it.
    await page.goto('/')
    const out = await page.evaluate(async () => {
      const m = await import('/src/parts.js')
      const pkg = m.packageOf('Capacitor_SMD:C_0603_1608Metric')   // imperial 0603
      const real0603 = { caseCode: '0603', lengthM: 1.6e-3, widthM: 0.8e-3 }
      const murata   = { caseCode: '0603', lengthM: 1.0e-3, widthM: 0.5e-3 } // metric 0603
      const rotated  = { caseCode: '', lengthM: 0.8e-3, widthM: 1.6e-3 }     // axes swapped
      const caseOnly = { caseCode: '0603' }                                   // no drawing
      const nothing  = { caseCode: '' }
      const oversize = { caseCode: '0603', lengthM: 10e-3, widthM: 10e-3 }
      return {
        pkgCode: pkg?.code,
        real: m.packageMatches(pkg, real0603),
        murata: m.packageMatches(pkg, murata),
        rotated: m.packageMatches(pkg, rotated),
        caseOnly: m.packageMatches(pkg, caseOnly),
        nothing: m.packageMatches(pkg, nothing),
        oversize: m.packageMatches(pkg, oversize),
        size0805: m.rowSize({ lengthM: 2.0e-3, widthM: 1.25e-3 })?.code,
        size1210: m.rowSize({ lengthM: 3.2e-3, widthM: 2.5e-3 })?.code,
        size1206: m.rowSize({ lengthM: 3.2e-3, widthM: 1.6e-3 })?.code,
      }
    })
    expect(out.pkgCode).toBe('0603')
    // a real 0603 body fits, and the answer says the body decided
    expect(out.real).toEqual({ verdict: 'match', basis: 'size' })
    // the metric-0603 part does NOT, despite an identical case string
    expect(out.murata).toEqual({ verdict: 'differs', basis: 'size' })
    // a drawing that lists the axes the other way round is the same part
    expect(out.rotated).toEqual({ verdict: 'match', basis: 'size' })
    // with no drawing the case code still answers, and says so
    expect(out.caseOnly).toEqual({ verdict: 'match', basis: 'case' })
    // with neither, nothing is claimed
    expect(out.nothing).toEqual({ verdict: 'unknown', basis: null })
    // a body that is no standard chip cannot sit on a standard chip land
    expect(out.oversize).toEqual({ verdict: 'differs', basis: 'size' })
    // neighbouring sizes stay distinct on both axes
    expect(out.size0805).toBe('0805')
    expect(out.size1210).toBe('1210')
    expect(out.size1206).toBe('1206')
  })

test('a part whose measured body is a different size is not offered as a fit',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS + LOAD_MS)
    await loadFixture(page, PARTS)
    await clickWorld(page, 10, 20)
    const byValue = page.getByTestId('part-by-value')
    await expect(byValue).toBeVisible({ timeout: CATALOGUE_MS })
    // the excluded ones are counted and named as a size difference, not hidden
    await expect(byValue).toContainText('a different size')
  })


// ---------------------------------------------------------------------------
// The catalogue overlay: which of this board's parts the catalogue can resolve.
// Runs against the REAL catalogue (the fixture board needs only the mosfet,
// capacitor and resistor families), because the whole point of the layer is
// what the catalogue actually holds.
// ---------------------------------------------------------------------------

test('the catalogue overlay answers per part, and says what it could not answer',
  async ({ page }) => {
    test.setTimeout(CATALOGUE_MS * 2 + LOAD_MS)
    const errors = []
    page.on('pageerror', e => errors.push(String(e)))
    await loadFixture(page, PARTS)

    // it is never run unasked — the plain parts layer is what you get first
    await expect(page.getByTestId('catalogue-bar')).toHaveCount(0)
    await page.getByTestId('catalogue-toggle').click()

    const bar = page.getByTestId('catalogue-bar')
    await expect(bar).toBeVisible()
    await expect(bar).toContainText('identified', { timeout: CATALOGUE_MS * 2 })

    // Q1 (EPC2019) is a real catalogue part; Q2 (XYZ9999ABC) is not; C1 and R1
    // carry values, so they have candidates rather than an identity.
    // the summary line is the contract the user reads
    const text = await bar.textContent()
    expect(text).toMatch(/[1-9]\d* identified/)
    expect(text).toMatch(/with candidates/)
    expect(text).toMatch(/unmatched/)
    // the legend explains every colour the board is now wearing
    expect(text).toContain('the board never named it')

    // hovering a part now says what the catalogue said about it
    const canvas = page.getByTestId('board-canvas')
    const view = JSON.parse(await canvas.getAttribute('data-view'))
    const box = await canvas.boundingBox()
    await page.mouse.move(box.x + (25 - view.ox) * view.scale, box.y + (20 - view.oy) * view.scale)
    await expect(page.getByTestId('board-tooltip')).toContainText('in the catalogue as EPC2019')

    // clicking the chip again drops back to the plain layer
    await page.getByTestId('catalogue-toggle').click()
    await expect(page.getByTestId('catalogue-bar')).toHaveCount(0)
    expect(errors).toEqual([])
  })

test('a part the board never named is not counted as a catalogue miss', async ({ page }) => {
  // H1 is a mounting hole and is not a part at all; a component with neither a
  // part number nor a value is a gap in the EXPORT, and blaming the catalogue
  // for it would send the reader looking in the wrong place.
  await page.goto('/')
  const out = await page.evaluate(async () => {
    const m = await import('/src/parts.js')
    const res = await m.sweepBoard([
      { ref: 'X1', value: '', footprint: 'Some:Odd_Thing', x: 0, y: 0, rot: 0 },
    ])
    return Object.fromEntries(res)
  })
  expect(out.X1.state).toBe('unlookupable')
  expect(out.X1.why).toContain('neither a part number nor a value')
})
