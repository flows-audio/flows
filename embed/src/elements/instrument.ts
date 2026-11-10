// <flows-instrument preset="soft-keys" theme="glass">: a complete instrument.
//
// Source:  preset="name" | src="url.flows" | source="..." | a child
//          <script type="text/flows">...</script> | the .source property.
// Layout:  without children it shows a header, scope, knobs and keys, and
//          with the code attribute the editable source (<flows-code>). Put
//          your own <flows-code>, <flows-keys>, <flows-knob>, <flows-scope>,
//          <flows-play-gate> (or anything else) inside it to replace that layout.
// Styling: --flows-* custom properties, ::part(...) and slots "title" and "gate".
// Events:  flows-ready, flows-state, flows-error, flows-param, flows-note, flows-load.
import { Flows, FlowsCompileError, type CreateOptions, type FlowsSource, type FlowsState } from '../core';
import { Base, TOKENS, adoptStyles, define, escapeHtml, type InstrumentHost } from './shared';

const STYLE = String.raw`
:host { display: block; position: relative; }
.panel {
  position: relative;
  display: grid; gap: var(--flows-gap, 14px);
  padding: var(--flows-padding, 0);
  background: var(--flows-surface, transparent);
  border: var(--flows-border-width, 0px) solid var(--flows-border-color, transparent);
  border-radius: var(--flows-radius, 10px);
  box-shadow: var(--flows-shadow, none);
  -webkit-backdrop-filter: blur(var(--flows-blur, 0px)) saturate(var(--flows-saturate, 100%));
  backdrop-filter: blur(var(--flows-blur, 0px)) saturate(var(--flows-saturate, 100%));
}
.header { display: flex; align-items: center; gap: var(--flows-gap, 14px); min-height: 2em; flex-wrap: wrap; }
.title { font-weight: var(--flows-title-weight, 600); font-size: var(--flows-title-size, 1em); margin-right: auto; }
.status { font-size: var(--flows-status-size, .8em); color: var(--flows-muted, color-mix(in srgb, currentColor 60%, transparent)); }
.status:empty { display: none; }
.error {
  font-size: .85em; white-space: pre-wrap; padding: .6em .8em;
  border-radius: calc(var(--flows-radius, 10px) * .6);
  color: var(--flows-error, #c2410c); background: color-mix(in srgb, var(--flows-error, #c2410c) 10%, transparent);
}
.body { display: grid; gap: var(--flows-gap, 14px); }
:host([custom-layout]) .body { display: none; }
:host([scope="none"]) flows-scope, :host([controls="none"]) flows-controls { display: none; }
`;
const PARTS = {
  scope: 'scope, scope-canvas',
  controls: 'controls, knob, knob-dial, knob-track, knob-arc, knob-indicator, knob-label, knob-value',
  keys: 'keys, key, key-white, key-black, key-pressed, key-label',
  gate: 'gate, gate-button, gate-icon, gate-label',
  code: 'code, code-gutter, code-line-number, code-line-bad, code-view, code-input, code-param, code-problems, code-problem, code-reset',
};

export class FlowsInstrumentElement extends Base implements InstrumentHost {
  static observedAttributes = ['preset', 'src', 'source', 'label', 'controls', 'max-controls', 'start', 'octaves', 'scope', 'volume', 'bpm', 'labels', 'code', 'code-readonly'];
  private root: ShadowRoot | null = null;
  private instrument: Flows | null = null;
  private subscribers = new Set<(flows: Flows | null) => void>();
  private loading = 0;
  private sourceProperty: string | null = null;
  private pending = false;
  private readyPromise: Promise<Flows>;
  private resolveReady!: (flows: Flows) => void;
  private offs: (() => void)[] = [];

  constructor() {
    super();
    this.readyPromise = new Promise((resolve) => { this.resolveReady = resolve; });
  }

  /** The engine instance (see @flows-audio/embed/core), or null while loading. */
  get flows(): Flows | null { return this.instrument; }
  /** Resolves with the engine once the instrument has compiled. */
  get ready(): Promise<Flows> { return this.readyPromise; }
  get state(): FlowsState { return this.instrument?.state ?? (this.root?.querySelector('.error:not([hidden])') ? 'error' : 'loading'); }
  /** Flows source text. Setting it recompiles. */
  get source(): string { return this.instrument?.source ?? this.sourceProperty ?? ''; }
  set source(text: string) { this.sourceProperty = String(text); this.reload(); }

  subscribe(callback: (flows: Flows | null) => void): () => void {
    this.subscribers.add(callback);
    callback(this.instrument);
    return () => { this.subscribers.delete(callback); };
  }
  /** Starts sound; call from a user gesture. */
  start(): Promise<void> {
    return this.instrument ? this.instrument.start() : this.readyPromise.then((flows) => flows.start());
  }
  noteOn(note: number | string, velocity?: number) { return this.instrument?.noteOn(note, velocity); }
  noteOff(note: number | string) { this.instrument?.noteOff(note); }
  setParam(key: string, value: number) { return this.instrument?.setParam(key, value); }
  params() { return this.instrument?.params() ?? []; }

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.root.innerHTML = `
        <div class="panel" part="panel">
          <div class="header" part="header">
            <div class="title" part="title"><slot name="title"></slot></div>
            <span class="status" part="status" role="status" aria-live="polite"></span>
            <flows-play-gate exportparts="${PARTS.gate}"><slot name="gate">Tap to play</slot></flows-play-gate>
          </div>
          <div class="error" part="error" role="alert" hidden></div>
          <div class="body" part="body">
            <flows-scope exportparts="${PARTS.scope}"></flows-scope>
            <flows-controls exportparts="${PARTS.controls}"></flows-controls>
            <flows-keys exportparts="${PARTS.keys}"></flows-keys>
          </div>
          <slot></slot>
        </div>`;
      const content = this.root.querySelector('slot:not([name])') as HTMLSlotElement;
      content.addEventListener('slotchange', () => this.updateLayout(content));
      this.updateLayout(content);
      this.forwardAttributes();
    }
    if (!this.instrument && !this.pending) this.reload();
  }
  disconnectedCallback() {
    // Elements moved within the page reconnect in the same task; keep them alive.
    queueMicrotask(() => {
      if (this.isConnected) return;
      this.loading++; // a load still in flight disposes its result
      this.instrument?.dispose();
      this.setInstrument(null);
      this.readyPromise = new Promise((resolve) => { this.resolveReady = resolve; });
    });
  }
  attributeChangedCallback(name: string, previous: string | null, value: string | null) {
    if (!this.root || previous === value) return;
    if (name === 'preset' || name === 'src' || name === 'source') this.reload();
    else if (name === 'volume' && this.instrument) this.instrument.volume = Number(value ?? 0.8);
    else if (name === 'bpm' && this.instrument) this.instrument.bpm = Number(value ?? 138);
    else this.forwardAttributes();
  }

  private updateLayout(content: HTMLSlotElement) {
    const custom = content.assignedElements().some((element) => element.localName !== 'script' && element.localName !== 'template');
    this.toggleAttribute('custom-layout', custom);
    this.syncCode();
  }
  /** The default layout shows the source only with the code attribute; a custom layout places its own. */
  private syncCode() {
    const body = this.root?.querySelector('.body');
    if (!body) return;
    const wanted = this.hasAttribute('code') && !this.hasAttribute('custom-layout');
    let code = body.querySelector('flows-code');
    if (wanted && !code) {
      code = document.createElement('flows-code');
      code.setAttribute('exportparts', PARTS.code);
      body.prepend(code);
    } else if (!wanted && code) code.remove();
    if (code) code.toggleAttribute('readonly', this.hasAttribute('code-readonly'));
  }
  private forwardAttributes() {
    const root = this.root!;
    const keys = root.querySelector('flows-keys')!, controls = root.querySelector('flows-controls')!, scope = root.querySelector('flows-scope')!;
    const copy = (target: Element, from: string, to = from) => {
      const value = this.getAttribute(from);
      if (value === null) target.removeAttribute(to); else target.setAttribute(to, value);
    };
    copy(keys, 'start'); copy(keys, 'octaves'); copy(keys, 'labels');
    this.syncCode();
    const list = this.getAttribute('controls');
    if (list && list !== 'auto') controls.setAttribute('params', list); else controls.removeAttribute('params');
    copy(controls, 'max-controls', 'max');
    const mode = this.getAttribute('scope');
    if (mode === 'spectrum') scope.setAttribute('mode', 'spectrum'); else scope.removeAttribute('mode');
    this.updateTitle();
  }
  private updateTitle() {
    const slot = this.root?.querySelector('slot[name="title"]');
    if (!slot) return;
    const preset = this.getAttribute('preset');
    slot.textContent = this.getAttribute('label') ?? (preset ? preset.replace(/-/g, ' ').replace(/^\w/, (c) => c.toUpperCase()) : '');
  }

  private options(): FlowsSource {
    const inline = this.querySelector('script[type="text/flows"], script[type="text/x-flows"]');
    if (this.sourceProperty !== null) return { source: this.sourceProperty };
    if (this.hasAttribute('source')) return { source: this.getAttribute('source')! };
    if (inline) return { source: dedent(inline.textContent ?? '') };
    if (this.hasAttribute('src')) return { src: this.getAttribute('src')! };
    return { preset: this.getAttribute('preset') ?? undefined };
  }

  private reload() {
    if (!this.root) return;
    // Batch attribute changes made in the same task.
    if (this.pending) return;
    this.pending = true;
    queueMicrotask(() => { this.pending = false; if (this.isConnected) void this.load(); });
  }
  private async load() {
    const request = ++this.loading;
    const source = this.options();
    this.showError(null);
    this.setStatus('Loading…');
    try {
      if (this.instrument && this.instrument.state !== 'error' && this.instrument.state !== 'disposed') {
        await this.instrument.load(source);
        if (request !== this.loading) return;
        this.setStatus(statusText(this.instrument.state));
        this.emit('flows-load', { flows: this.instrument });
        return;
      }
      const options: CreateOptions = { ...source };
      const volume = Number(this.getAttribute('volume'));
      if (this.hasAttribute('volume') && Number.isFinite(volume)) options.volume = volume;
      const bpm = Number(this.getAttribute('bpm'));
      if (this.hasAttribute('bpm') && Number.isFinite(bpm)) options.bpm = bpm;
      const flows = await Flows.create(options);
      if (request !== this.loading || !this.isConnected) { flows.dispose(); return; }
      this.instrument?.dispose();
      this.setInstrument(flows);
      this.resolveReady(flows);
      this.emit('flows-ready', { flows });
    } catch (error) {
      if (request !== this.loading) return;
      this.showError(error as Error);
      this.emit('flows-error', { error });
    }
  }

  private setInstrument(flows: Flows | null) {
    for (const off of this.offs) off();
    this.offs = [];
    this.instrument = flows;
    if (flows) {
      this.offs.push(
        flows.on('state', ({ state, previous }) => { this.setStatus(statusText(state)); this.emit('flows-state', { state, previous }); }),
        flows.on('error', ({ error }) => { this.showError(error); this.emit('flows-error', { error }); }),
        flows.on('param', ({ parameter, value }) => this.emit('flows-param', { key: parameter.key, parameter, value })),
        flows.on('note', (detail) => this.emit('flows-note', detail)),
      );
      this.setStatus(statusText(flows.state));
    }
    for (const subscriber of this.subscribers) subscriber(flows);
  }
  private emit(type: string, detail: unknown) {
    this.dispatchEvent(new CustomEvent(type, { detail, bubbles: true, composed: true }));
  }
  private setStatus(text: string) {
    const status = this.root?.querySelector('.status');
    if (status) status.textContent = text;
  }
  private showError(error: Error | null) {
    const box = this.root?.querySelector('.error') as HTMLElement | null;
    if (!box) return;
    box.hidden = !error;
    if (!error) { box.textContent = ''; return; }
    this.setStatus('');
    box.innerHTML = error instanceof FlowsCompileError
      ? `<strong>The instrument has errors</strong>\n${escapeHtml(error.message)}`
      : escapeHtml(error.message);
  }
}

function statusText(state: FlowsState): string {
  switch (state) {
    case 'loading': return 'Loading…';
    case 'starting': return 'Starting sound…';
    case 'suspended': return 'Sound paused';
    case 'running': return 'Sound on';
    default: return '';
  }
}
// Inline <script type="text/flows"> blocks are usually indented with the page.
function dedent(text: string): string {
  const lines = text.replace(/^\s*\n/, '').replace(/\s+$/, '').split('\n');
  const indent = Math.min(...lines.filter((line) => line.trim()).map((line) => /^[ \t]*/.exec(line)![0].length));
  return lines.map((line) => line.slice(Number.isFinite(indent) ? indent : 0)).join('\n') + '\n';
}

export function defineInstrument() { define('flows-instrument', FlowsInstrumentElement); }
