import assert from 'node:assert/strict';
import { build } from 'esbuild';

// client.ts imports the JUCE transport; bundle it so the test runs in Node.
const { outputFiles } = await build({
  entryPoints: [new URL('../src/client.ts', import.meta.url).pathname],
  bundle: true, write: false, format: 'esm', platform: 'neutral', logLevel: 'silent',
});
globalThis.window = {};
const client = await import(`data:text/javascript;base64,${Buffer.from(outputFiles[0].text).toString('base64')}`);
const { assembleState, setTransport, request, subscribe, hostCapabilities, hostAvailable } = client;

const snapshot = (revision, extra = {}) => ({
  sessionId: 's', document: { id: 'a.flows', path: 'a.flows', text: `v${revision}`, revision },
  files: [], compile: { status: 'ready' }, audio: {}, ai: {},
  capabilities: { host: 'web', fileDialogs: 'browser' }, ...extra,
});

// No transport outside a host.
assert.equal(hostAvailable(), false);
await assert.rejects(request('session.get'), /native app or the web app/);

// Slices before a full snapshot are dropped; the UI asks for one.
assert.equal(assembleState({ protocol: 1, full: false, slices: { audio: { running: true } } }), null);
// A full event replaces state; later slices merge; null removes a key.
const full = assembleState({ protocol: 1, full: true, slices: snapshot(1, { samples: [{ path: 'x.wav' }] }) });
assert.equal(full.document.revision, 1);
const merged = assembleState({ protocol: 1, full: false, slices: { document: snapshot(2).document, samples: null } });
assert.equal(merged.document.revision, 2);
assert.equal(merged.compile.status, 'ready');
assert.equal('samples' in merged, false);
assert.equal(hostCapabilities().fileDialogs, 'browser');
// Events from a newer protocol are ignored instead of misread.
assert.equal(assembleState({ protocol: 99, full: true, slices: snapshot(3) }), null);
// Legacy full snapshots still work.
assert.equal(assembleState(snapshot(4)).document.revision, 4);

// Slices are relative to the previous event, so a request result never
// becomes their base: merging onto it could keep fields the event omitted.
const listeners = new Map();
const calls = [];
setTransport({
  kind: 'web',
  call: async (method, params, timeout) => {
    calls.push(method);
    assert.equal(timeout, method.startsWith('ai.') ? 180000 : 30000);
    return method === 'session.get' ? snapshot(5) : { ok: true, params };
  },
  listen: (event, callback) => { listeners.set(event, callback); return () => listeners.delete(event); },
});
assert.equal(hostAvailable(), true);
await request('session.get');
calls.length = 0;
const received = [];
const stop = subscribe('flowsState', value => received.push(value));
// Without earlier events the slice is dropped and the client asks the host
// to restart its events in full (session.get), once while that is pending.
listeners.get('flowsState')({ protocol: 1, full: false, slices: { audio: { running: true } } });
listeners.get('flowsState')({ protocol: 1, full: false, slices: { audio: { running: false } } });
assert.equal(received.length, 0);
assert.deepEqual(calls, ['session.get']);
await new Promise(resolve => setTimeout(resolve, 0));
listeners.get('flowsState')({ protocol: 1, full: true, slices: snapshot(6, { audio: { running: false } }) });
listeners.get('flowsState')({ protocol: 1, full: false, slices: { audio: { running: true } } });
assert.equal(received.length, 2);
assert.equal(received[1].document.revision, 6);
assert.equal(received[1].audio.running, true);
// A later request result does not replace the event state.
await request('session.get');
listeners.get('flowsState')({ protocol: 1, full: false, slices: { compile: { status: 'working' } } });
assert.equal(received[2].document.revision, 6);
assert.equal(received[2].compile.status, 'working');
stop();
assert.equal(listeners.has('flowsState'), false);
console.log('Client: slices assemble onto event state only, a missing base restarts events in full, protocol versions are checked and transports are swappable.');
