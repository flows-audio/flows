import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/keymap.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { editorShortcut, textSizeShortcut, shortcutLabel } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);

const key = (code, options = {}) => ({ key: options.key ?? code.replace(/^Key|^Digit/, '').toLowerCase(), code, metaKey: false, ctrlKey: false, altKey: false, shiftKey: false, ...options });
const mac = { mac: true, web: false }, macWeb = { mac: true, web: true }, pc = { mac: false, web: false }, pcWeb = { mac: false, web: true };

// Native keeps the platform shortcuts.
assert.equal(editorShortcut(key('KeyW', { metaKey: true }), mac), 'close');
assert.equal(editorShortcut(key('KeyW', { ctrlKey: true }), pc), 'close');
assert.equal(editorShortcut(key('Digit2', { metaKey: true }), mac), 'focus-second');
assert.equal(editorShortcut(key('Tab', { key: 'Tab', ctrlKey: true }), mac), 'next-tab');
assert.equal(editorShortcut(key('Tab', { key: 'Tab', ctrlKey: true, shiftKey: true }), pc), 'previous-tab');
assert.equal(editorShortcut(key('PageUp', { key: 'PageUp', metaKey: true }), mac), 'previous-tab');

// The browser never gets ⌘W, ⌘1/⌘2 or Ctrl+Tab; Option/Alt takes over.
assert.equal(editorShortcut(key('KeyW', { metaKey: true }), macWeb), undefined);
assert.equal(editorShortcut(key('Digit1', { ctrlKey: true }), pcWeb), undefined);
assert.equal(editorShortcut(key('Tab', { key: 'Tab', ctrlKey: true }), pcWeb), undefined);
assert.equal(editorShortcut(key('KeyW', { altKey: true, key: '∑' }), macWeb), 'close');
assert.equal(editorShortcut(key('Digit2', { altKey: true, key: '™' }), macWeb), 'focus-second');
assert.equal(editorShortcut(key('BracketRight', { altKey: true, key: '‘' }), macWeb), 'next-tab');
assert.equal(editorShortcut(key('BracketLeft', { altKey: true }), pcWeb), 'previous-tab');
assert.equal(editorShortcut(key('KeyW', { altKey: true, metaKey: true }), macWeb), undefined);
// Option+Shift still types characters (’ and ” on a US Mac layout).
assert.equal(editorShortcut(key('BracketRight', { altKey: true, shiftKey: true, key: '’' }), macWeb), undefined);
assert.equal(editorShortcut(key('BracketLeft', { altKey: true, shiftKey: true, key: '”' }), macWeb), undefined);
assert.equal(editorShortcut(key('PageDown', { altKey: true, shiftKey: true, key: 'PageDown' }), pcWeb), undefined);

// Split is the same everywhere.
assert.equal(editorShortcut(key('Backslash', { metaKey: true }), macWeb), 'split');
assert.equal(editorShortcut(key('Backslash', { ctrlKey: true }), pc), 'split');

// Composition never triggers shortcuts.
assert.equal(editorShortcut(key('KeyW', { metaKey: true, isComposing: true }), mac), undefined);

// Text size: native only; the browser keeps its zoom.
assert.equal(textSizeShortcut(key('Equal', { key: '=', metaKey: true }), { web: false }), 'larger');
assert.equal(textSizeShortcut(key('Minus', { key: '-', ctrlKey: true }), { web: false }), 'smaller');
assert.equal(textSizeShortcut(key('Digit0', { key: '0', metaKey: true }), { web: false }), 'reset');
assert.equal(textSizeShortcut(key('Equal', { key: '=', metaKey: true }), { web: true }), undefined);

assert.equal(shortcutLabel('close', mac), '⌘W');
assert.equal(shortcutLabel('close', macWeb), '⌥W');
assert.equal(shortcutLabel('next-tab', pcWeb), 'Alt+]');
console.log('Keymap: native and browser shortcuts resolve without browser-reserved keys.');
