// The wordmark as the way out of a review.
//
// Going home is not just hiding the board: everything the session accumulated
// was ABOUT that board, and leaving any of it behind would decorate the next
// one with the last one's answers. These check that it really goes, that the
// same file can be opened again afterwards (a file input keeps its selection
// and fires no change event on a re-pick), and that work nobody can get back
// by re-dropping the file is not thrown away silently.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('faraday.view', 'advanced'))
})

async function loadMppt(page) {
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first()).toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
}

test('with no board open the wordmark is not a button', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByText('Drop a board here')).toBeVisible()
  // nowhere to go, so it must not pretend to be a way of going there
  await expect(page.getByTestId('go-home')).toHaveCount(0)
})

test('the wordmark closes the board and returns to the start screen', async ({ page }) => {
  await loadMppt(page)
  await expect(page.getByTestId('meta-strip')).toBeVisible()

  await page.getByTestId('go-home').click()

  await expect(page.getByText('Drop a board here')).toBeVisible()
  await expect(page.getByTestId('finding-F-0001')).toHaveCount(0)
  await expect(page.getByTestId('meta-strip')).toHaveCount(0)
  await expect(page.getByTestId('board-canvas')).toHaveCount(0)
  // and the way out is gone with the board it was for
  await expect(page.getByTestId('go-home')).toHaveCount(0)
})

test('the SAME board can be opened again after going home', async ({ page }) => {
  // A file input keeps its last selection, so re-picking the same file fires
  // no change event — without clearing it, going home would strand you on the
  // start screen with the board you just closed unopenable.
  await loadMppt(page)
  await page.getByTestId('go-home').click()
  await expect(page.getByText('Drop a board here')).toBeVisible()

  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first()).toBeVisible({ timeout: LOAD_MS })
  if (await card.count()) await card.getByText('Default 4-layer').click()
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
})

test('the next board is not screened with the last one\'s promotions', async ({ page }) => {
  // The ENGINE clears its own session promotions on import ("new board, new
  // session"), so this pins that guarantee rather than the UI's reset — a
  // check worth having precisely because the UI cannot be what enforces it.
  await loadMppt(page)
  const chip = page.getByTestId('meta-sw-candidates').locator('button.candbtn').first()
  if (await chip.count()) {
    const net = (await chip.textContent()).replace(/^\s*⊕\s*/, '').trim()
    await chip.click()
    // the promoted net joins the screened switch nodes, marked as the user's call
    await expect(page.getByTestId('meta-strip')).toContainText(net, { timeout: LOAD_MS })
    page.once('dialog', d => d.accept())
    await page.getByTestId('go-home').click()
    await expect(page.getByText('Drop a board here')).toBeVisible()

    await page.getByTestId('file-input').setInputFiles(MPPT)
    const card = page.getByTestId('stackup-card')
    await expect(card.or(page.getByTestId('finding-F-0001')).first()).toBeVisible({ timeout: LOAD_MS })
    if (await card.count()) await card.getByText('Default 4-layer').click()
    await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: LOAD_MS })
    // the next board is screened WITHOUT the promotion: the net is back to
    // being a candidate, not a screened switch node
    const strip = await page.getByTestId('meta-strip').textContent()
    const beforeCandidates = strip.split('candidate switch node')[0]
    expect(beforeCandidates).not.toContain(net)
  }
})

test('work a re-drop cannot restore is confirmed before it goes', async ({ page }) => {
  await loadMppt(page)
  const chip = page.getByTestId('meta-sw-candidates').locator('button.candbtn').first()
  test.skip(await chip.count() === 0, 'this board offers no candidate to promote')
  const net = (await chip.textContent()).replace(/^\s*⊕\s*/, '').trim()
  await chip.click()
  await expect(page.getByTestId('meta-strip')).toContainText(net, { timeout: LOAD_MS })

  // cancelling keeps the board AND the promotion
  let asked = ''
  page.once('dialog', d => { asked = d.message(); d.dismiss() })
  await page.getByTestId('go-home').click()
  expect(asked).toContain('promoted switch node')
  await expect(page.getByTestId('finding-F-0001')).toBeVisible()
  await expect(page.getByTestId('meta-strip')).toContainText(net)

  // accepting closes it
  page.once('dialog', d => d.accept())
  await page.getByTestId('go-home').click()
  await expect(page.getByText('Drop a board here')).toBeVisible()
})

test('a board with nothing to lose closes without asking', async ({ page }) => {
  await loadMppt(page)
  let asked = false
  page.on('dialog', d => { asked = true; d.accept() })
  await page.getByTestId('go-home').click()
  await expect(page.getByText('Drop a board here')).toBeVisible()
  expect(asked).toBe(false)
})


test('a board that FAILED to load can still be closed', async ({ page }) => {
  // An Altium .PcbDoc is a binary CAD database and is refused by name. That
  // leaves an error banner and no report, and the wordmark used to be inert
  // there — an error you could not dismiss without reloading the page.
  await page.goto('/')
  await page.getByTestId('file-input').setInputFiles({
    name: 'PoE_USBC.PcbDoc',
    mimeType: 'application/octet-stream',
    buffer: Buffer.from('\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1' + 'x'.repeat(400), 'binary'),
  })
  await expect(page.getByTestId('error-banner')).toBeVisible({ timeout: LOAD_MS })

  await expect(page.getByTestId('go-home')).toBeVisible()
  await page.getByTestId('go-home').click()

  await expect(page.getByTestId('error-banner')).toHaveCount(0)
  await expect(page.getByText('Drop a board here')).toBeVisible()
})
