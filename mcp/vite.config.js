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
export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  build: {
    outDir: "dist",
    emptyOutDir: false,
    rollupOptions: { input: process.env.INPUT || "board.html" },
  },
});
