import { expect, test, type Page } from '@playwright/test';

// Flows Embed on "someone else's website": pages from e2e/embed-host.mjs
// (localhost:4180, no isolation headers, no service worker) load the embed
// from the GitHub Pages stand-in (127.0.0.1:4179), a different origin.
// The example pages are the ones in embed/examples and the integration guide.
const host = 'http://localhost:4180';
const shots = (name: string) => `test-results/screens/embed-${name}.png`;

declare global {
  interface Window { flows?: any }
}
type Instrument = HTMLElement & { flows: any; ready: Promise<any> };

async function visit(page: Page, path: string) {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  // Example pages use Google Fonts; tests stay offline and deterministic.
  await page.route(/fonts\.(googleapis|gstatic)\.com/, route => route.abort());
  await page.goto(`${host}${path}`);
  return errors;
}
const ready = (page: Page, selector: string) =>
  page.waitForFunction(s => (document.querySelector(s) as Instrument | null)?.flows?.state === 'ready'
    || (document.querySelector(s) as Instrument | null)?.flows?.state === 'running', selector);

// Starts sound with a real click. CI provides a virtual output device; WebKit's
// Linux build and machines without audio may not start, and those runs skip.
async function startAudio(page: Page, click: () => Promise<void>, state: () => Promise<string>, browserName: string) {
  await click();
  const reached = await expect.poll(state, { timeout: 15_000 }).toBe('running').then(() => 'running', async () => state());
  if (process.env.CI && browserName !== 'webkit') expect(reached, `${browserName} audio state`).toBe('running');
  else test.skip(reached !== 'running', `${browserName} did not start audio here (${reached})`);
}
const peakOf = (page: Page, selector: string) => page.evaluate(s => (document.querySelector(s) as Instrument).flows.peak as number, selector);

test.describe('embed', () => {
  test('a page without isolation loads every layer cross-origin', async ({ page }, info) => {
    const errors = await visit(page, '/examples/minimal-blog.html');
    await ready(page, '#demo');
    expect(await page.evaluate(() => globalThis.crossOriginIsolated)).toBe(false);
    expect(await page.evaluate(() => typeof SharedArrayBuffer === 'undefined' || !globalThis.crossOriginIsolated)).toBe(true);
    // Nothing registers a service worker on the host site.
    expect(await page.evaluate(async () => navigator.serviceWorker ? (await navigator.serviceWorker.getRegistrations()).length : 0)).toBe(0);
    const params = await page.evaluate(() => (document.querySelector('#demo') as Instrument).flows.params().map((p: { key: string }) => p.key));
    expect(params).toEqual(expect.arrayContaining(['filtered.cutoff', 'filtered.resonance', 'amp.R']));
    await expect(page.locator('#demo').getByRole('slider', { name: 'Cutoff' })).toBeVisible();
    await expect(page.locator('#demo').getByRole('button', { name: 'Tap to play' })).toBeVisible();
    await page.screenshot({ path: shots(`${info.project.name}-blog`), fullPage: true });
    expect(errors).toEqual([]);
  });

  test('plays a note after a click and its keys follow the note', async ({ page, browserName }) => {
    await visit(page, '/examples/minimal-blog.html');
    await ready(page, '#demo');
    const demo = page.locator('#demo');
    await startAudio(page, () => demo.getByRole('button', { name: 'Tap to play' }).click(),
      () => page.evaluate(() => (document.querySelector('#demo') as Instrument).flows.state), browserName);
    await expect(demo.getByRole('button', { name: 'Tap to play' })).toBeHidden();
    const key = demo.getByRole('button', { name: 'C4', exact: true });
    await key.scrollIntoViewIfNeeded();
    const box = (await key.boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height * 0.85);
    await page.mouse.down();
    await expect(key).toHaveAttribute('aria-pressed', 'true');
    await expect.poll(() => peakOf(page, '#demo'), { timeout: 10_000 }).toBeGreaterThan(0.01);
    await page.mouse.up();
    await expect(key).toHaveAttribute('aria-pressed', 'false');
  });

  test('works with the keyboard and a screen reader', async ({ page }) => {
    await visit(page, '/examples/minimal-blog.html');
    await ready(page, '#demo');
    const demo = page.locator('#demo');
    // One tab stop for the keyboard; arrows move, Space plays.
    const middleC = demo.getByRole('button', { name: 'C4', exact: true });
    await middleC.focus();
    await page.keyboard.press('ArrowRight');
    const cSharp = demo.getByRole('button', { name: 'C#4', exact: true });
    await expect(cSharp).toBeFocused();
    await page.keyboard.down('Space');
    await expect(cSharp).toHaveAttribute('aria-pressed', 'true');
    expect(await page.evaluate(() => (document.querySelector('#demo') as Instrument).flows.heldNotes())).toEqual([61]);
    await page.keyboard.up('Space');
    await expect(cSharp).toHaveAttribute('aria-pressed', 'false');
    // Knobs are sliders with readable values.
    const cutoff = demo.getByRole('slider', { name: 'Cutoff' });
    await expect(cutoff).toHaveAttribute('aria-valuetext', '2.4 kHz');
    await cutoff.focus();
    await page.keyboard.press('PageUp');
    const raised = Number(await cutoff.getAttribute('aria-valuenow'));
    expect(raised).toBeGreaterThan(2400);
    expect(await page.evaluate(() => (document.querySelector('#demo') as Instrument).flows.getParam('filtered.cutoff'))).toBe(raised);
    await page.keyboard.press('Delete');
    await expect(cutoff).toHaveAttribute('aria-valuenow', '2400');
  });

  test('instruments on one page share a context but not their sound', async ({ page, browserName }) => {
    const errors = await visit(page, '/fixtures/multi.html');
    await ready(page, '#a');
    await ready(page, '#b');
    // One gesture unlocks sound for both.
    await startAudio(page, () => page.locator('#a').getByRole('button', { name: 'Tap to play' }).click(),
      () => page.evaluate(() => (document.querySelector('#b') as Instrument).flows.state), browserName);
    expect(await page.evaluate(() => {
      const [a, b] = ['#a', '#b'].map(s => (document.querySelector(s) as Instrument).flows);
      return a.context === b.context && a.output !== b.output;
    })).toBe(true);
    await page.evaluate(() => { (document.querySelector('#a') as Instrument).flows.noteOn('C4'); });
    await expect.poll(() => peakOf(page, '#a'), { timeout: 10_000 }).toBeGreaterThan(0.01);
    await page.waitForTimeout(300);
    expect(await peakOf(page, '#b')).toBe(0);
    expect(await page.evaluate(() => (document.querySelector('#b') as Instrument).flows.heldNotes())).toEqual([]);
    // Parameters are per instrument.
    await page.evaluate(() => (document.querySelector('#a') as Instrument).flows.setParam('amp.R', 1.5));
    expect(await page.evaluate(() => (document.querySelector('#b') as Instrument).flows.getParam('amp.R'))).toBeCloseTo(0.09);
    await page.evaluate(() => { (document.querySelector('#a') as Instrument).flows.allNotesOff(); });
    expect(errors).toEqual([]);
  });

  test('custom properties, parts and slots restyle the instrument', async ({ page }, info) => {
    await visit(page, '/fixtures/style.html');
    await ready(page, '#plain');
    await ready(page, '#styled');
    // Pixel check on real screenshots: the reference keeps its look, the styled copy changes.
    const colorAt = async (selector: string, part: string, label: string) => {
      const key = page.locator(selector).getByRole('button', { name: label, exact: true });
      await key.scrollIntoViewIfNeeded();
      const box = (await key.boundingBox())!;
      const shot = await page.screenshot({ clip: { x: box.x + box.width / 2 - 2, y: box.y + box.height * (part === 'white' ? 0.85 : 0.4) - 2, width: 4, height: 4 } });
      return page.evaluate(async (base64) => {
        const image = await createImageBitmap(await (await fetch(`data:image/png;base64,${base64}`)).blob());
        const canvas = new OffscreenCanvas(image.width, image.height);
        const context = canvas.getContext('2d')!;
        context.drawImage(image, 0, 0);
        return Array.from(context.getImageData(1, 1, 1, 1).data.slice(0, 3));
      }, shot.toString('base64'));
    };
    const [r, g, b] = await colorAt('#styled', 'white', 'D3');
    expect(r).toBeGreaterThan(200); expect(g).toBeLessThan(60); expect(b).toBeLessThan(60);
    const black = await colorAt('#styled', 'black', 'C#3');
    expect(black[2]).toBeGreaterThan(200); expect(black[0]).toBeLessThan(60);
    const plain = await colorAt('#plain', 'white', 'D3');
    expect(plain[0] - plain[1]).toBeLessThan(30);
    // Variables, parts and slots in computed style.
    const styles = await page.evaluate(() => {
      const read = (selector: string) => {
        const root = (document.querySelector(selector) as Instrument).shadowRoot!;
        const panel = getComputedStyle(root.querySelector('[part="panel"]')!);
        const title = getComputedStyle(root.querySelector('[part="title"]')!);
        return { radius: panel.borderTopLeftRadius, background: panel.backgroundColor, font: panel.fontFamily, transform: title.textTransform };
      };
      return { plain: read('#plain'), styled: read('#styled') };
    });
    expect(styles.styled).toMatchObject({ radius: '30px', background: 'rgb(250, 240, 200)', transform: 'uppercase' });
    expect(styles.styled.font).toContain('Georgia');
    expect(styles.plain.background).toBe('rgba(0, 0, 0, 0)');
    expect(styles.plain.font).toBe(await page.evaluate(() => getComputedStyle(document.body).fontFamily));
    await expect(page.locator('#styled').getByRole('button', { name: 'Start' })).toBeVisible();
    await page.screenshot({ path: shots(`${info.project.name}-style`) });
  });

  for (const colorScheme of ['light', 'dark'] as const) {
    test(`built-in themes follow the ${colorScheme} color scheme`, async ({ page }, info) => {
      await page.emulateMedia({ colorScheme });
      await visit(page, '/fixtures/themes.html');
      for (const id of ['#none', '#glass', '#studio']) await ready(page, id);
      const surfaces = await page.evaluate(() => ['#none', '#glass', '#studio'].map(id =>
        getComputedStyle((document.querySelector(id) as Instrument).shadowRoot!.querySelector('[part="panel"]')!).backgroundColor));
      expect(surfaces[0]).toBe('rgba(0, 0, 0, 0)');
      expect(surfaces[2]).toBe(colorScheme === 'light' ? 'rgb(243, 242, 239)' : 'rgb(23, 25, 28)');
      await page.screenshot({ path: shots(`${info.project.name}-themes-${colorScheme}`) });
    });
  }

  test('the liquid glass example uses a custom layout and a classic script', async ({ page }, info) => {
    const errors = await visit(page, '/examples/liquid-glass.html');
    await ready(page, '#play');
    expect(await page.evaluate(() => typeof window.Flows?.create)).toBe('function');
    const play = page.locator('#play');
    for (const name of ['Shimmer', 'Ring', 'Swell', 'Tail']) await expect(play.getByRole('slider', { name })).toBeVisible();
    await expect(play.getByRole('button', { name: 'Touch to wake it' })).toBeVisible();
    // The built-in layout gives way to the page's own.
    expect(await page.evaluate(() => (document.querySelector('#play') as Instrument).hasAttribute('custom-layout'))).toBe(true);
    const filter = await page.evaluate(() => {
      const panel = (document.querySelector('#play') as Instrument).shadowRoot!.querySelector('[part="panel"]')!;
      const style = getComputedStyle(panel);
      return style.backdropFilter || style.getPropertyValue('-webkit-backdrop-filter');
    });
    expect(filter).toContain('blur(22px)');
    await page.screenshot({ path: shots(`${info.project.name}-glass`), fullPage: true });
    expect(errors).toEqual([]);
  });

  test('the dark portfolio drives the engine from its own controls', async ({ page, browserName }, info) => {
    const errors = await visit(page, '/examples/dark-portfolio.html');
    await page.waitForFunction(() => window.flows?.state === 'ready');
    await expect(page.locator('.sliders label')).toHaveCount(3);
    await expect(page.locator('.pad')).toHaveCount(8);
    await page.screenshot({ path: shots(`${info.project.name}-dark`), fullPage: true });
    await startAudio(page, () => page.getByRole('button', { name: 'Turn sound on' }).click(), () => page.evaluate(() => window.flows.state), browserName);
    const pad = page.getByRole('button', { name: 'A1' });
    await pad.hover();
    await page.mouse.down();
    await expect(pad).toHaveAttribute('aria-pressed', 'true');
    await expect.poll(() => page.evaluate(() => window.flows.peak), { timeout: 10_000 }).toBeGreaterThan(0.01);
    await page.mouse.up();
    await expect(pad).toHaveAttribute('aria-pressed', 'false');
    expect(errors).toEqual([]);
  });

  test('a strict content security policy falls back to the hosted worklet', async ({ page, browserName }) => {
    const errors = await visit(page, '/fixtures/csp.html');
    await ready(page, '#csp');
    await startAudio(page, () => page.locator('#csp').getByRole('button', { name: 'Tap to play' }).click(),
      () => page.evaluate(() => (document.querySelector('#csp') as Instrument).flows.state), browserName);
    await page.evaluate(() => { (document.querySelector('#csp') as Instrument).flows.noteOn('A4'); });
    await expect.poll(() => peakOf(page, '#csp'), { timeout: 10_000 }).toBeGreaterThan(0.01);
    expect(errors).toEqual([]);
  });

  test('moving an element keeps it; removing and re-adding starts a fresh engine', async ({ page }) => {
    await visit(page, '/fixtures/multi.html');
    await ready(page, '#a');
    const result = await page.evaluate(async () => {
      const element = document.querySelector('#a') as Instrument;
      const first = element.flows;
      document.body.append(element); // a move reconnects in the same task
      const kept = element.flows === first && first.state !== 'disposed';
      element.remove();
      await new Promise(resolve => setTimeout(resolve, 0));
      const disposed = first.state === 'disposed';
      document.body.append(element);
      const second = await element.ready;
      return { kept, disposed, fresh: second !== first && second.state === 'ready' };
    });
    expect(result).toEqual({ kept: true, disposed: true, fresh: true });
  });

  test('reports compile errors with line numbers', async ({ page }) => {
    await visit(page, '/fixtures/multi.html');
    await ready(page, '#a');
    const result = await page.evaluate(async () => {
      const element = document.querySelector('#a') as Instrument;
      const failed = new Promise(resolve => element.addEventListener('flows-error', event => resolve((event as CustomEvent).detail.error.diagnostics), { once: true }));
      element.setAttribute('source', 'import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return missing\n');
      return failed;
    }) as { line: number; message: string }[];
    expect(result[0].line).toBe(3);
    await expect(page.locator('#a').getByRole('alert')).toContainText('line 3');
  });
  test('shows the source with the instrument, and edits to a number play in place', async ({ page }, info) => {
    const errors = await visit(page, '/fixtures/code.html');
    await ready(page, '#default');
    const editor = page.locator('#default').getByRole('textbox', { name: 'Flows source code' });
    await expect(editor).toBeVisible();
    const source = await editor.inputValue();
    expect(source).toContain('Inst.sigstream');
    // Every live number in the code is a handle on its parameter.
    const handles = await page.locator('#default').locator('[data-param]').count();
    expect(handles).toBeGreaterThan(3);
    // Typing a new release time recompiles without replacing the engine.
    await page.evaluate(() => { window.flows = (document.querySelector('#default') as Instrument).flows; });
    const R = await page.evaluate(() => (document.querySelector('#default') as Instrument).flows.param('amp.R'));
    const literal = source.slice(R.from, R.to);
    await editor.focus();
    await page.evaluate(([from, to]) => {
      const input = (document.querySelector('#default') as Instrument).shadowRoot!.querySelector('flows-code')!.shadowRoot!.querySelector('textarea')!;
      input.setSelectionRange(from, to);
    }, [R.from, R.to]);
    await page.keyboard.type('1.5s');
    await expect.poll(() => page.evaluate(() => (document.querySelector('#default') as Instrument).flows.getParam('amp.R'))).toBe(1.5);
    expect(await page.evaluate(() => (document.querySelector('#default') as Instrument).flows === window.flows)).toBe(true);
    expect(literal).not.toBe('1.5s');
    await page.locator('#default').screenshot({ path: shots(`${info.project.name}-code`) });
    expect(errors).toEqual([]);
  });

  test('dragging a number in the code changes the sound and a knob rewrites the code', async ({ page }) => {
    await visit(page, '/fixtures/code.html');
    await ready(page, '#custom');
    const custom = page.locator('#custom');
    const editor = custom.getByRole('textbox', { name: 'Flows source code' });
    const cutoff = () => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.getParam('cutoff') as number);
    expect(await cutoff()).toBe(1200);
    const number = custom.locator('[data-param]', { hasText: '1.2khz' });
    await number.scrollIntoViewIfNeeded();
    const box = (await number.boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2 + 60, box.y + box.height / 2, { steps: 6 });
    await page.mouse.up();
    const dragged = await cutoff();
    expect(dragged).toBeGreaterThan(1200);
    await expect(editor).not.toHaveValue(/cutoff: 1\.2khz/);
    await expect(editor).toHaveValue(/cutoff: [\d.]+khz/);
    // A knob turned with the keyboard writes its value into the code.
    const knob = custom.getByRole('slider', { name: 'Cutoff' });
    await knob.focus();
    await page.keyboard.press('End');
    await expect(editor).toHaveValue(/cutoff: 20khz/);
  });

  test('a mistake in the code is marked and the last good version keeps playing', async ({ page }) => {
    await visit(page, '/fixtures/code.html');
    await ready(page, '#custom');
    const custom = page.locator('#custom');
    const editor = custom.getByRole('textbox', { name: 'Flows source code' });
    const original = await editor.inputValue();
    await editor.fill(original.replace('return tone * amp * key.velocity', 'return tone * missing'));
    await expect(custom.getByRole('status').filter({ hasText: 'Line' })).toContainText(/Line \d+: /);
    await expect(custom.locator('[part~="code-line-bad"]')).toHaveCount(1);
    expect(await page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.source)).toBe(original);
    expect(await page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.state)).not.toBe('error');
    await editor.fill(original);
    await expect(custom.locator('[part~="code-problem"]')).toHaveCount(0);
  });

  test('editing a live number keeps a held note sounding', async ({ page, browserName }) => {
    await visit(page, '/fixtures/code.html');
    await ready(page, '#custom');
    await startAudio(page, () => page.locator('#custom').getByRole('button', { name: 'C3', exact: true }).click(),
      () => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.state), browserName);
    await page.evaluate(() => { (document.querySelector('#custom') as Instrument).flows.noteOn('A3'); });
    await expect.poll(() => peakOf(page, '#custom'), { timeout: 10_000 }).toBeGreaterThan(0.01);
    const loads = await page.evaluate(async () => {
      const flows = (document.querySelector('#custom') as Instrument).flows;
      const code = document.querySelector('#custom flows-code') as HTMLElement & { value: string; apply(): Promise<boolean> };
      const seen: boolean[] = [];
      flows.on('load', ({ hot }: { hot: boolean }) => seen.push(hot));
      code.value = code.value.replace('S: 0.7', 'S: 0.4');
      await new Promise(resolve => setTimeout(resolve, 50));
      return { seen, held: flows.heldNotes() };
    });
    expect(loads.seen).toEqual([true]);
    expect(loads.held).toContain(57);
    await expect.poll(() => peakOf(page, '#custom'), { timeout: 5_000 }).toBeGreaterThan(0.005);
  });

  test('Reset brings back the loaded code, and a knob scrolls its number into view', async ({ page }) => {
    await visit(page, '/fixtures/code.html');
    await ready(page, '#custom');
    const custom = page.locator('#custom');
    const code = custom.locator('flows-code');
    const editor = custom.getByRole('textbox', { name: 'Flows source code' });
    const reset = custom.getByRole('button', { name: 'Reset' });
    const cutoff = () => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.getParam('cutoff') as number);
    const original = await editor.inputValue();
    await expect(reset).toBeHidden();
    // A knob and an edit both change the code; Reset undoes both.
    const knob = custom.getByRole('slider', { name: 'Cutoff' });
    await knob.focus();
    await page.keyboard.press('End');
    await expect(editor).toHaveValue(/cutoff: 20khz/);
    await editor.fill(original.replace('S: 0.7', 'S: 0.3').replace('cutoff: 1.2khz', 'cutoff: 20khz'));
    await expect.poll(() => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.source as string)).toContain('S: 0.3');
    await expect(reset).toBeVisible();
    await expect(code).toHaveAttribute('modified', '');
    await reset.click();
    await expect(editor).toHaveValue(original);
    await expect.poll(() => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.source as string)).toBe(original);
    await expect.poll(cutoff).toBe(1200);
    await expect(knob).toHaveAttribute('aria-valuenow', '1200');
    await expect(reset).toBeHidden();
    await expect(code).not.toHaveAttribute('modified', '');
    // In a short box, a knob brings its number into view.
    await code.evaluate(element => { element.style.setProperty('--flows-code-max-height', '64px'); });
    await knob.focus();
    await page.keyboard.press('PageUp');
    await expect.poll(() => code.evaluate(element => {
      const box = element.shadowRoot!.querySelector('.code')!.getBoundingClientRect();
      const number = [...element.shadowRoot!.querySelectorAll('.param')].find(p => p.textContent!.includes('khz'))!.getBoundingClientRect();
      return number.top >= box.top && number.bottom <= box.bottom;
    })).toBe(true);
  });

  test('the computer keys play while a knob or a number in the code is dragged, and never reach the page', async ({ page }) => {
    await visit(page, '/fixtures/code.html');
    await ready(page, '#custom');
    const custom = page.locator('#custom');
    const held = () => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.heldNotes() as number[]);
    const cutoff = () => page.evaluate(() => (document.querySelector('#custom') as Instrument).flows.getParam('cutoff') as number);
    // The page has single-key shortcuts of its own, as a blog might (G then H: home).
    await page.evaluate(() => {
      (window as any).shortcuts = [];
      document.addEventListener('keydown', event => (window as any).shortcuts.push(event.key));
    });
    const c3 = custom.getByRole('button', { name: 'C3', exact: true });
    const hint = c3.locator('[part~="key-hint"]');
    await expect(c3).toHaveAttribute('aria-keyshortcuts', 'A');
    await expect(hint).toHaveCSS('opacity', '0');
    // A click arms the computer keys: the letters show and play.
    await c3.click();
    await expect(hint).toHaveCSS('opacity', '1');
    await page.keyboard.down('g');
    expect(await held()).toEqual([55]);
    // One hand holds G while the other turns the knob: the note and the keys stay.
    const knob = custom.getByRole('slider', { name: 'Cutoff' });
    const box = (await knob.boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2 - 40, { steps: 5 });
    await page.mouse.up();
    expect(await cutoff()).toBeGreaterThan(1200);
    expect(await held()).toEqual([55]);
    await expect(hint).toHaveCSS('opacity', '1');
    await page.keyboard.up('g');
    // Still armed after the knob took focus: H plays, and no letter turns the knob.
    await page.keyboard.down('h');
    expect(await held()).toEqual([57]);
    await page.keyboard.up('h');
    // ...and after dragging a number in the code.
    const number = custom.locator('[data-param]').first();
    const at = (await number.boundingBox())!;
    await page.mouse.move(at.x + at.width / 2, at.y + at.height / 2);
    await page.mouse.down();
    await page.mouse.move(at.x + at.width / 2 + 30, at.y + at.height / 2, { steps: 4 });
    await page.mouse.up();
    await page.keyboard.down('a');
    expect(await held()).toEqual([48]);
    await page.keyboard.up('a');
    // Escape disarms; it is the keyboard's (a second Escape would reach the page).
    await page.keyboard.press('Escape');
    await expect(hint).toHaveCSS('opacity', '0');
    await page.keyboard.press('a');
    expect(await held()).toEqual([]);
    // Typing in the code stays in the editor, and a click on a key then the caret in the code disarms.
    await c3.click();
    await expect(hint).toHaveCSS('opacity', '1');
    const editor = custom.getByRole('textbox', { name: 'Flows source code' });
    await editor.focus();
    await expect(hint).toHaveCSS('opacity', '0');
    await page.keyboard.type('gh/?');
    expect(await held()).toEqual([]);
    // A click outside the instrument disarms too.
    await c3.click();
    await page.mouse.click(5, 5);
    await expect(hint).toHaveCSS('opacity', '0');
    expect(await page.evaluate(() => (window as any).shortcuts)).toEqual(['a']);
  });
});
