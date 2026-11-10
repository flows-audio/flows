import { defineConfig, devices } from '@playwright/test';

// Browser smoke tests for the web build (npm run build:web first).
const port = 4178, pagesPort = 4179, embedPort = 4180;
export default defineConfig({
  testDir: './e2e',
  timeout: 90_000,
  expect: { timeout: 20_000 },
  retries: process.env.CI ? 1 : 0,
  workers: 1,
  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : 'list',
  use: { baseURL: `http://127.0.0.1:${port}/`, trace: 'retain-on-failure', viewport: { width: 1440, height: 900 } },
  webServer: [{
    command: `npx vite preview --mode web --host 127.0.0.1 --port ${port} --strictPort`,
    url: `http://127.0.0.1:${port}/`,
    reuseExistingServer: !process.env.CI,
  }, {
    // GitHub Pages: a subpath and no isolation headers (e2e/pages.spec.ts).
    command: `node e2e/pages-server.mjs ${pagesPort}`,
    url: `http://127.0.0.1:${pagesPort}/flows/`,
    reuseExistingServer: !process.env.CI,
  }, {
    // Another site that embeds Flows from the Pages origin (e2e/embed.spec.ts).
    command: `node e2e/embed-host.mjs ${embedPort} http://127.0.0.1:${pagesPort}/flows/embed/`,
    url: `http://localhost:${embedPort}/fixtures/multi.html`,
    reuseExistingServer: !process.env.CI,
  }],
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'], viewport: { width: 1440, height: 900 }, launchOptions: { args: ['--autoplay-policy=no-user-gesture-required'] } } },
    { name: 'firefox', use: { ...devices['Desktop Firefox'], viewport: { width: 1440, height: 900 }, launchOptions: { firefoxUserPrefs: { 'media.autoplay.default': 0, 'media.autoplay.block-webaudio': false } } } },
    { name: 'webkit', use: { ...devices['Desktop Safari'], viewport: { width: 1440, height: 900 } } },
  ],
});
