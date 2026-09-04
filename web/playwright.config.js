import { defineConfig } from '@playwright/test'

// FARADAY_E2E_BASE points the suite at a deployed site (the deploy script uses
// this to prove the LIVE app runs, not just that its files serve); unset, it
// spins up the local dev server.
const remote = process.env.FARADAY_E2E_BASE
// the local dev server's port — overridable, because 5199 may be taken by
// another project's dev server on a shared machine
const port = process.env.FARADAY_DEV_PORT || '5199'

export default defineConfig({
  testDir: 'tests/e2e',
  // Against a deployed site every board test pays for a 656 kB WASM download
  // and a 1.16 MB layout over the network, where locally both are cached. That
  // is real work, not flakiness, and 30 s is not enough for it — the failures
  // were the test clock expiring, not the app.
  timeout: remote ? 150000 : 30000,
  use: { baseURL: remote || `http://localhost:${port}` },
  ...(remote ? {} : {
    webServer: {
      command: `npm run dev -- --port ${port} --strictPort`,
      url: `http://localhost:${port}`,
      reuseExistingServer: true,
    },
  }),
})
