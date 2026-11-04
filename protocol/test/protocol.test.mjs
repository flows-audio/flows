// Protocol checks.
//   node protocol/test/protocol.test.mjs                      schema + UI usage
//   node protocol/test/protocol.test.mjs native.json          + validate a core run
//   node protocol/test/protocol.test.mjs native.json wasm.json + compare two cores
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { checkReferences, loadProtocol, resolve, validate } from '../schema.mjs';

const protocol = loadProtocol();
checkReferences(protocol);
const generator = fileURLToPath(new URL('../generate.mjs', import.meta.url));
execFileSync(process.execPath, [generator, '--check'], { stdio: 'inherit' });

// Every method has an owner, params, result; owners are declared.
for (const [name, method] of Object.entries(protocol.methods)) {
  assert.ok(protocol.owners[method.owner], `${name}: unknown owner ${method.owner}`);
  assert.ok(method.params && method.result && method.summary, `${name}: params, result and summary are required`);
}

// Every literal method the UI calls exists in the contract.
const sources = new URL('../../ui/src/', import.meta.url);
const files = readdirSync(sources, { recursive: true }).filter(name => /\.(ts|tsx)$/.test(name) && !name.endsWith('.gen.ts'));
const used = new Set();
for (const file of files) {
  const text = readFileSync(new URL(file, sources), 'utf8');
  for (const match of text.matchAll(/\b(?:request|perform|call|execute)\(\s*'([a-z]+(?:\.[a-zA-Z]+)+|compile)'/g)) used.add(match[1]);
}
for (const method of used) assert.ok(protocol.methods[method], `UI calls ${method}, which protocol/flows-protocol.json does not define`);

function check(file) {
  const run = JSON.parse(readFileSync(file, 'utf8'));
  for (const step of run.steps) {
    const method = protocol.methods[step.method];
    if (!method) { assert.equal(step.ok, false, `${step.label}: undefined method must fail`); continue; }
    // Steps that expect an error send invalid params on purpose.
    if (!step.ok) continue;
    const paramErrors = validate(protocol, method.params, step.params);
    assert.deepEqual(paramErrors, [], `${step.label}: params do not match ${step.method}`);
    const errors = validate(protocol, method.result, step.result);
    assert.deepEqual(errors, [], `${step.label}: ${step.method} result does not match the protocol`);
  }
  return run;
}

// Values that legitimately differ between runs or machines.
const volatileKeys = new Set(['sessionId', 'workspaceId', 'cpu', 'peak', 'updatedAt', 'startedAt', 'finishedAt', 'host', 'version', 'sourceUrl']);
function normalize(value, key = '') {
  if (volatileKeys.has(key)) return '<volatile>';
  if (Array.isArray(value)) return value.map(item => normalize(item));
  if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, normalize(v, k)]));
  if (typeof value === 'string') return value.replace(/\b[0-9a-f]{32}\b/g, '<id>');
  // Different CPUs may fuse multiply-adds; compare compile-time numbers to 9 digits.
  if (typeof value === 'number' && !Number.isInteger(value)) return Number(value.toPrecision(9));
  return value;
}

const outputs = process.argv.slice(2);
const runs = outputs.map(check);
if (runs.length === 2) {
  const [a, b] = runs;
  assert.equal(a.steps.length, b.steps.length, 'Core runs executed different step counts');
  a.steps.forEach((step, index) => {
    const other = b.steps[index];
    assert.equal(other.label, step.label);
    assert.deepEqual(normalize(other.params), normalize(step.params), `${step.label}: resolved params differ between cores`);
    assert.deepEqual(normalize(other.result ?? other.error), normalize(step.result ?? step.error), `${step.label}: results differ between cores`);
  });
}
console.log(`Protocol: ${Object.keys(protocol.methods).length} methods, ${Object.keys(protocol.$defs).length} types; UI uses ${used.size} literal methods${runs.length ? `; validated ${runs.length} core run(s)` : ''}${runs.length === 2 ? ' and found them identical' : ''}.`);
// resolve() is exported for tools; keep the import used.
void resolve;
