// Light and dark. Not two skins over one design: the board render is the
// hero, and a ramp or a layer colour tuned to glow on a dark ground is the
// hardest thing to see on a pale one. So the palette has one definition
// (style.css), the canvas reads it back rather than keeping a second copy,
// and the tests here check the parts that a screenshot cannot: that the
// choice is remembered, that the system's preference is honoured until the
// user overrides it, and that the canvas actually repaints when it flips.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const LOAD_MS = process.env.FARADAY_E2E_BASE ? 75000 : 30000

const root = page => page.locator('html')

test('with nothing stored, the operating system decides', async ({ browser }) => {
  for (const scheme of ['dark', 'light']) {
    const ctx = await browser.newContext({ colorScheme: scheme })
    const page = await ctx.newPage()
    await page.goto('/')
    await expect(root(page)).toHaveAttribute('data-theme', scheme)
    await ctx.close()
  }
})

test('a choice outranks the system, and survives a reload', async ({ browser }) => {
  const ctx = await browser.newContext({ colorScheme: 'dark' })
  const page = await ctx.newPage()
  await page.goto('/')
  await expect(root(page)).toHaveAttribute('data-theme', 'dark')
  await page.getByTestId('theme-toggle').click()
  await expect(root(page)).toHaveAttribute('data-theme', 'light')
  await page.reload()
  await expect(root(page)).toHaveAttribute('data-theme', 'light')
  // and the OS moving underneath no longer speaks for the user
  await page.emulateMedia({ colorScheme: 'dark' })
  await expect(root(page)).toHaveAttribute('data-theme', 'light')
  await ctx.close()
})

test('the board repaints on a flip: the canvas is pixels, not styles',
  async ({ page }) => {
    const errs = []
    page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })
    page.on('pageerror', e => errs.push(String(e)))
    await page.addInitScript(() => localStorage.setItem('faraday.theme', 'dark'))
    await page.goto('/')
    await page.getByTestId('file-input').setInputFiles(MPPT)
    const card = page.getByTestId('stackup-card')
    await expect(card.or(page.getByTestId('board-canvas')).first())
      .toBeVisible({ timeout: LOAD_MS })
    if (await card.count()) await card.getByText('Default 4-layer').click()
    const canvas = page.getByTestId('board-canvas')
    await expect(canvas).toBeVisible({ timeout: LOAD_MS })

    // the substrate the canvas last painted, straight from its own hook
    const substrate = async () =>
      JSON.parse(await canvas.getAttribute('data-ramp')).board
    const dark = await substrate()
    await page.getByTestId('theme-toggle').click()
    await expect(root(page)).toHaveAttribute('data-theme', 'light')
    const light = await substrate()
    expect(light, 'the canvas kept the dark substrate after the flip')
      .not.toBe(dark)

    // and nothing in the flip is an error
    expect(errs).toEqual([])
  })
