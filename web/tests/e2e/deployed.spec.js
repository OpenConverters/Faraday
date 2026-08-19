// The deploy gate: is the LIVE engine the code at HEAD, and does it run?
//
// Runs only with FARADAY_E2E_BASE set (the deploy script sets it); skipped
// locally, where there is no deployment to check.
//
// WHY THIS IS NOT A HASH COMPARISON. The house rule is to byte-verify the live
// artifact against a clean-HEAD rebuild — and for the SPA bundle and index.html
// that works and the deploy script does it. For the WASM engine it cannot:
// this emcc toolchain is NOT byte-reproducible. Two builds of identical source
// in the identical directory, back to back, produce two different binaries —
// the same 1,372,181 bytes with 18 bytes different inside the code section, no
// custom sections involved, flipping between exactly two outputs. Serializing
// the optimizer (BINARYEN_CORES=1, EMCC_CORES=1) does not settle it, which
// points at pointer-ordered containers somewhere in LLVM/Binaryen rather than
// at anything a flag can reach.
//
// So a hash mismatch on faraday.wasm proves nothing, and — worse — a hash
// MATCH would have been luck. What actually matters is behavioural: does the
// engine serving on prod produce the same report as a clean-HEAD build, for a
// real board, finding for finding. That is what this asserts. It subsumes the
// 18 bytes: if any of them mattered, the reports would differ.
import { test, expect } from '@playwright/test'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const BASE = process.env.FARADAY_E2E_BASE
const here = path.dirname(fileURLToPath(import.meta.url))
const MPPT = path.join(here, '../../../cpp/tests/fixtures/real/mppt-2420-hc.kicad_pcb')
const LOCAL = process.env.FARADAY_LOCAL_BASE || 'http://localhost:5199'

test.skip(!BASE, 'no deployment to check (set FARADAY_E2E_BASE)')

// The engine's own answer for one board, as the engine serializes it.
async function reportFrom(page, url, boardText) {
  await page.goto(url)
  await page.waitForFunction(() => typeof window.createFaraday === 'function')
  return page.evaluate(async (board) => {
    const eng = await window.createFaraday()
    const raw = eng.analyze(board, 'default-4layer')
    const r = JSON.parse(raw)
    // The findings and the meta are the product; the board geometry is the
    // input echoed back and would only add noise to a mismatch report.
    return JSON.stringify({ findings: r.findings, meta: r.meta,
                            version: r.faraday, format: r.format })
  }, boardText)
}

test('the live engine answers exactly as a clean-HEAD build does', async ({ page }) => {
  const board = fs.readFileSync(MPPT, 'utf8')
  const live = await reportFrom(page, BASE + '/', board)
  const local = await reportFrom(page, LOCAL + '/', board)
  expect(live.length).toBeGreaterThan(1000)
  // Finding for finding, number for number. A difference here is a real
  // difference in the deployed engine, whatever the file hashes say.
  expect(live).toBe(local)
})

test('the live app runs end to end, with a clean console', async ({ page }) => {
  const errors = []
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()) })
  page.on('pageerror', e => errors.push(String(e)))

  await page.goto(BASE + '/')
  await page.getByTestId('file-input').setInputFiles(MPPT)
  const card = page.getByTestId('stackup-card')
  await expect(card.or(page.getByTestId('finding-F-0001')).first())
    .toBeVisible({ timeout: 90000 })
  if (await card.count()) await card.getByText('Default 4-layer').click()

  // a real result, rendered: findings ranked and the board drawn
  await expect(page.getByTestId('finding-F-0001')).toBeVisible({ timeout: 90000 })
  await expect(page.getByTestId('board-canvas')).toBeVisible()
  const count = Number(await page.getByTestId('finding-count').textContent())
  expect(count).toBeGreaterThan(10)

  // and this session's work is actually on prod: the guided/advanced switch
  await expect(page.getByTestId('view-toggle')).toBeVisible()

  expect(errors, `console errors on ${BASE}:\n${errors.join('\n')}`).toEqual([])
})
