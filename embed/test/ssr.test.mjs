// Server-side rendering: importing any entry point in Node (no window, no
// customElements, no AudioContext) must not throw or touch the network.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const dist = join(dirname(fileURLToPath(import.meta.url)), '../dist');
const fetched = [];
globalThis.fetch = async (url) => { fetched.push(String(url)); throw new Error('no network during SSR'); };

const all = await import(join(dist, 'flows-embed.js'));
const core = await import(join(dist, 'flows-embed-core.js'));
const elements = await import(join(dist, 'flows-embed-elements.js'));
assert.equal(typeof all.Flows.create, 'function');
assert.equal(all.Flows, core.Flows, 'one engine module shared between entry points');
assert.equal(typeof elements.FlowsInstrumentElement, 'function');
assert.equal(all.Flows.supported, false, 'no audio in Node');
assert.ok(all.PRESETS.includes('soft-keys'));
assert.equal(all.Flows.context, null);

// The classic script runs in a bare global scope.
const context = vm.createContext({ console });
vm.runInContext(readFileSync(join(dist, 'flows-embed.iife.js'), 'utf8'), context);
vm.runInContext(readFileSync(join(dist, 'flows-embed-worklet.js'), 'utf8').replace(/registerProcessor\(/, 'void ('), vm.createContext({ AudioWorkletProcessor: class {} }));
assert.deepEqual(fetched, [], 'importing must not fetch');
console.log('SSR imports passed.');
