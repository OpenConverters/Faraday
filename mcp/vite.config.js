import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// MCP App resources render in a deny-by-default CSP iframe, so the widget must be ONE
// self-contained file: no external script/style/font requests.
//
// The Vue plugin is here so the widget can import the web app's real BoardView out of
// ../web/src instead of reimplementing board rendering — the board an engineer clicks in a
// chat is drawn by the same component, from the same report JSON, as the one in the browser
// app. One definition, two surfaces.
// IMPORTING THE WEB APP'S COMPONENT MEANS INHERITING THE WEB APP'S ALIASES.
// BoardView reaches the parts inspector, which imports Kelvin's own browser code
// as `@kelvin/...` — resolved in web/vite.config.js and, until now, nowhere here.
// So the moment that import appeared the widget build stopped resolving and the
// bundle froze at its last good build (2026-08-27) while the server kept
// deploying: prod served a widget two weeks behind its own source, and nothing
// said so, because a build nobody runs fails silently.
//
// Same resolution and the same explicit refusal as the web app, deliberately: two
// configs that disagree about where Kelvin lives is the next version of this bug.
const here = path.dirname(fileURLToPath(import.meta.url));
const kelvinSrc = process.env.KELVIN_WEB_SRC ||
  path.resolve(here, "../../Kelvin/web/src");
if (!existsSync(path.join(kelvinSrc, "crossref.js"))) {
  throw new Error(
    `Kelvin's web sources not found at ${kelvinSrc} — BoardView's parts ` +
    `inspector imports them (crossref.js, engine.js, curves.js). Check out ` +
    `OpenConverters/Kelvin beside Faraday, or set KELVIN_WEB_SRC.`);
}

export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  resolve: { alias: { "@kelvin": kelvinSrc } },
  build: {
    outDir: "dist",
    emptyOutDir: false,
    rollupOptions: { input: process.env.INPUT || "board.html" },
  },
});
