import { defineConfig } from '@playwright/test'

// FARADAY_E2E_BASE points the suite at a deployed site (the deploy script uses
// this to prove the LIVE app runs, not just that its files serve); unset, it
// spins up the local dev server.
const remote = process.env.FARADAY_E2E_BASE

export default defineConfig({
  testDir: 'tests/e2e',
  timeout: 30000,
  use: { baseURL: remote || 'http://localhost:5199' },
  ...(remote ? {} : {
    webServer: {
      command: 'npm run dev -- --port 5199 --strictPort',
      url: 'http://localhost:5199',
      reuseExistingServer: true,
    },
  }),
})
