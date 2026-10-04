// The cross-reference table widget, rendered headless from a REAL crossref_board result.
//
//   cd mcp && npm run build && npx playwright test
//   CROSSREF_FIXTURE=<a recorded {content, structuredContent}> npx playwright test
//   CROSSREF_BUNDLE=../../Kelvin/mcp/dist/crossref-table.html (Kelvin ships the same widget)
//
// The committed fixture is crossref_board(glasgow.kicad_pcb, target Würth Elektronik), an
// open-hardware board. CROSSREF_FIXTURE renders any other recorded result the same way.
import { test, expect } from "@playwright/test";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSync } from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const BUNDLE = process.env.CROSSREF_BUNDLE || join(HERE, "..", "dist", "crossref-table.html");
const FIXTURE = process.env.CROSSREF_FIXTURE || join(HERE, "fixtures", "crossref-glasgow-wurth.json");

const host = buildSync({
  entryPoints: [join(HERE, "widget-host.js")], bundle: true, write: false, format: "iife",
}).outputFiles[0].text;

async function mount(page, theme = "dark") {
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.setContent('<!doctype html><body style="margin:0">'
    + '<iframe id="w" sandbox="allow-scripts" style="width:1280px;height:720px;border:0">'
    + "</iframe></body>");
  await page.addScriptTag({ content: host });
  const result = JSON.parse(readFileSync(FIXTURE, "utf8"));
  await page.evaluate(([html, r, t]) => window.mountWidget(html, r, t),
                      [readFileSync(BUNDLE, "utf8"), result, theme]);
  return { result, errors, frame: page.frameLocator("#w") };
}

const qtySum = async (frame) => (await frame.locator("tr.row td.qty").allInnerTexts())
  .reduce((n, s) => n + Number(s), 0);

test("every position on the board is in the table, once", async ({ page }) => {
  const { result, errors, frame } = await mount(page);
  const sc = result.structuredContent;
  await expect(frame.locator("h1")).toContainText(`${sc.total} position`);
  const withSub = sc.lines.filter((l) => ["recommended", "partial"].includes(l.status)).length;
  await expect(frame.locator("h1")).toContainText(
    `${withSub} with a ${sc.targetManufacturer} substitute`);
  expect(await qtySum(frame)).toBe(sc.total);
  // Every substitute the tool named is on screen.
  const text = await frame.locator("table").innerText();
  for (const mpn of new Set(sc.lines.map((l) => l.mpn).filter(Boolean))) expect(text).toContain(mpn);
  await page.screenshot({ path: test.info().outputPath("crossref-table.png") });
  expect(errors).toEqual([]);
});

test("a status chip filters to that status and its positions add up", async ({ page }) => {
  const { result, frame } = await mount(page, "light");
  const lines = result.structuredContent.lines;
  for (const status of [...new Set(lines.map((l) => l.status))]) {
    await frame.locator(`button.chip.${status}`).click();
    const want = lines.filter((l) => l.status === status).length;
    await expect(frame.locator(`button.chip.${status} .n`)).toHaveText(String(want));
    expect(await qtySum(frame)).toBe(want);
    expect(await frame.locator(`tr.row:not(.${status})`).count()).toBe(0);
  }
});

test("clicking a row tells the model which positions the user picked", async ({ page }) => {
  const { frame } = await mount(page);
  const row = frame.locator("tr.row").first();
  const refs = await row.locator("td.refs").innerText();
  await row.click();
  await expect(row).toHaveClass(/chosen/);
  const ctx = await page.evaluate(() => window.widgetContexts);
  expect(ctx.length).toBe(1);
  expect(ctx[0].content[0].text).toContain(`[user selected] ${refs}`);
});
