// Minimal JSON Schema support for the Flows protocol: the subset that
// flows-protocol.json uses ($ref, type, enum, const, properties, required,
// items, oneOf, allOf, minimum, maximum, maxItems). No dependencies, so the
// generator and the conformance tests run with plain Node.
import { readFileSync } from 'node:fs';

export const protocolUrl = new URL('./flows-protocol.json', import.meta.url);
export function loadProtocol() {
  return JSON.parse(readFileSync(protocolUrl, 'utf8'));
}

export function resolve(root, schema) {
  let current = schema;
  for (let depth = 0; current && current.$ref; ++depth) {
    if (depth > 32) throw new Error(`Reference cycle at ${schema.$ref}`);
    const match = /^#\/\$defs\/(.+)$/.exec(current.$ref);
    if (!match || !root.$defs[match[1]]) throw new Error(`Unresolved reference ${current.$ref}`);
    current = root.$defs[match[1]];
  }
  return current;
}

function typeOf(value) {
  if (value === null) return 'null';
  if (Array.isArray(value)) return 'array';
  if (Number.isInteger(value)) return 'integer';
  return typeof value;
}
function matchesType(expected, value) {
  const actual = typeOf(value);
  if (expected === 'number') return actual === 'number' || actual === 'integer';
  return expected === actual;
}

/** Returns a list of "path: problem" strings; empty when the value conforms. */
export function validate(root, schema, value, path = '$') {
  const errors = [];
  const check = (s, v, p) => {
    s = resolve(root, s);
    if (s.allOf) for (const part of s.allOf) check(part, v, p);
    if (s.oneOf) {
      const passing = s.oneOf.filter(part => validate(root, part, v, p).length === 0).length;
      if (passing !== 1) errors.push(`${p}: expected exactly one alternative to match, ${passing} matched`);
      return;
    }
    if ('const' in s && v !== s.const) errors.push(`${p}: expected ${JSON.stringify(s.const)}`);
    if (s.enum && !s.enum.includes(v)) errors.push(`${p}: ${JSON.stringify(v)} is not one of ${JSON.stringify(s.enum)}`);
    if (s.type) {
      const types = Array.isArray(s.type) ? s.type : [s.type];
      if (!types.some(type => matchesType(type, v))) {
        errors.push(`${p}: expected ${types.join(' or ')}, got ${typeOf(v)}`);
        return;
      }
    }
    if (typeof v === 'number') {
      if (s.minimum !== undefined && v < s.minimum) errors.push(`${p}: ${v} < minimum ${s.minimum}`);
      if (s.maximum !== undefined && v > s.maximum) errors.push(`${p}: ${v} > maximum ${s.maximum}`);
    }
    if (Array.isArray(v)) {
      if (s.maxItems !== undefined && v.length > s.maxItems) errors.push(`${p}: more than ${s.maxItems} items`);
      if (s.items) v.forEach((item, index) => check(s.items, item, `${p}[${index}]`));
    }
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      for (const key of s.required ?? []) if (!(key in v)) errors.push(`${p}: missing required "${key}"`);
      for (const [key, child] of Object.entries(s.properties ?? {})) if (key in v) check(child, v[key], `${p}.${key}`);
      if (s.additionalProperties === false)
        for (const key of Object.keys(v)) if (!(key in (s.properties ?? {}))) errors.push(`${p}: unexpected "${key}"`);
    }
  };
  check(schema, value, path);
  return errors;
}

/** Throws if any $ref in the document does not resolve. */
export function checkReferences(root) {
  const visit = node => {
    if (Array.isArray(node)) return node.forEach(visit);
    if (!node || typeof node !== 'object') return;
    if (node.$ref) resolve(root, node);
    Object.values(node).forEach(visit);
  };
  visit(root);
}
