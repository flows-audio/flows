// Parameter keys, knob mapping, note names and the WebAssembly wrapper in Node.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { FlowsWasm, findParameter, formatValue, fromPosition, noteName, notePitch, toPosition, defaultControls } = await import(join(here, '../dist/flows-embed-core.js'));

assert.equal(notePitch('C4'), 60);
assert.equal(notePitch('A4'), 69);
assert.equal(notePitch('F#3'), 54);
assert.equal(notePitch('Bb2'), 46);
assert.equal(notePitch(72), 72);
assert.throws(() => notePitch('H2'));
assert.throws(() => notePitch(128));
assert.equal(noteName(61), 'C#4');

const wasm = new FlowsWasm(new WebAssembly.Module(readFileSync(join(here, '../dist/flows-embed.wasm'))));
const broken = wasm.compile('import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return nope\n');
assert.equal(broken.info.ok, false);
assert.ok(broken.info.diagnostics[0].line >= 1 && broken.info.diagnostics[0].message.length > 0);
wasm.free(broken.handle);

// Unicode survives the hand-written UTF-8 coding (comments and diagnostics).
const source = readFileSync(join(here, '../presets/soft-keys.flows'), 'utf8').replace('# Soft keys', '# Soft keys — 柔和 🎹');
const { handle, info } = wasm.compile(source);
assert.equal(info.ok, true);
assert.equal(info.channels, 2);
wasm.prepare(handle, 48000, 0.003, 138);
assert.equal(wasm.noteOn(handle, 1, 60, 0.8, 0), true);
assert.equal(wasm.noteOn(handle, 1, 62, 0.8, 0), false, 'held identities are unique');
let peak = 0;
for (let block = 0; block < 40; block++) {
  assert.ok(wasm.render(handle, 128) >= 0);
  for (const channel of wasm.output(handle, 128)) for (const v of channel) peak = Math.max(peak, Math.abs(v));
}
assert.ok(peak > 0.01, `soft-keys sounds (${peak})`);
assert.equal(wasm.noteOff(handle, 1, 0), true);
assert.equal(wasm.noteOff(handle, 1, 0), false, 'unknown note-offs are ignored');
assert.equal(wasm.param(handle, 999, 1), false);
wasm.free(handle);

// Parameter keys, labels and knob response, from the compiled preset.
const params = info.parameters;
assert.ok(params.some((p) => p.name === 'cutoff' && p.hasRange && p.scale === 'log'));
const { describeParameters: describe } = await importParams();
const bass = wasm.compile(readFileSync(join(here, '../presets/pluck-bass.flows'), 'utf8'));
const bassList = describe(bass.info.parameters);
assert.equal(findParameter(bassList, 'contour.A').label, 'Contour attack', 'shared names get their owner');
assert.equal(findParameter(bassList, 'amp.A').label, 'Amp attack');
wasm.free(bass.handle);
const list = describe(params);
const cutoff = findParameter(list, 'filtered.cutoff');
assert.ok(cutoff, 'owner.name key');
assert.equal(findParameter(list, 'cutoff'), cutoff, 'unique names resolve');
assert.equal(findParameter(list, cutoff.id), cutoff, 'compiler ids resolve');
assert.equal(findParameter(list, 'level'), undefined, 'ambiguous names do not resolve');
assert.equal(cutoff.label, 'Cutoff');
assert.ok(Math.abs(fromPosition(cutoff, toPosition(cutoff, 2400)) - 2400) <= cutoff.step);
assert.equal(fromPosition(cutoff, 0), cutoff.min);
assert.equal(fromPosition(cutoff, 1), cutoff.max);
assert.ok(toPosition(cutoff, 1000) > 0.4 && toPosition(cutoff, 1000) < 0.6, 'log response puts 1 kHz near the middle');
assert.equal(formatValue(cutoff, 2400), '2.4 kHz');
assert.equal(formatValue(findParameter(list, 'amp.R'), 0.38), '380 ms');
assert.deepEqual(defaultControls(list, 4).map((p) => p.name), ['cutoff', 'resonance', 'A', 'D']);

// Source spans: each parameter points at its literal, also after non-ASCII text.
const spanned = wasm.compile(source);
for (const p of spanned.info.parameters.filter((p) => p.from >= 0))
  assert.match(source.slice(p.from, p.to), /^-?(\d+(\.\d*)?|\.\d+)[a-z]*$/i, `${p.id} spans its literal`);
const cutoffSpan = spanned.info.parameters.find((p) => p.name === 'cutoff' && source.slice(p.from, p.to) === '2.4khz');
assert.ok(cutoffSpan, 'the cutoff literal is found past the emoji');
wasm.free(spanned.handle);
const bad = 'import standard as std\n# ünïcode\nInst.sigstream(std.event.keys.poly()) as key:\n    return nope\n';
const badInfo = wasm.compile(bad);
const d = badInfo.info.diagnostics[0];
assert.ok(d.from >= bad.indexOf('return') && d.from <= bad.length, 'diagnostic spans index the string');
wasm.free(badInfo.handle);
const same = (a, b) => { const x = wasm.compile(a), y = wasm.compile(b); const r = x.info.structure === y.info.structure; wasm.free(x.handle); wasm.free(y.handle); return r; };
assert.ok(same(source, source.replace('R: 380ms', 'R: 900ms')), 'a live value keeps the structure');
assert.ok(!same(source, source.replace('unison: 3', 'unison: 4')), 'a structural value changes it');

// Literals keep the unit and style of the number they replace.
const { formatLiteral } = await importParams();
assert.equal(formatLiteral(cutoff, 3300, '2.4khz'), '3.3khz');
assert.equal(formatLiteral(findParameter(list, 'amp.R'), 0.52, '380ms'), '520ms');
assert.equal(formatLiteral(findParameter(list, 'amp.S'), 0.4, '0.55'), '0.4');

// Without a declared range, the inferred one still holds the authored value.
const unranged = (name, unit, value) => describe([{ id: `x.${name}`, name, unit, value, live: true, hasRange: false, scale: 'linear', scaleOffset: 0, logarithmic: false, from: -1, to: -1 }])[0];
const high = unranged('cutoff', 'hertz', 24000);
assert.ok(high.min <= 24000 && high.max >= 24000 && high.scale === 'log', 'a high frequency fits its range');
const zero = unranged('cutoff', 'hertz', 0);
assert.ok(zero.min <= 0 && zero.max > 0 && zero.scale === 'linear', 'zero hertz needs a linear range');
const quiet = unranged('level', 'decibels', -80);
assert.ok(quiet.min <= -80, 'a quiet level fits its range');

// The editor's highlighting: keywords, numbers, and live numbers as drag handles.
const { highlight, tokenize } = await importModule('../src/elements/code.ts');
const kinds = tokenize('import x\ncutoff: 2.4khz  # note').filter((t) => t.kind).map((t) => t.kind);
assert.deepEqual(kinds, ['keyword', 'property', 'operator', 'number', 'comment']);
const html = highlight('a: 2.4khz <b>', [{ index: 7, from: 3, to: 9 }], []);
assert.match(html, /<span class="param" part="code-param" data-param="7"><span class="number">2\.4khz<\/span><\/span>/);
assert.ok(!html.includes('<b>') && html.includes('&lt;'), 'source text is escaped');
assert.match(highlight('x = nope', [], [{ from: 4, to: 8, line: 1, column: 5, message: '' }]), /bad-text">nope/);
console.log('Unit tests passed.');

async function importParams() {
  // describeParameters is internal (Flows.create applies it); bundle it from source.
  return importModule('../src/params.ts');
}
async function importModule(path) {
  const { build } = await import('esbuild');
  const result = await build({ entryPoints: [join(here, path)], bundle: true, format: 'esm', write: false });
  return import(`data:text/javascript;base64,${Buffer.from(result.outputFiles[0].text).toString('base64')}`);
}
