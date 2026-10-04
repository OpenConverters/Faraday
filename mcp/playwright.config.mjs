import { defineConfig } from "@playwright/test";

// Widget tests only: a blank page hosts the built bundle through the official AppBridge
// (tests/widget-host.js). Headless, always.
export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.mjs/,
  timeout: 30000,
  use: { headless: true },
});
