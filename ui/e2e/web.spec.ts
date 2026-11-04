import { expect, test, type Page } from '@playwright/test';
import { fileURLToPath } from 'node:url';

// The browser build of Flows: the WebAssembly core, its audio worklet,
// browser storage and the same React editor as the macOS app.
declare global { interface Window { __flows: { request(method: string, params?: Record<string, unknown>): Promise<any>; devices(): any } } }

const kick = fileURLToPath(new URL('../../test/fixtures/samples/flows-kick.wav', import.meta.url));
const shots = (name: string) => `test-results/screens/${name}.png`;

async function open(page: Page) {
  await page.goto('/');
  await expect(page.locator('main.flows-app')).toBeVisible();
  await expect(page.locator('.program-state.ready')).toBeVisible();
}
const call = (page: Page, method: string, params: Record<string, unknown> = {}) =>
  page.evaluate(([m, p]) => window.__flows.request(m as string, p as Record<string, unknown>), [method, params] as const);

test.beforeEach(async ({ context }) => {
  // Each test starts with an empty browser workspace.
  await context.clearCookies();
});

test('starts, compiles the starter instrument and reports browser capabilities', async ({ page }, info) => {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await open(page);
  const snapshot = await call(page, 'session.get');
  expect(snapshot.protocol).toBe(1);
  expect(snapshot.capabilities).toMatchObject({ host: 'web', audioSettings: 'in-app', fileDialogs: 'browser', secrets: 'session', liveBridge: false });
  expect(snapshot.capabilities.sourceUrl).toContain('github.com/flows-audio/flows');
  expect(snapshot.files.map((file: { path: string }) => file.path)).toContain('examples/sine-reference.flows');
  await expect(page.locator('.web-welcome')).toBeVisible();
  await page.screenshot({ path: shots(`${info.project.name}-first-visit`) });
  await page.getByRole('button', { name: 'Dismiss' }).click();
  await expect(page.locator('.web-welcome')).toHaveCount(0);
  expect(errors).toEqual([]);
});

test('edits source and recompiles in the WebAssembly core', async ({ page }) => {
  await open(page);
  const before = await call(page, 'session.get');
  const text = `# Edited in the browser\n${before.document.text}`;
  await call(page, 'document.apply', { documentId: before.document.id, baseRevision: before.document.revision, text });
  await expect.poll(async () => (await call(page, 'session.get')).compile.status).toBe('ready');
  const after = await call(page, 'session.get');
  expect(after.document.text.startsWith('# Edited in the browser')).toBe(true);
  expect(after.document.dirty).toBe(true);
  expect(after.compile.parameters.length).toBeGreaterThan(0);
  await expect(page.locator('.cm-content')).toContainText('Edited in the browser');
});

test('plays a note through the audio worklet', async ({ page, browserName }) => {
  await open(page);
  // A click anywhere starts audio; resuming the context can take a moment.
  await page.locator('.document-toolbar').click();
  const state = await page.waitForFunction(() => {
    const value = window.__flows.devices().audio.state;
    return ['running', 'failed', 'unsupported'].includes(value) ? value : null;
  }, null, { timeout: 15_000 }).then(handle => handle.jsonValue()).catch(() => 'suspended');
  // CI gives the browsers a virtual output device (.github/scripts/virtual-audio.sh),
  // so Chromium and Firefox must play there. WebKit's Linux build and machines
  // without an output device may not start audio; those runs skip.
  if (process.env.CI && browserName !== 'webkit') expect(state, `${browserName} audio state`).toBe('running');
  else test.skip(state !== 'running', `${browserName} did not start audio in this environment (${state})`);
  const snapshot = await call(page, 'session.get');
  await expect.poll(async () => (await call(page, 'session.get')).audio.running).toBe(true);
  await call(page, 'keyboard.noteOn', { noteId: 1, pitch: 60, velocity: 0.9, documentId: snapshot.document.id });
  await expect.poll(async () => (await call(page, 'session.get')).audio.peak, { timeout: 10_000 }).toBeGreaterThan(0.01);
  await call(page, 'keyboard.noteOff', { noteId: 1, pitch: 60, documentId: snapshot.document.id });
  await expect(page.locator('.audio-device .connection-dot.active')).toBeVisible();
});

test('imports a sample with the browser file picker', async ({ page }) => {
  await open(page);
  const chooser = page.waitForEvent('filechooser');
  await page.getByRole('button', { name: 'Import audio sample' }).click();
  await (await chooser).setFiles(kick);
  await expect.poll(async () => (await call(page, 'session.get')).samples.map((sample: { path: string }) => sample.path)).toContain('Samples/flows-kick(2).wav');
});

test('keeps the workspace after a reload', async ({ page }) => {
  await open(page);
  await call(page, 'document.new', { name: 'Persisted' });
  await expect.poll(async () => (await call(page, 'session.get')).document.id).toBe('examples/Persisted.flows');
  await page.waitForTimeout(1500);
  await page.reload();
  await expect(page.locator('.program-state.ready')).toBeVisible();
  const snapshot = await call(page, 'session.get');
  expect(snapshot.files.map((file: { path: string }) => file.path)).toContain('examples/Persisted.flows');
  expect(snapshot.documents.map((document: { id: string }) => document.id)).toContain('examples/Persisted.flows');
});

test('opens in one tab at a time and hands over on request', async ({ page, context }, info) => {
  await open(page);
  const second = await context.newPage();
  await second.goto('/');
  await expect(second.getByRole('heading', { name: 'Flows is open in another tab' })).toBeVisible();
  await second.screenshot({ path: shots(`${info.project.name}-busy`) });
  await second.getByRole('button', { name: 'Use Flows here' }).click();
  await expect(second.locator('.program-state.ready')).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Flows moved to another tab' })).toBeVisible();
});

test('shows audio, MIDI and storage settings in the page', async ({ page }, info) => {
  await open(page);
  await page.getByRole('button', { name: 'Dismiss' }).click();
  await page.locator('button.audio-device').click();
  const panel = page.getByRole('dialog', { name: 'Audio and MIDI' });
  await expect(panel).toBeVisible();
  await expect(panel.getByRole('heading', { name: 'Output' })).toBeVisible();
  await expect(panel.getByRole('heading', { name: 'MIDI keyboards' })).toBeVisible();
  await expect(panel.getByRole('heading', { name: 'Storage' })).toBeVisible();
  await page.screenshot({ path: shots(`${info.project.name}-audio-settings`) });
  await page.keyboard.press('Escape');
  await expect(panel).toHaveCount(0);
});

test('runs an assistant request through the browser network bridge', async ({ page }) => {
  // A loopback endpoint needs no key; the page's fetch() is answered here.
  let requests = 0;
  await page.route('http://localhost:8787/v1/chat/completions', async route => {
    const cors = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'content-type, accept, authorization', 'Access-Control-Allow-Methods': 'POST' };
    if (route.request().method() === 'OPTIONS') return route.fulfill({ status: 204, headers: cors });
    ++requests;
    const body = route.request().postDataJSON();
    expect(body.model).toBe('browser-test');
    await route.fulfill({ status: 200, headers: { ...cors, 'Content-Type': 'application/json' },
      body: JSON.stringify({ choices: [{ message: { role: 'assistant', content: 'A lowpass keeps the lows.' } }] }) });
  });
  await open(page);
  await call(page, 'ai.models.save', { name: 'Browser test', endpoint: 'http://localhost:8787/v1', model: 'browser-test' });
  const snapshot = await call(page, 'session.get');
  expect(snapshot.ai.configured).toBe(true);
  const reply = await call(page, 'ai.send', { requestId: 'browser-request', prompt: 'What does a lowpass do?' });
  expect(reply.status).toBe('working');
  await expect.poll(async () => {
    const view = await call(page, 'ai.conversations.select', { conversationId: reply.conversationId });
    const message = view.conversation?.messages?.[0];
    return message?.status === 'failed' ? `failed: ${message.text}` : message?.status;
  }).toBe('done');
  const view = await call(page, 'ai.conversations.select', { conversationId: reply.conversationId });
  expect(view.conversation.messages[0].text).toBe('A lowpass keeps the lows.');
  expect(requests).toBe(1);
});
