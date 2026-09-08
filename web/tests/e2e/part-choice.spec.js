// Choosing which part a footprint holds, and being told when there is nothing
// to choose from. Both reported on the demo board.
//
// Reaches the REAL catalogue, like the other parts specs: the by-value list is
// a shard read, and stubbing it would test the stub.
//
// Headless always (house rule).
import { test, expect } from '@playwright/test'

test.setTimeout(280000)

// click the board at a WORLD coordinate, through the canvas's own transform
async function clickWorld(page, x, y) {
  const canvas = page.getByTestId('board-canvas')
  const view = JSON.parse(await canvas.getAttribute('data-view'))
  const box = await canvas.boundingBox()
  await page.mouse.click(box.x + (x - view.ox) * view.scale,
                         box.y + (y - view.oy) * view.scale)
}

async function demo(page) {
  await page.goto('/')
  await page.getByTestId('load-demo').click()
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('board-canvas')).first())
    .toBeVisible({ timeout: 120000 })
  if (await card.count()) await card.getByText(/Default/).first().click()
  await expect(page.getByTestId('board-canvas')).toBeVisible({ timeout: 120000 })
  await page.waitForTimeout(4000)
}

async function closePanel(page) {
  await page.locator('[data-testid="part-panel"] button.x').click()
  await expect(page.getByTestId('part-panel')).toHaveCount(0)
}

// L1 is a 32.8 mm toroid at (120, 58); its body is big enough that a couple of
// offsets find it wherever the hit test lands.
async function openL1(page) {
  for (const [dx, dy] of [[0, 8], [0, 0], [8, 0], [-8, 0], [0, -8]]) {
    await clickWorld(page, 120 + dx, 58 + dy)
    await page.waitForTimeout(900)
    if (await page.getByTestId('part-panel').count()) {
      const t = (await page.getByTestId('part-panel').innerText()).split('\n')[0]
      if (t.startsWith('L1')) return true
      await closePanel(page)
      await page.waitForTimeout(300)
    }
  }
  return false
}

test('an empty by-value list does not invite a pick', async ({ page }) => {
  // C1 is 390 uF in an 18 mm radial can. Nothing in the catalogue matches both
  // the value and that body, so the list is empty — and the panel used to say
  // "Pick one to read its record" over the top of it, with nothing to pick.
  await demo(page)
  await clickWorld(page, 117, 96)
  await expect(page.getByTestId('part-panel')).toBeVisible()
  const bv = page.getByTestId('part-by-value')
  await expect(bv).toBeVisible({ timeout: 240000 })

  const before = (await bv.innerText()).replace(/\s+/g, ' ')
  expect(await bv.locator('li').count()).toBe(0)
  expect(before).not.toContain('Pick one to read its record')

  // the size-less matches are a real answer when the alternative is none
  await page.getByTestId('show-unsized').click()
  await page.waitForTimeout(800)
  const after = (await bv.innerText()).replace(/\s+/g, ' ')
  expect(await bv.locator('li').count()).toBeGreaterThan(0)
  expect(after).toContain('Pick one to read its record')
  // ...and it must stop saying they are not shown while showing them
  expect(after).not.toContain('are not shown')
})

test('a part chosen for a component is remembered', async ({ page }) => {
  // Picking a part IS the answer to "which part is this?", and it used to live
  // and die inside the panel: the board never changed, and closing the modal
  // threw the choice away, so the next click offered the same search again.
  const errs = []
  page.on('pageerror', e => errs.push(String(e)))
  page.on('console', m => { if (m.type() === 'error') errs.push(m.text()) })

  await demo(page)
  expect(await openL1(page), 'L1 opens').toBe(true)
  const bv = page.getByTestId('part-by-value')
  await expect(bv).toBeVisible({ timeout: 240000 })
  if (!(await bv.locator('li').count())) {
    const unsized = page.getByTestId('show-unsized')
    if (await unsized.count()) { await unsized.click(); await page.waitForTimeout(800) }
  }
  const first = bv.locator('li button.lnk').first()
  const mpn = (await first.innerText()).trim()
  await first.click()
  await page.waitForTimeout(2500)

  await closePanel(page)
  await page.waitForTimeout(600)
  expect(await openL1(page), 'L1 opens again').toBe(true)
  const panel = (await page.getByTestId('part-panel').innerText()).replace(/\s+/g, ' ')
  expect(panel, 'the part chosen a moment ago is still the answer').toContain(mpn)
  expect(errs).toEqual([])
})
