import { expect, test } from '@playwright/test';

// GitHub Pages cannot send the isolation headers; the service worker adds them
// after one reload and the editor starts as it does on a host with headers.
const site = 'http://127.0.0.1:4179/flows/';

test('starts without isolation headers under a subpath', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(site);
  await expect(page.locator('main.flows-app')).toBeVisible();
  await expect(page.locator('.program-state.ready')).toBeVisible();
  expect(await page.evaluate(() => globalThis.crossOriginIsolated)).toBe(true);
  expect(await page.evaluate(() => sessionStorage.getItem('flows.isolationReload'))).toBeNull();
  // Later visits are isolated from the first response.
  await page.reload();
  await expect(page.locator('.program-state.ready')).toBeVisible();
  expect(await page.evaluate(() => globalThis.crossOriginIsolated)).toBe(true);
  expect(errors).toEqual([]);
});

test('leaves the embed pages under the same site unisolated', async ({ page }) => {
  // Visiting the app installs its isolation worker for the whole /flows/ scope.
  await page.goto(site);
  await expect(page.locator('.program-state.ready')).toBeVisible();
  expect(await page.evaluate(() => globalThis.crossOriginIsolated)).toBe(true);
  await page.goto(`${site}embed/examples/index.html`);
  await expect(page.getByRole('heading', { name: 'Flows Embed' })).toBeVisible();
  expect(await page.evaluate(() => globalThis.crossOriginIsolated)).toBe(false);
  await page.waitForFunction(() => (document.querySelector('flows-instrument') as HTMLElement & { flows?: { state: string } })?.flows?.state === 'ready');
});
