#!/usr/bin/env node
// Renders every audio test case (test/cases) with the embed engine, through
// the same JavaScript wrapper the AudioWorklet uses, and compares the result
// with the native `flows` CLI. Both compile the same source with the same
// compiler and engine; output must agree to 2 PCM16 steps, the tolerance of
// the existing native/WebAssembly parity test (test/tools/render_parity.py).
//
// Usage: node embed/test/render-parity.mjs --native build/flows [--wasm build-web/embed/flows-embed.wasm]
import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '../..');
const argument = (name, fallback) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : fallback; };
const native = resolve(argument('--native', join(repo, 'build/flows')));
const wasmPath = resolve(argument('--wasm', join(repo, 'build-web/embed/flows-embed.wasm')));
const distCore = join(repo, 'embed/dist/flows-embed-core.js');
for (const required of [native, wasmPath, distCore])
  if (!existsSync(required)) { console.error(`Missing ${required}`); process.exit(1); }

const { FlowsWasm } = await import(distCore);
const module = new WebAssembly.Module(readFileSync(wasmPath));
const AUDIO_STEPS = 2;

const sampleIndex = (seconds, rate) => Math.floor(seconds * rate + 0.5);
function readPcm16(path) {
  const data = readFileSync(path);
  for (let offset = 12; offset + 8 <= data.length;) {
    const size = data.readUInt32LE(offset + 4);
    if (data.toString('latin1', offset, offset + 4) === 'data') {
      const values = new Int16Array(size / 2);
      for (let i = 0; i < values.length; i++) values[i] = data.readInt16LE(offset + 8 + i * 2);
      return values;
    }
    offset += 8 + size + (size & 1);
  }
  throw new Error(`${path} has no data chunk`);
}
const pcm = (value) => Math.round(Math.max(-1, Math.min(1, value)) * 32767);

function renderEmbed(source, sourcePath, kase) {
  const { sample_rate: rate, block_size: block, duration_seconds: duration } = kase.render;
  const wasm = new FlowsWasm(module);
  const { handle, info } = wasm.compile(source);
  if (!info.ok) throw new Error(`${kase.id}: ${JSON.stringify(info.diagnostics)}`);
  (info.samples ?? []).forEach((path, index) => wasm.sample(handle, index, readFileSync(resolve(assets, path))));
  // Offline renders use exact note boundaries (no 3 ms smoothing) at 138 BPM.
  wasm.prepare(handle, rate, 0, 138);
  const ids = new Map();
  for (const event of kase.events) {
    const id = ids.get(event.note_id) ?? ids.size + 1;
    ids.set(event.note_id, id);
    const at = sampleIndex(event.time_seconds, rate);
    const ok = event.type === 'note_on' ? wasm.noteOn(handle, id, event.pitch, event.velocity ?? 0, at) : wasm.noteOff(handle, id, at);
    if (!ok) throw new Error(`${kase.id}: event rejected ${JSON.stringify(event)}`);
  }
  const frames = Math.floor(duration * rate + 0.5);
  const out = [];
  for (let start = 0; start < frames; start += block) {
    const count = Math.min(block, frames - start);
    if (wasm.render(handle, count) < 0) throw new Error(`${kase.id}: engine rejected block at ${start}`);
    const [left, right] = wasm.output(handle, count);
    for (let i = 0; i < count; i++) {
      out.push(pcm(left[i]));
      if (info.channels === 2) out.push(pcm(right[i]));
    }
  }
  wasm.free(handle);
  return out;
}

function renderNative(sourcePath, kase, scratch) {
  const { sample_rate: rate, block_size: block, duration_seconds: duration } = kase.render;
  const ids = new Map();
  const lines = kase.events.map((event) => {
    const id = ids.get(event.note_id) ?? ids.size + 1;
    ids.set(event.note_id, id);
    return `${sampleIndex(event.time_seconds, rate)} ${event.type} ${id} ${event.pitch} ${event.velocity ?? 0}\n`;
  });
  const events = join(scratch, `${kase.id.replace(/\W+/g, '-')}.tsv`);
  writeFileSync(events, lines.join(''));
  const output = join(scratch, kase.id.replace(/\W+/g, '-'));
  execFileSync(native, ['render', sourcePath, '--events', events, '--output', output, '--sample-rate', String(rate),
    '--block-size', String(block), '--duration', String(duration), '--asset-root', assets], { stdio: 'pipe' });
  return readPcm16(join(output, 'output.wav'));
}

// The audio test cases, plus every preset the embed ships (stereo, sampler,
// mono glide) played with the same short phrase.
const cases = readdirSync(join(repo, 'test/cases')).filter((name) => name.endsWith('.json')).sort()
  .map((name) => { const path = join(repo, 'test/cases', name); const kase = JSON.parse(readFileSync(path, 'utf8')); return { kase, sourcePath: resolve(dirname(path), kase.preset) }; });
const phrase = [[0, 'note_on', 'a', 60, 0.9], [0.15, 'note_on', 'b', 64, 0.7], [0.3, 'note_on', 'c', 67, 0.8],
  [0.7, 'note_off', 'b', 64], [0.8, 'note_off', 'a', 60], [0.9, 'note_off', 'c', 67]]
  .map(([time_seconds, type, note_id, pitch, velocity]) => ({ time_seconds, type, note_id, pitch, velocity }));
for (const folder of [join(repo, 'test/presets'), join(repo, 'embed/presets')])
  for (const name of readdirSync(folder).filter((n) => n.endsWith('.flows')).sort())
    cases.push({ sourcePath: join(folder, name), kase: { id: `preset ${name.replace(/\.flows$/, '')}`, events: phrase, render: { sample_rate: 44100, block_size: 128, duration_seconds: 1.6 } } });
const scratch = mkdtempSync(join(tmpdir(), 'flows-embed-parity-'));
// Sample presets refer to Samples/<file>, as in the app workspace and in embed/dist/presets.
const assets = join(scratch, 'assets');
mkdirSync(join(assets, 'Samples'), { recursive: true });
for (const name of ['flows-kick.wav', 'flows-stereo.wav']) cpSync(join(repo, 'test/fixtures/samples', name), join(assets, 'Samples', name));
let failed = 0;
try {
  for (const { kase, sourcePath } of cases) {
    const expected = renderNative(sourcePath, kase, scratch);
    const actual = renderEmbed(readFileSync(sourcePath, 'utf8'), sourcePath, kase);
    let worst = expected.length === actual.length ? 0 : Infinity;
    for (let i = 0; i < Math.min(expected.length, actual.length); i++) worst = Math.max(worst, Math.abs(expected[i] - actual[i]));
    const peak = expected.reduce((a, v) => Math.max(a, Math.abs(v)), 0);
    const ok = worst <= AUDIO_STEPS && peak > 0;
    if (!ok) failed++;
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${kase.id}: ${actual.length} samples (native ${expected.length}), max difference ${worst} PCM16 step(s), peak ${peak}`);
  }
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
if (failed) { console.error(`${failed} of ${cases.length} cases differ between the embed and native engines.`); process.exit(1); }
console.log(`Embed render parity passed for ${cases.length} cases.`);
