// Instrument parameters as the page sees them: stable keys, usable ranges,
// knob response and display text. The response curves follow the Flows
// editor (ui/src/parameterDrag.ts) so a knob feels the same in both.
import type { ParameterInfo } from './wasm';

export interface FlowsParameter {
  /** Compiler binding, e.g. "amp.literal3". Stable while the source keeps its shape. */
  readonly id: string;
  /** Friendly unique key, e.g. "amp.R" or "filtered.cutoff". Use with setParam(). */
  readonly key: string;
  /** Port name, e.g. "R" or "cutoff". setParam() also accepts it when unique. */
  readonly name: string;
  /** The assignment that owns it, e.g. "amp". */
  readonly owner: string;
  /** Human label, e.g. "Release" or "Cutoff". */
  readonly label: string;
  readonly unit: ParameterInfo['unit'];
  /** Authored value in the source. */
  readonly initial: number;
  /** Usable range. Unranged values get a range suited to their unit. */
  readonly min: number;
  readonly max: number;
  readonly step: number;
  /** Declared by the source (true) or inferred from the unit (false). */
  readonly hasRange: boolean;
  /** Changes apply while playing. Other values need a new source. */
  readonly live: boolean;
  readonly discrete: boolean;
  /** Knob response: linear, log, offset_log, decibels or gain. */
  readonly scale: string;
  readonly scaleOffset: number;
  /** Index in the compiled program. */
  readonly index: number;
  /** The literal in the source that sets it: [from, to) string indices, or -1 when there is none. */
  readonly from: number;
  readonly to: number;
}

const LABELS: Record<string, string> = {
  A: 'Attack', D: 'Decay', S: 'Sustain', R: 'Release', cutoff: 'Cutoff', resonance: 'Resonance', level: 'Level',
  detune: 'Detune', spread: 'Spread', pan: 'Pan', drive: 'Drive', mix: 'Mix', output: 'Output', rate: 'Rate',
  glide: 'Glide', octave: 'Octave', semitune: 'Semitones', phase: 'Phase', random: 'Random', fade: 'Fade', delay: 'Delay',
};
const humanize = (name: string) => LABELS[name] ?? name.replace(/[_-]+/g, ' ').replace(/^\w/, (c) => c.toUpperCase());

function inferredRange(p: ParameterInfo): [number, number, string, number] {
  const value = p.value;
  switch (p.unit) {
    // A range that holds the authored value; log needs a positive minimum.
    case 'hertz': return value > 0 ? [Math.min(20, value), Math.max(20000, value), 'log', 0] : [Math.min(0, value), 20000, 'linear', 0];
    case 'seconds': return [0, Math.max(10, value * 2), 'offset_log', 0.001];
    case 'decibels': return [Math.min(-60, value), Math.max(12, value), 'decibels', 0];
    case 'pitch': return [0, 127, 'linear', 0];
    case 'boolean': return [0, 1, 'linear', 0];
    default: {
      const span = Math.max(1, Math.abs(value) * 2);
      return [Math.min(0, value < 0 ? -span : 0), Math.max(1, span), 'linear', 0];
    }
  }
}

export function describeParameters(list: readonly ParameterInfo[]): FlowsParameter[] {
  const seen = new Map<string, number>();
  // A name used by several owners ("A" in two envelopes) gets the owner in its label.
  const owners = new Map<string, Set<string>>();
  for (const p of list) owners.set(p.name, (owners.get(p.name) ?? new Set()).add(p.id.slice(0, Math.max(0, p.id.lastIndexOf('.')))));
  return list.map((p) => {
    const dot = p.id.lastIndexOf('.');
    const owner = dot > 0 ? p.id.slice(0, dot) : '';
    let key = owner ? `${owner}.${p.name}` : p.name;
    const count = (seen.get(key) ?? 0) + 1;
    seen.set(key, count);
    if (count > 1) key = `${key}#${count}`;
    const ranged = p.hasRange && Number.isFinite(p.min) && Number.isFinite(p.max) && (p.max as number) > (p.min as number);
    const [min, max, scale, offset] = ranged
      ? [p.min as number, p.max as number, p.logarithmic && p.scale === 'linear' ? 'log' : p.scale, p.scaleOffset]
      : inferredRange(p);
    return Object.freeze({
      id: p.id, key, name: p.name, owner,
      label: (owners.get(p.name)?.size ?? 0) > 1 && owner && !owner.startsWith('$') ? `${humanize(owner)} ${humanize(p.name).toLowerCase()}` : humanize(p.name),
      unit: p.unit, initial: p.value,
      min, max, step: p.step > 0 ? p.step : (max - min) / 1000, hasRange: ranged, live: p.live, discrete: p.discrete,
      scale, scaleOffset: offset, index: p.index,
      from: p.from ?? -1, to: p.to ?? -1,
    });
  });
}

/** Finds a parameter by key, compiler id, or a name/owner that is unique. */
export function findParameter(list: readonly FlowsParameter[], query: string): FlowsParameter | undefined {
  const exact = list.find((p) => p.key === query || p.id === query);
  if (exact) return exact;
  const named = list.filter((p) => p.name === query);
  if (named.length === 1) return named[0];
  const owned = list.filter((p) => p.owner === query);
  return owned.length === 1 ? owned[0] : undefined;
}

// Musical priority for the default controls: tone first, then envelope.
const PRIORITY = ['cutoff', 'resonance', 'drive', 'mix', 'A', 'D', 'S', 'R', 'detune', 'spread', 'glide', 'rate', 'level'];
export function defaultControls(list: readonly FlowsParameter[], limit: number): FlowsParameter[] {
  const rank = (p: FlowsParameter) => {
    const index = PRIORITY.indexOf(p.name);
    return index < 0 ? PRIORITY.length : index;
  };
  return list
    .filter((p) => p.live && !p.discrete && (p.hasRange || p.unit === 'hertz'))
    .map((p, order) => ({ p, order }))
    .sort((a, b) => rank(a.p) - rank(b.p) || a.order - b.order)
    .slice(0, limit)
    .map(({ p }) => p);
}

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
const dbValues = [-120, -60, -36, -12, 0, 6, 24];
const dbPositions = [0, 0.03, 0.12, 0.48, 0.75, 0.88, 1];
function interpolate(value: number, xs: number[], ys: number[]): number {
  let i = 0;
  while (i < xs.length - 2 && value > xs[i + 1]) i++;
  return ys[i] + ((value - xs[i]) / (xs[i + 1] - xs[i])) * (ys[i + 1] - ys[i]);
}

/** Knob position (0..1) for a value, following the parameter's response. */
export function toPosition(p: FlowsParameter, value: number): number {
  const [lo, hi] = [encode(p, p.min), encode(p, p.max)];
  return hi > lo ? clamp((encode(p, value) - lo) / (hi - lo), 0, 1) : 0;
}
/** Value for a knob position (0..1), snapped to the parameter's step. */
export function fromPosition(p: FlowsParameter, position: number): number {
  const [lo, hi] = [encode(p, p.min), encode(p, p.max)];
  if (position <= 0) return p.min;
  if (position >= 1) return p.max;
  const value = decode(p, lo + position * (hi - lo));
  const step = p.discrete ? Math.max(1, Math.round(p.step)) : p.step;
  return clamp(Number((Math.round(value / step) * step).toPrecision(12)), p.min, p.max);
}
function encode(p: FlowsParameter, v: number): number {
  if (p.scale === 'decibels') return interpolate(v, dbValues, dbPositions);
  if (p.scale === 'gain') return v < 1e-6 ? -0.01 + Math.max(0, v) * 10000 : interpolate(20 * Math.log10(v), dbValues, dbPositions);
  if (p.scale === 'offset_log' && p.scaleOffset > 0) return Math.log1p(Math.max(0, v) / p.scaleOffset);
  if (p.scale === 'log' && p.min > 0) return Math.log(Math.max(p.min, v));
  return v;
}
function decode(p: FlowsParameter, v: number): number {
  if (p.scale === 'decibels') return interpolate(v, dbPositions, dbValues);
  if (p.scale === 'gain') return v <= -0.01 ? 0 : v < 0 ? (v + 0.01) * 0.0001 : Math.pow(10, interpolate(v, dbPositions, dbValues) / 20);
  if (p.scale === 'offset_log' && p.scaleOffset > 0) return p.scaleOffset * Math.expm1(v);
  if (p.scale === 'log' && p.min > 0) return Math.exp(v);
  return v;
}

const trim = (value: number, digits: number) => String(Number(value.toPrecision(digits)));
/** Display text with units, e.g. "1.2 kHz", "350 ms", "-6 dB". */
export function formatValue(p: FlowsParameter, value: number): string {
  if (!Number.isFinite(value)) return '–';
  switch (p.unit) {
    case 'hertz': return Math.abs(value) >= 1000 ? `${trim(value / 1000, 3)} kHz` : `${trim(value, 3)} Hz`;
    case 'seconds': return Math.abs(value) < 1 ? `${trim(value * 1000, 3)} ms` : `${trim(value, 3)} s`;
    case 'decibels': return `${trim(value, 3)} dB`;
    case 'boolean': return value >= 0.5 ? 'On' : 'Off';
    default: return p.discrete ? String(Math.round(value)) : trim(value, 3);
  }
}

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const SEMITONES: Record<string, number> = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
/** MIDI note from a number or a name such as "C4" (60), "F#3" or "Bb2". */
export function notePitch(note: number | string): number {
  if (typeof note === 'number') {
    if (!Number.isFinite(note) || note < 0 || note > 127) throw new RangeError(`Note out of range: ${note}`);
    return note;
  }
  const match = /^([A-Ga-g])([#b]?)(-?\d)$/.exec(note.trim());
  if (!match) throw new RangeError(`Unknown note name: ${note}`);
  const pitch = (Number(match[3]) + 1) * 12 + SEMITONES[match[1].toUpperCase()] + (match[2] === '#' ? 1 : match[2] === 'b' ? -1 : 0);
  return notePitch(pitch);
}
export function noteName(pitch: number): string {
  return `${NAMES[((pitch % 12) + 12) % 12]}${Math.floor(pitch / 12) - 1}`;
}

// Unit suffixes a literal may carry, and the factor from the parameter's base unit (Hz, s, dB).
const SUFFIX_SCALE: Record<string, number> = { hz: 1, khz: 0.001, s: 1, ms: 1000, db: 1 };

/**
 * Source text for a value, in the unit and style of the literal it replaces:
 * 3200 Hz over "3.2khz" gives "3.2khz", 0.52 s over "380ms" gives "520ms".
 */
export function formatLiteral(p: FlowsParameter, value: number, original: string): string {
  const suffix = /[a-z]+$/i.exec(original)?.[0] ?? '';
  const scale = SUFFIX_SCALE[suffix.toLowerCase()] ?? 1;
  if (p.discrete) return `${Math.round(value * scale)}${suffix}`;
  const step = Math.abs(p.step * scale) || 0.001;
  const decimals = Math.max(0, Math.min(6, Math.ceil(-Math.log10(step) - 1e-9)));
  const text = (value * scale).toFixed(decimals).replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
  return `${text === '-0' ? '0' : text}${suffix}`;
}
