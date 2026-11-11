// @flows-audio/embed/core: a Flows instrument without any interface.
//
//   const flows = await Flows.create({ preset: 'soft-keys' });
//   button.onclick = async () => { await flows.start(); flows.noteOn('C4'); };
//
// Each instrument compiles once on the page (parameters, diagnostics) and
// renders in its own AudioWorkletNode with its own WebAssembly instance. All
// instruments on a page share one AudioContext; the first start() from a user
// gesture unlocks sound for every one of them.
import { FlowsWasm, type Diagnostic, type ProgramInfo } from './wasm';
import type { FromProcessor, ProcessorOptions, ToProcessor } from './worklet';
import { describeParameters, findParameter, notePitch, type FlowsParameter } from './params';
// Replaced at build time with the bundled processor (scripts/build.mjs).
import { WORKLET_SOURCE } from 'flows:worklet';

export type { Diagnostic, FlowsParameter };
export { FlowsWasm } from './wasm';
export { defaultControls, findParameter, formatLiteral, formatValue, fromPosition, noteName, notePitch, toPosition } from './params';

/** Presets shipped next to the script in presets/. */
export const PRESETS = Object.freeze([
  'soft-keys', 'glass-pad', 'pluck-bass',
  'sine-reference', 'mono-legato-glide', 'trance-rolling-bass', 'stereo-motion', 'probe-lab', 'sample-one-shot', 'sample-loop',
] as const);
export const DEFAULT_PRESET = 'soft-keys';
const PRODUCTION_ASSETS = 'https://flows-audio.github.io/flows/embed/';

/**
 * - loading: fetching and compiling
 * - ready: compiled; sound starts after a user gesture calls start()
 * - starting: the audio context or worklet is starting
 * - running: producing sound
 * - suspended: the browser paused audio (hidden tab, phone call); start() resumes
 * - error: see `error`
 * - disposed
 */
export type FlowsState = 'loading' | 'ready' | 'starting' | 'running' | 'suspended' | 'error' | 'disposed';

export interface FlowsSource {
  /** Flows source text. */
  source?: string;
  /** A bundled preset name (see PRESETS). */
  preset?: string;
  /** URL of a .flows file. Samples resolve relative to it. */
  src?: string | URL;
  /** Base URL for sample paths in inline `source`. Defaults to the page. */
  baseUrl?: string | URL;
}
export interface CreateOptions extends FlowsSource {
  /** Use this AudioContext instead of the page-wide shared one. */
  context?: AudioContext;
  /** Where the output goes. Defaults to the context destination; null leaves it unconnected. */
  destination?: AudioNode | null;
  /** Output gain, 0..1 (default 0.8). */
  volume?: number;
  /** Tempo for tempo-synced LFOs (default 138, as in the app). */
  bpm?: number;
  /** Click-free note boundaries in seconds (default 0.003, as in the app). 0 renders exactly like offline. */
  boundarySeconds?: number;
}
export interface NoteOptions {
  /** Velocity 0..1 (default 0.8). */
  velocity?: number;
  /** AudioContext time to schedule at; omitted or past plays now. */
  time?: number;
}
export interface FlowsAnalyser {
  /** The Web Audio AnalyserNode, once audio has started. */
  readonly node: AnalyserNode | null;
  readonly size: number;
  /** Time-domain samples in -1..1 (fftSize values). Zeros before audio starts. */
  waveform(into?: Float32Array): Float32Array;
  /** Magnitudes in dB (fftSize / 2 bins). */
  spectrum(into?: Float32Array): Float32Array;
  /** RMS level of the current waveform, 0..1. */
  level(): number;
}
export interface FlowsEvents {
  state: { state: FlowsState; previous: FlowsState };
  error: { error: Error };
  param: { parameter: FlowsParameter; value: number };
  note: { pitch: number; on: boolean; velocity: number };
  /** `hot`: the new source changed only live values, applied in place without interrupting sound. */
  load: { parameters: FlowsParameter[]; hot: boolean };
  meter: { peak: number; voices: number };
}

/** Compilation failed; `diagnostics` lists line, column and message. */
export class FlowsCompileError extends Error {
  constructor(readonly diagnostics: Diagnostic[]) {
    super(diagnostics.map((d) => `line ${d.line}:${d.column}: ${d.message}`).join('\n') || 'The instrument did not compile');
    this.name = 'FlowsCompileError';
  }
}

// --- Assets ---------------------------------------------------------------

interface Config { assets?: string; wasm?: string; worklet?: string }
const config: Config = {};
/** Where the .wasm, worklet and presets live (default: next to this script). */
export function configure(options: { assets?: string | URL; wasm?: string | URL; worklet?: string | URL }) {
  if (options.assets !== undefined) config.assets = String(options.assets).replace(/\/?$/, '/');
  if (options.wasm !== undefined) config.wasm = String(options.wasm);
  if (options.worklet !== undefined) config.worklet = String(options.worklet);
}
function scriptBase(): string {
  try {
    // Bundlers recognise this pattern and emit the .wasm next to the bundle.
    return new URL('./', import.meta.url).href;
  } catch {
    return PRODUCTION_ASSETS;
  }
}
function asset(name: string): string {
  if (config.assets) return new URL(name, config.assets).href;
  return new URL(name, /^(https?|file):/.test(scriptBase()) ? scriptBase() : PRODUCTION_ASSETS).href;
}
function wasmUrl(): string {
  if (config.wasm) return config.wasm;
  if (config.assets) return asset('flows-embed.wasm');
  try {
    const url = new URL('./flows-embed.wasm', import.meta.url);
    if (/^(https?|file|blob|data):/.test(url.protocol)) return url.href;
  } catch { /* classic script without a known location */ }
  return asset('flows-embed.wasm');
}
const pageBase = () => (typeof document !== 'undefined' ? document.baseURI : PRODUCTION_ASSETS);

async function fetchOk(url: string, what: string): Promise<Response> {
  let response: Response;
  try {
    response = await fetch(url, { mode: 'cors', credentials: 'omit' });
  } catch (error) {
    throw new Error(`Could not load ${what} from ${url} (network or CORS): ${(error as Error).message}`);
  }
  if (!response.ok) throw new Error(`Could not load ${what} from ${url}: HTTP ${response.status}`);
  return response;
}

interface Runtime { module: WebAssembly.Module; bytes: ArrayBuffer; page: FlowsWasm }
let runtime: Promise<Runtime> | null = null;
function loadRuntime(): Promise<Runtime> {
  runtime ??= (async () => {
    const bytes = await (await fetchOk(wasmUrl(), 'the Flows engine')).arrayBuffer();
    const module = await WebAssembly.compile(bytes);
    return { module, bytes, page: new FlowsWasm(module) };
  })();
  runtime.catch(() => { runtime = null; });
  return runtime;
}

interface Resolved { source: string; base: string }
async function resolveSource(options: FlowsSource): Promise<Resolved> {
  if (typeof options.source === 'string') return { source: options.source, base: new URL(String(options.baseUrl ?? pageBase()), pageBase()).href };
  if (options.src !== undefined) {
    const url = new URL(String(options.src), pageBase()).href;
    return { source: await (await fetchOk(url, 'the instrument')).text(), base: url };
  }
  const preset = options.preset ?? DEFAULT_PRESET;
  if (!/^[a-z0-9][a-z0-9-]*$/.test(preset)) throw new Error(`Unknown preset: ${preset}`);
  const url = asset(`presets/${preset}.flows`);
  try {
    return { source: await (await fetchOk(url, `preset "${preset}"`)).text(), base: url };
  } catch (error) {
    // Bundlers copy the .wasm next to the bundle but not the presets folder.
    throw new Error(`${(error as Error).message}. Presets: ${PRESETS.join(', ')}. When bundling, call `
      + `Flows.configure({ assets: '${PRODUCTION_ASSETS}' }) or pass src/source instead.`);
  }
}

interface Compiled { info: ProgramInfo; parameters: FlowsParameter[]; samples: (ArrayBuffer | null)[]; source: string; base: string }
async function compileOnPage(resolved: Resolved): Promise<Compiled> {
  const { page } = await loadRuntime();
  const { handle, info } = page.compile(resolved.source);
  page.free(handle);
  if (!info.ok) throw new FlowsCompileError(info.diagnostics);
  const samples = await Promise.all((info.samples ?? []).map(async (path) =>
    (await fetchOk(new URL(path, resolved.base).href, `sample "${path}"`)).arrayBuffer()));
  return { info, parameters: describeParameters(info.parameters ?? []), samples, source: resolved.source, base: resolved.base };
}

// --- Audio context --------------------------------------------------------

let shared: AudioContext | null = null;
const instances = new Set<Flows>();
const worklets = new WeakMap<BaseAudioContext, Promise<void>>();

function audioSupported() {
  return typeof AudioContext !== 'undefined' && typeof AudioWorkletNode !== 'undefined';
}
/** The page-wide AudioContext, created on first use (call from a user gesture). */
export function sharedContext(): AudioContext {
  if (!audioSupported()) throw new Error('This browser does not support AudioWorklet');
  if (!shared || shared.state === 'closed') {
    shared = new AudioContext({ latencyHint: 'interactive' });
    watch(shared);
  }
  return shared;
}
const watched = new WeakSet<BaseAudioContext>();
function watch(context: AudioContext) {
  if (watched.has(context)) return;
  watched.add(context);
  context.addEventListener('statechange', () => {
    for (const flows of instances) flows._contextChanged(context);
  });
}

function loadWorklet(context: BaseAudioContext): Promise<void> {
  let loading = worklets.get(context);
  if (!loading) {
    loading = (async () => {
      // A Blob URL avoids a cross-origin worklet fetch; pages whose CSP blocks
      // blob: scripts fall back to the hosted file (served with CORS).
      try {
        const url = URL.createObjectURL(new Blob([WORKLET_SOURCE], { type: 'text/javascript' }));
        try { await context.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
      } catch {
        await context.audioWorklet.addModule(config.worklet ?? asset('flows-embed-worklet.js'));
      }
    })();
    worklets.set(context, loading);
    loading.catch(() => worklets.delete(context));
  }
  return loading;
}

// --- Instrument -----------------------------------------------------------

type Listener<K extends keyof FlowsEvents> = (event: FlowsEvents[K]) => void;

export class Flows {
  /** Compiles an instrument. Sound starts after start() from a user gesture. */
  static async create(options: CreateOptions = {}): Promise<Flows> {
    const flows = new Flows(options);
    try {
      flows.compiled = await compileOnPage(await resolveSource(options));
      flows.resetValues();
      flows.setState('ready');
      // Another instrument may already have unlocked audio.
      const context = options.context ?? shared;
      if (context && context.state === 'running') void flows.attach().catch(() => {});
    } catch (error) {
      flows.fail(error as Error);
      throw error;
    }
    return flows;
  }
  static readonly presets = PRESETS;
  static configure = configure;
  /** The shared AudioContext, or null before any instrument started. */
  static get context(): AudioContext | null { return shared; }
  /** True when this browser can run instruments (AudioWorklet and WebAssembly). */
  static get supported(): boolean { return audioSupported() && typeof WebAssembly !== 'undefined'; }

  private compiled!: Compiled;
  private values = new Map<number, number>();
  private held = new Map<number, { id: number; velocity: number }>();
  private nextNote = 1;
  private node: AudioWorkletNode | null = null;
  private gain: GainNode | null = null;
  private analyserNode: AnalyserNode | null = null;
  private analyserSize = 2048;
  private attaching: Promise<void> | null = null;
  private listeners = new Map<keyof FlowsEvents, Set<(event: never) => void>>();
  private requests = new Map<number, { resolve(): void; reject(error: Error): void }>();
  private nextRequest = 1;
  private currentState: FlowsState = 'loading';
  private volumeValue: number;
  private bpmValue: number;
  private readonly boundary: number;
  private readonly ownContext: AudioContext | null;
  private readonly destinationOption: AudioNode | null | undefined;
  /** The last error, if state is 'error'. */
  error: Error | null = null;
  /** Peak output of the last ~20 ms, 0.. (from the audio thread). */
  peak = 0;
  /** Voices currently sounding. */
  voices = 0;

  private constructor(options: CreateOptions) {
    this.ownContext = options.context ?? null;
    this.destinationOption = options.destination;
    this.volumeValue = clamp01(options.volume ?? 0.8);
    this.bpmValue = options.bpm ?? 138;
    this.boundary = options.boundarySeconds ?? 0.003;
    if (this.ownContext) watch(this.ownContext);
    instances.add(this);
  }

  get state(): FlowsState { return this.currentState; }
  /** The AudioContext this instrument plays in, once started. */
  get context(): AudioContext | null { return this.ownContext ?? shared; }
  /** The instrument's output (a GainNode), once started. Connect it anywhere. */
  get output(): GainNode | null { return this.gain; }
  /** The compiled source text. */
  get source(): string { return this.compiled?.source ?? ''; }
  /** Where the current instrument's relative sample paths resolve (its src or preset URL, else the page). */
  get baseUrl(): string { return this.compiled?.base ?? ''; }
  get channels(): 1 | 2 { return this.compiled?.info.channels ?? 1; }
  get voiceMode(): 'poly' | 'mono' { return this.compiled?.info.voice ?? 'poly'; }

  /** Parameters with keys, ranges and current values. */
  params(): (FlowsParameter & { value: number })[] {
    return (this.compiled?.parameters ?? []).map((p) => ({ ...p, value: this.values.get(p.index) ?? p.initial }));
  }
  param(key: string): (FlowsParameter & { value: number }) | undefined {
    const p = findParameter(this.compiled?.parameters ?? [], key);
    return p && { ...p, value: this.values.get(p.index) ?? p.initial };
  }
  getParam(key: string): number | undefined { return this.param(key)?.value; }
  /** Sets a parameter by key ("amp.R"), compiler id or unique name. Returns the applied value. */
  setParam(key: string, value: number): number {
    const p = findParameter(this.compiled?.parameters ?? [], key);
    if (!p) throw new Error(`Unknown parameter "${key}". Available: ${this.params().map((x) => x.key).join(', ')}`);
    if (!p.live) throw new Error(`Parameter "${p.key}" cannot change while playing; edit the source instead`);
    if (!Number.isFinite(value)) throw new RangeError(`Parameter "${p.key}" needs a finite number`);
    const applied = Math.max(p.min, Math.min(p.max, value));
    this.values.set(p.index, applied);
    this.send({ type: 'param', index: p.index, value: applied });
    this.emit('param', { parameter: p, value: applied });
    return applied;
  }
  /** Restores every parameter to the value written in the source. */
  resetParams() {
    for (const p of this.compiled?.parameters ?? []) if (p.live && this.values.get(p.index) !== p.initial) this.setParam(p.key, p.initial);
  }

  /**
   * Starts sound. Call it from a click, tap or key press: browsers only allow
   * audio after a user gesture. Resolves when the instrument is running.
   */
  start(): Promise<void> {
    if (this.currentState === 'disposed') return Promise.reject(new Error('This instrument was disposed'));
    if (this.currentState === 'loading' || this.currentState === 'error') return Promise.reject(this.error ?? new Error('The instrument is still loading'));
    // Resume synchronously, inside the gesture (Safari requires it).
    let context: AudioContext;
    try {
      context = this.ownContext ?? sharedContext();
    } catch (error) {
      this.fail(error as Error);
      return Promise.reject(error);
    }
    const resumed = context.state === 'running' ? Promise.resolve() : context.resume();
    if (this.currentState !== 'running') this.setState('starting');
    return Promise.all([resumed, this.attach()]).then(() => this._contextChanged(context));
  }

  /** Plays a note. `note` is a MIDI number (60 = C4) or a name ("C4", "F#3"). Returns the pitch. */
  noteOn(note: number | string, velocityOrOptions?: number | NoteOptions): number {
    const pitch = notePitch(note);
    const options = typeof velocityOrOptions === 'number' ? { velocity: velocityOrOptions } : velocityOrOptions ?? {};
    const velocity = clamp01(options.velocity ?? 0.8);
    if (this.held.has(pitch)) this.noteOff(pitch, { time: options.time });
    const id = this.nextNote++;
    this.held.set(pitch, { id, velocity });
    this.send({ type: 'on', id, pitch, velocity, time: options.time });
    this.emit('note', { pitch, on: true, velocity });
    return pitch;
  }
  noteOff(note: number | string, options: { time?: number } = {}): void {
    const pitch = notePitch(note);
    const held = this.held.get(pitch);
    if (!held) return;
    this.held.delete(pitch);
    this.send({ type: 'off', id: held.id, time: options.time });
    this.emit('note', { pitch, on: false, velocity: 0 });
  }
  /** Releases every held note. */
  allNotesOff(): void { for (const pitch of [...this.held.keys()]) this.noteOff(pitch); }
  /** Silences immediately, cutting release tails. */
  panic(): void {
    for (const pitch of [...this.held.keys()]) this.emit('note', { pitch, on: false, velocity: 0 });
    this.held.clear();
    this.send({ type: 'panic' });
  }
  /** Pitches currently held. */
  heldNotes(): number[] { return [...this.held.keys()]; }

  get volume(): number { return this.volumeValue; }
  set volume(value: number) {
    this.volumeValue = clamp01(value);
    if (this.gain) this.gain.gain.setTargetAtTime(this.volumeValue, this.gain.context.currentTime, 0.015);
  }
  get bpm(): number { return this.bpmValue; }
  set bpm(value: number) {
    if (!Number.isFinite(value)) return;
    this.bpmValue = Math.max(20, Math.min(300, value));
    this.send({ type: 'bpm', bpm: this.bpmValue });
  }

  /** Waveform and spectrum data for drawing your own visuals. */
  analyser(options: { size?: number } = {}): FlowsAnalyser {
    if (options.size) this.analyserSize = Math.max(32, Math.min(32768, 2 ** Math.round(Math.log2(options.size))));
    if (this.analyserNode) this.analyserNode.fftSize = this.analyserSize;
    else if (this.gain) this.connectAnalyser();
    const self = this;
    return {
      get node() { return self.analyserNode; },
      get size() { return self.analyserNode?.fftSize ?? self.analyserSize; },
      waveform(into) {
        const out = into ?? new Float32Array(this.size);
        if (self.analyserNode) self.analyserNode.getFloatTimeDomainData(out); else out.fill(0);
        return out;
      },
      spectrum(into) {
        const out = into ?? new Float32Array(this.size / 2);
        if (self.analyserNode) self.analyserNode.getFloatFrequencyData(out); else out.fill(-Infinity);
        return out;
      },
      level() {
        const data = this.waveform();
        let sum = 0;
        for (const value of data) sum += value * value;
        return Math.sqrt(sum / data.length);
      },
    };
  }

  /**
   * Replaces the instrument (source, preset or src). When the new source differs
   * only in live values (a number edited in the code), the running engine keeps
   * playing: changed values are applied in place, values the page set since stay.
   * Otherwise the engine is replaced, held notes end and parameters reset.
   */
  async load(options: FlowsSource): Promise<void> {
    const compiled = await compileOnPage(await resolveSource(options));
    const previous = this.compiled;
    if (previous?.info.structure && compiled.info.structure === previous.info.structure
      && compiled.parameters.length === previous.parameters.length) {
      this.compiled = compiled;
      compiled.parameters.forEach((p, i) => {
        if (p.live && p.initial !== previous.parameters[i].initial) this.setParam(p.key, p.initial);
      });
      this.emit('load', { parameters: compiled.parameters, hot: true });
      return;
    }
    // Notes held now end with the old engine. Notes pressed while it loads reach the new one
    // (the worklet handles messages in order), so they stay held until their note-off.
    const ending = new Map([...this.held].map(([pitch, note]) => [pitch, note.id]));
    if (this.node) {
      const request = this.nextRequest++;
      const done = new Promise<void>((resolve, reject) => this.requests.set(request, { resolve, reject }));
      this.send({ type: 'load', source: compiled.source, samples: compiled.samples.map(copyBuffer), request });
      await done;
    }
    for (const [pitch, id] of ending) {
      if (this.held.get(pitch)?.id !== id) continue;
      this.held.delete(pitch);
      this.emit('note', { pitch, on: false, velocity: 0 });
    }
    this.compiled = compiled;
    this.resetValues();
    this.emit('load', { parameters: compiled.parameters, hot: false });
    if (this.currentState === 'error') this.setState(this.node ? 'running' : 'ready');
  }

  on<K extends keyof FlowsEvents>(type: K, listener: Listener<K>): () => void {
    let set = this.listeners.get(type);
    if (!set) this.listeners.set(type, (set = new Set()));
    set.add(listener as (event: never) => void);
    return () => this.off(type, listener);
  }
  off<K extends keyof FlowsEvents>(type: K, listener: Listener<K>): void {
    this.listeners.get(type)?.delete(listener as (event: never) => void);
  }

  /** Stops sound and releases the audio node. The shared context stays open. */
  dispose(): void {
    if (this.currentState === 'disposed') return;
    this.send({ type: 'dispose' });
    this.node?.port.close();
    this.node?.disconnect();
    this.gain?.disconnect();
    this.analyserNode?.disconnect();
    this.node = this.gain = this.analyserNode = null;
    for (const request of this.requests.values()) request.reject(new Error('This instrument was disposed'));
    this.requests.clear();
    instances.delete(this);
    this.setState('disposed');
    this.listeners.clear();
  }

  // --- internals ---

  private resetValues() {
    this.values.clear();
    for (const p of this.compiled.parameters) this.values.set(p.index, p.initial);
  }
  private emit<K extends keyof FlowsEvents>(type: K, event: FlowsEvents[K]) {
    // A copy: a listener may subscribe again (a knob rebinding on load) while this runs.
    for (const listener of [...(this.listeners.get(type) ?? [])]) {
      try { (listener as Listener<K>)(event); } catch (error) { console.error(error); }
    }
  }
  private setState(state: FlowsState) {
    if (state === this.currentState) return;
    const previous = this.currentState;
    this.currentState = state;
    this.emit('state', { state, previous });
  }
  private fail(error: Error) {
    this.error = error;
    this.setState('error');
    this.emit('error', { error });
  }
  private send(message: ToProcessor) {
    // Before the node exists, held notes and values are replayed on attach.
    if (this.node && this.attaching === null) this.node.port.postMessage(message);
  }

  /** @internal Called when an AudioContext changes state. */
  _contextChanged(context: BaseAudioContext) {
    if (context !== this.context || this.currentState === 'disposed' || this.currentState === 'loading' || this.currentState === 'error') return;
    if (context.state === 'running') {
      if (this.node && !this.attaching) this.setState('running');
      else void this.attach().catch(() => {});
    } else if (this.node && !this.attaching) this.setState('suspended');
  }

  private attach(): Promise<void> {
    if (this.node && !this.attaching) return Promise.resolve();
    this.attaching ??= this.createNode().then(
      () => { this.attaching = null; this.replay(); if (this.context?.state === 'running') this.setState('running'); else this.setState('suspended'); },
      (error) => { this.attaching = null; if (this.currentState !== 'disposed') this.fail(error as Error); throw error; });
    return this.attaching;
  }

  private async createNode() {
    const context = this.ownContext ?? sharedContext();
    const [{ module, bytes }] = await Promise.all([loadRuntime(), loadWorklet(context)]);
    if (this.currentState === 'disposed') throw new Error('This instrument was disposed');
    const options = (wasm: Pick<ProcessorOptions, 'module' | 'bytes'>): AudioWorkletNodeOptions => ({
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      processorOptions: { ...wasm, source: this.compiled.source, samples: this.compiled.samples.map(copyBuffer), boundarySeconds: this.boundary, bpm: this.bpmValue } satisfies ProcessorOptions,
    });
    let node: AudioWorkletNode;
    try {
      node = new AudioWorkletNode(context, 'flows-embed', options({ module }));
    } catch {
      // Browsers that cannot clone a WebAssembly.Module into the worklet get the bytes.
      node = new AudioWorkletNode(context, 'flows-embed', options({ bytes: bytes.slice(0) }));
    }
    await new Promise<void>((resolve, reject) => {
      // A processor that never became ready must stop rendering silence.
      const abandon = (error: Error) => { node.port.postMessage({ type: 'dispose' } satisfies ToProcessor); node.port.close(); reject(error); };
      const timer = setTimeout(() => abandon(new Error('The audio worklet did not start')), 15000);
      node.port.onmessage = (event: MessageEvent<FromProcessor>) => {
        const message = event.data;
        if (message.type === 'ready') {
          clearTimeout(timer);
          // Disposed while the worklet started: release it before it can connect.
          if (this.currentState === 'disposed') abandon(new Error('This instrument was disposed'));
          else resolve();
        }
        else if (message.type === 'error' && message.request === undefined && !this.node) { clearTimeout(timer); abandon(new Error(message.message)); }
        else this.receive(message);
      };
    });
    const gain = context.createGain();
    gain.gain.value = this.volumeValue;
    node.connect(gain);
    const destination = this.destinationOption === undefined ? context.destination : this.destinationOption;
    if (destination) gain.connect(destination);
    this.node = node;
    this.gain = gain;
    this.connectAnalyser();
  }
  private connectAnalyser() {
    if (!this.gain || this.analyserNode) return;
    const analyser = this.gain.context.createAnalyser();
    analyser.fftSize = this.analyserSize;
    analyser.smoothingTimeConstant = 0.6;
    this.gain.connect(analyser);
    this.analyserNode = analyser;
  }
  private replay() {
    for (const [index, value] of this.values) {
      const p = this.compiled.parameters.find((x) => x.index === index);
      if (p?.live && value !== p.initial) this.send({ type: 'param', index, value });
    }
    if (this.bpmValue !== 138) this.send({ type: 'bpm', bpm: this.bpmValue });
    // Notes pressed while audio was starting still sound.
    for (const [pitch, note] of this.held) this.send({ type: 'on', id: note.id, pitch, velocity: note.velocity });
  }
  private receive(message: FromProcessor) {
    switch (message.type) {
      case 'meter':
        this.peak = message.peak;
        this.voices = message.voices;
        this.emit('meter', { peak: message.peak, voices: message.voices });
        break;
      case 'loaded': this.requests.get(message.request)?.resolve(); this.requests.delete(message.request); break;
      case 'error': {
        const error = new Error(message.message);
        if (message.request !== undefined) {
          this.requests.get(message.request)?.reject(error);
          this.requests.delete(message.request);
        } else this.emit('error', { error });
        break;
      }
    }
  }
}

const clamp01 = (value: number) => (Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0.8);
const copyBuffer = (buffer: ArrayBuffer | null) => (buffer ? buffer.slice(0) : null);

export default Flows;
