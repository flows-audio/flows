// <flows-code>: the instrument's Flows source, editable, beside its sound.
//
// Typing recompiles when you pause (or at once with Ctrl/Cmd+Enter). Numbers
// that set a parameter are live: drag one sideways to change it while playing
// (Shift for fine), Alt+Up/Down nudges the number at the caret, and a knob
// turned elsewhere rewrites its number here. Edits that only change live
// values keep the sound running; others rebuild the instrument. Problems are
// marked in the code and listed below it; the last good version keeps playing.
//
// Once edited, a Reset button (part code-reset) brings back the source as it
// was loaded; the host has a `modified` attribute meanwhile. When the code is
// taller than its box (--flows-code-max-height), a number changed elsewhere
// (a knob) scrolls into view and lights up.
//
// Attributes: instrument="id" (else the enclosing <flows-instrument>),
// readonly, delay="ms" (pause before recompiling, default 400).
// Property: value, original. Methods: apply(), reset().
// Events: flows-apply {source, ok, diagnostics}.
import type { Flows } from '../core';
import { formatLiteral, fromPosition, toPosition, type FlowsParameter } from '../params';
import type { Diagnostic } from '../wasm';
import { Base, InstrumentLink, TOKENS, adoptStyles, define, escapeHtml } from './shared';

const STYLE = String.raw`
:host { display: block; min-width: 0; }
.frame { position: relative; }
.reset {
  position: absolute; top: 8px; right: 8px; z-index: 3; margin: 0; cursor: pointer;
  font-family: var(--flows-code-font, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-size: calc(var(--flows-code-size, 13px) * .85); line-height: 1; padding: 5px 10px; border-radius: 999px;
  color: var(--flows-code-text, inherit);
  background: var(--flows-code-background, var(--flows-surface-raised, color-mix(in srgb, currentColor 5%, transparent)));
  border: var(--flows-border-width, 1px) solid var(--flows-code-border, var(--flows-border-color, color-mix(in srgb, currentColor 14%, transparent)));
}
.reset:hover { border-color: currentColor; }
.code {
  display: flex; align-items: flex-start; overflow: auto;
  max-height: var(--flows-code-max-height, none);
  font-family: var(--flows-code-font, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-size: var(--flows-code-size, 13px); line-height: var(--flows-code-line-height, 1.6);
  color: var(--flows-code-text, inherit);
  background: var(--flows-code-background, var(--flows-surface-raised, color-mix(in srgb, currentColor 5%, transparent)));
  border: var(--flows-border-width, 1px) solid var(--flows-code-border, var(--flows-border-color, color-mix(in srgb, currentColor 14%, transparent)));
  border-radius: var(--flows-code-radius, calc(var(--flows-radius, 10px) * .6));
  tab-size: 4;
}
.gutter {
  position: sticky; left: 0; z-index: 2; flex: none;
  padding: var(--flows-code-padding, 12px) 10px var(--flows-code-padding, 12px) 12px;
  text-align: right; user-select: none; font-variant-numeric: tabular-nums;
  color: var(--flows-code-gutter, color-mix(in srgb, currentColor 38%, transparent));
  background: inherit;
}
.gutter span { display: block; }
.gutter .bad { color: var(--flows-error, #d9480f); }
.area { position: relative; display: grid; flex: 1 0 auto; min-width: 0; }
.view, .input {
  grid-area: 1 / 1; margin: 0; border: 0; min-width: 100%;
  padding: var(--flows-code-padding, 12px) var(--flows-code-padding, 12px) var(--flows-code-padding, 12px) 4px;
  font: inherit; line-height: inherit; letter-spacing: inherit; tab-size: inherit;
  white-space: pre; overflow-wrap: normal; word-break: normal;
}
.view { position: relative; z-index: 1; pointer-events: none; color: inherit; }
.input {
  resize: none; overflow: hidden; background: transparent; color: transparent; outline: none;
  caret-color: var(--flows-code-caret, var(--flows-accent, currentColor));
  -webkit-text-fill-color: transparent;
}
.input::selection { background: var(--flows-code-selection, color-mix(in srgb, var(--flows-accent, currentColor) 26%, transparent)); }
.code:focus-within {
  outline: var(--flows-focus-width, 2px) solid var(--flows-focus, var(--flows-accent, currentColor));
  outline-offset: 2px;
}
.keyword { color: var(--flows-syntax-keyword, color-mix(in srgb, #c026d3 72%, currentColor)); }
.type { color: var(--flows-syntax-type, color-mix(in srgb, #0891b2 72%, currentColor)); }
.function { color: var(--flows-syntax-function, color-mix(in srgb, #2563eb 72%, currentColor)); }
.property { color: var(--flows-syntax-property, color-mix(in srgb, #059669 72%, currentColor)); }
.number { color: var(--flows-syntax-number, color-mix(in srgb, #ea580c 75%, currentColor)); }
.string { color: var(--flows-syntax-string, color-mix(in srgb, #16a34a 72%, currentColor)); }
.comment { color: var(--flows-syntax-comment, color-mix(in srgb, currentColor 50%, transparent)); font-style: italic; }
.operator { color: var(--flows-syntax-operator, color-mix(in srgb, currentColor 62%, transparent)); }
.param {
  pointer-events: auto; cursor: ew-resize; touch-action: none; border-radius: 3px;
  background: var(--flows-code-param, color-mix(in srgb, currentColor 7%, transparent));
  box-shadow: 0 1px 0 var(--flows-code-param-line, color-mix(in srgb, currentColor 35%, transparent));
}
.param:hover, .param.active {
  background: var(--flows-code-param-active, color-mix(in srgb, var(--flows-accent, currentColor) 22%, transparent));
}
:host([readonly]) .param { cursor: default; pointer-events: none; }
.bad-text { text-decoration: wavy underline var(--flows-error, #d9480f); text-decoration-skip-ink: none; text-underline-offset: 3px; }
.problems {
  margin: 8px 0 0; padding: 0; list-style: none;
  font-family: var(--flows-code-font, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-size: calc(var(--flows-code-size, 13px) * .92); line-height: 1.5;
  color: var(--flows-error, #d9480f);
}
.problems:empty { display: none; }
.problems button { all: unset; cursor: pointer; text-decoration: underline; text-underline-offset: 2px; }
`;

// The Flows language as the editor in the Flows app colours it.
const RULES: [RegExp, string][] = [
  [/#.*/y, 'comment'],
  [/"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'/y, 'string'],
  [/\b(?:import|from|as|return|class|def|if|else|and|or|not)\b/y, 'keyword'],
  [/\b(?:Inst|std|Classic\w+|self)\b/y, 'type'],
  [/(?:\d+(?:\.\d*)?|\.\d+)(?:khz|hz|ms|db|s)?\b/iy, 'number'],
  [/\b(?:sigstream|poly|mono|make|probe|power|spec|spectrum|mel_spec|stereo|left|right|mid|side|register_knob)\b/y, 'function'],
  [/\b(?:pitch|gate|velocity|trigger|cutoff|resonance|level|unison|file|root|rate|start|end|mode|reverse|A|D|S|R)\b/y, 'property'],
  [/[+*/=<>!:-]/y, 'operator'],
  [/[A-Za-z_]\w*/y, ''],
];
interface Token { from: number; to: number; kind: string }
export function tokenize(text: string): Token[] {
  const tokens: Token[] = [];
  let at = 0;
  while (at < text.length) {
    let matched = false;
    if (/\s/.test(text[at])) {
      let end = at + 1;
      while (end < text.length && /\s/.test(text[end])) end++;
      tokens.push({ from: at, to: end, kind: '' });
      at = end;
      continue;
    }
    for (const [rule, kind] of RULES) {
      rule.lastIndex = at;
      const match = rule.exec(text);
      if (match && match[0].length) {
        tokens.push({ from: at, to: at + match[0].length, kind });
        at += match[0].length;
        matched = true;
        break;
      }
    }
    if (!matched) tokens.push({ from: at, to: ++at, kind: '' });
  }
  return tokens;
}

interface Span { index: number; from: number; to: number }
interface Problem { from: number; to: number; line: number; column: number; message: string }

/** Highlighted HTML for `text`: live numbers wrapped for dragging, problems underlined. */
export function highlight(text: string, spans: readonly Span[], problems: readonly Problem[]): string {
  const cuts = new Set<number>();
  for (const s of spans) { cuts.add(s.from); cuts.add(s.to); }
  for (const p of problems) { cuts.add(p.from); cuts.add(p.to); }
  let html = '';
  let open: Span | null = null;
  for (const token of tokenize(text)) {
    const points = [token.from, ...[...cuts].filter((c) => c > token.from && c < token.to).sort((a, b) => a - b), token.to];
    for (let i = 0; i < points.length - 1; i++) {
      const from = points[i], to = points[i + 1];
      const span = spans.find((s) => s.from <= from && to <= s.to) ?? null;
      if (open && span !== open) { html += '</span>'; open = null; }
      if (span && !open) { html += `<span class="param" part="code-param" data-param="${span.index}">`; open = span; }
      const bad = problems.some((p) => p.from < to && from < p.to);
      const classes = [token.kind, bad ? 'bad-text' : ''].filter(Boolean).join(' ');
      const piece = escapeHtml(text.slice(from, to));
      html += classes ? `<span class="${classes}">${piece}</span>` : piece;
    }
  }
  if (open) html += '</span>';
  return html;
}

const lineStart = (text: string, line: number) => {
  let at = 0;
  for (let n = 1; n < line && at >= 0; n++) at = text.indexOf('\n', at) + 1 || -1;
  return Math.max(0, at);
};
function toProblem(text: string, d: Diagnostic): Problem {
  let from = d.from >= 0 ? d.from : lineStart(text, d.line) + Math.max(0, d.column - 1);
  let to = d.to > from ? d.to : from;
  from = Math.min(from, text.length);
  if (to <= from) {
    // An empty span marks the character there, or the line's end.
    const end = text.indexOf('\n', from);
    to = Math.min(text.length, from < (end < 0 ? text.length : end) ? from + 1 : from);
    if (to === from && from > 0) from--;
  }
  return { from, to, line: d.line, column: d.column, message: d.message };
}

export class FlowsCodeElement extends Base {
  static observedAttributes = ['instrument', 'readonly'];
  private link = new InstrumentLink(this, (flows) => this.bind(flows));
  private root: ShadowRoot | null = null;
  private flows: Flows | null = null;
  private offs: (() => void)[] = [];
  private text = '';
  private spans: Span[] = [];
  private params: FlowsParameter[] = [];
  private problems: Problem[] = [];
  private timer = 0;
  private applying = 0;
  private escaped = false;
  private source = '';
  private flash = 0;
  private drag: { pointer: number; span: Span; parameter: FlowsParameter; x: number; start: number; position: number; value: number; moved: boolean } | null = null;

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.root.innerHTML = `
        <div class="frame">
        <button type="button" class="reset" part="code-reset" hidden title="Bring back the code as it was loaded">Reset</button>
        <div class="code" part="code">
          <div class="gutter" part="code-gutter" aria-hidden="true"></div>
          <div class="area">
            <pre class="view" part="code-view" aria-hidden="true"></pre>
            <textarea class="input" part="code-input" spellcheck="false" autocapitalize="off" autocomplete="off"
              autocorrect="off" wrap="off" aria-label="Flows source code"
              aria-describedby="hint"></textarea>
          </div>
        </div>
        </div>
        <span class="sr-only" id="hint">Edits play when you pause typing, or press Control or Command with Enter. Alt with the up or down arrow changes the number at the cursor. Press Escape, then Tab, to leave the editor.</span>
        <ul class="problems" part="code-problems" role="status" aria-live="polite"></ul>`;
      const input = this.input;
      input.addEventListener('input', () => this.typed());
      input.addEventListener('keydown', (event) => this.keyDown(event));
      // Typing stays here. A page's own shortcuts (a letter, /, ?) see the event
      // retargeted to <flows-code>, not the textarea, so they cannot tell it is typing.
      // Escape and Command/Control combinations still reach the page.
      for (const type of ['keydown', 'keypress', 'keyup'] as const)
        input.addEventListener(type, (event) => { if (event.key !== 'Escape' && !event.metaKey && !event.ctrlKey) event.stopPropagation(); });
      const view = this.root.querySelector('.view') as HTMLElement;
      view.addEventListener('pointerdown', (event) => this.pointerDown(event));
      view.addEventListener('pointermove', (event) => this.pointerMove(event));
      for (const type of ['pointerup', 'pointercancel'] as const) view.addEventListener(type, (event) => this.pointerUp(event, type === 'pointercancel'));
      view.addEventListener('dblclick', (event) => this.selectLiteral(event));
      this.root.querySelector('.reset')!.addEventListener('click', () => { void this.reset(); });
      this.root.querySelector('.problems')!.addEventListener('click', (event) => {
        const at = (event.target as HTMLElement).closest<HTMLElement>('[data-at]')?.dataset.at;
        if (at !== undefined) { input.focus(); input.setSelectionRange(Number(at), Number(at)); }
      });
    }
    this.input.readOnly = this.hasAttribute('readonly');
    this.link.connect();
  }
  disconnectedCallback() {
    this.link.disconnect();
    clearTimeout(this.timer);
  }
  attributeChangedCallback(name: string) {
    if (!this.root) return;
    if (name === 'instrument') this.link.connect();
    if (name === 'readonly') this.input.readOnly = this.hasAttribute('readonly');
  }

  /** The source as edited here (it may be ahead of the instrument while you type). */
  get value(): string { return this.text; }
  set value(text: string) {
    this.setText(String(text), []);
    void this.apply();
  }
  /** The source as it was loaded (src, preset or source), before any edit here. */
  get original(): string { return this.source; }
  /** Brings back the original source and plays it. */
  reset(): Promise<boolean> {
    if (!this.source || this.text === this.source) return Promise.resolve(true);
    this.problems = [];
    this.setText(this.source, []);
    this.showProblems([]);
    return this.apply();
  }
  /** Compiles and plays the current text now. */
  apply(): Promise<boolean> {
    clearTimeout(this.timer);
    this.timer = 0;
    const flows = this.flows;
    if (!flows) return Promise.resolve(false);
    const text = this.text, request = ++this.applying;
    return flows.load({ source: text }).then(() => {
      if (request === this.applying) this.showProblems([]);
      this.emit({ source: text, ok: true, diagnostics: [] });
      return true;
    }, (error: Error & { diagnostics?: Diagnostic[] }) => {
      const diagnostics = error.diagnostics ?? [{ line: 1, column: 1, from: -1, to: -1, message: error.message }];
      if (request === this.applying) this.showProblems(diagnostics);
      this.emit({ source: text, ok: false, diagnostics });
      return false;
    });
  }

  private get input() { return this.root!.querySelector('.input') as HTMLTextAreaElement; }

  private bind(flows: Flows | null) {
    for (const off of this.offs) off();
    this.offs = [];
    this.flows = flows;
    if (!flows) return;
    this.offs.push(flows.on('load', () => this.loaded(flows)));
    this.offs.push(flows.on('param', ({ parameter, value }) => this.rewrite(parameter, value)));
    this.loaded(flows);
  }

  /** The instrument compiled: adopt its source (unless it is ours) and its parameter spans. */
  private loaded(flows: Flows) {
    this.params = flows.params();
    const source = flows.source;
    if (source === this.text) {
      this.spans = this.params.filter((p) => p.from >= 0).map((p) => ({ index: p.index, from: p.from, to: p.to }));
      this.render();
      // Values the page set before the code bound (knobs) show in the text.
      for (const p of flows.params()) if (p.from >= 0 && p.value !== p.initial) this.rewrite(p, p.value);
    } else if (!this.timer && !this.drag) {
      // A load from elsewhere (new src, preset or source): show it, and keep it for Reset.
      this.source = source;
      this.problems = [];
      this.setText(source, this.params.filter((p) => p.from >= 0).map((p) => ({ index: p.index, from: p.from, to: p.to })));
      this.showProblems([]);
      for (const p of flows.params()) if (p.from >= 0 && p.value !== p.initial) this.rewrite(p, p.value);
    }
  }

  private setText(text: string, spans: Span[]) {
    this.text = text;
    this.spans = spans;
    const input = this.input;
    if (input.value !== text) {
      const { selectionStart, selectionEnd } = input;
      input.value = text;
      if (document.activeElement === this || this.root!.activeElement === input) input.setSelectionRange(selectionStart, selectionEnd);
    }
    this.render();
  }

  private render() {
    if (!this.root) return;
    const view = this.root.querySelector('.view') as HTMLElement;
    // A trailing newline keeps the last empty line as tall as the textarea's.
    view.innerHTML = highlight(this.text, this.spans, this.problems) + '\n';
    const lines = this.text.split('\n').length;
    const bad = new Set(this.problems.map((p) => this.text.slice(0, p.from).split('\n').length));
    let gutter = '';
    for (let n = 1; n <= lines; n++) gutter += bad.has(n) ? `<span class="bad" part="code-line-number code-line-bad">${n}</span>` : `<span part="code-line-number">${n}</span>`;
    (this.root.querySelector('.gutter') as HTMLElement).innerHTML = gutter;
    const modified = !!this.source && this.text !== this.source;
    this.toggleAttribute('modified', modified);
    (this.root.querySelector('.reset') as HTMLElement).hidden = !modified || this.hasAttribute('readonly');
  }

  private showProblems(diagnostics: readonly Diagnostic[]) {
    this.problems = diagnostics.map((d) => toProblem(this.text, d));
    const list = this.root!.querySelector('.problems') as HTMLElement;
    list.innerHTML = this.problems.map((p) =>
      `<li part="code-problem"><button type="button" data-at="${p.from}">Line ${p.line}</button>: ${escapeHtml(p.message)}</li>`).join('');
    this.render();
  }

  /** Text typed: keep the spans that the edit did not touch, then recompile after a pause. */
  private typed() {
    const next = this.input.value, previous = this.text;
    let start = 0;
    while (start < next.length && start < previous.length && next[start] === previous[start]) start++;
    let endNext = next.length, endPrevious = previous.length;
    while (endNext > start && endPrevious > start && next[endNext - 1] === previous[endPrevious - 1]) { endNext--; endPrevious--; }
    const delta = next.length - previous.length;
    const moved = (s: { from: number; to: number }) => s.to <= start ? s : s.from >= endPrevious ? { ...s, from: s.from + delta, to: s.to + delta } : null;
    this.spans = this.spans.map((s) => moved(s) as Span | null).filter((s): s is Span => !!s);
    this.problems = this.problems.map((p) => moved(p) as Problem | null).filter((p): p is Problem => !!p);
    this.text = next;
    this.render();
    this.schedule();
  }
  private schedule() {
    clearTimeout(this.timer);
    const delay = Number(this.getAttribute('delay') ?? 400);
    this.timer = window.setTimeout(() => { this.timer = 0; void this.apply(); }, Number.isFinite(delay) ? Math.max(0, delay) : 400);
  }

  /** A parameter changed (a knob, a drag, the page): write its number into the source. */
  private rewrite(parameter: FlowsParameter, value: number) {
    const span = this.spans.find((s) => s.index === parameter.index);
    if (!span) return;
    const original = this.text.slice(span.from, span.to);
    // The number already says this (perhaps written differently, as typed): leave it.
    if (Math.abs(this.literalValue(parameter, span) - value) <= Math.abs(value) * 1e-9) return;
    const literal = formatLiteral(parameter, value, original);
    if (literal === original) return;
    this.replaceSpan(span, literal);
    // Keep the instrument's source in step; live values apply in place.
    if (!this.drag) { this.schedule(); this.reveal(parameter.index); }
  }
  /** A number changed from outside (a knob): light it up, and scroll it into view in a scrolling box. */
  private reveal(index: number) {
    const root = this.root!;
    const number = root.querySelector<HTMLElement>(`.param[data-param="${index}"]`);
    if (!number) return;
    number.classList.add('active');
    clearTimeout(this.flash);
    this.flash = window.setTimeout(() => { if (!this.drag) root.querySelectorAll('.param.active').forEach((element) => element.classList.remove('active')); }, 700);
    const box = root.querySelector('.code') as HTMLElement;
    const outer = box.getBoundingClientRect(), at = number.getBoundingClientRect();
    if (at.top >= outer.top && at.bottom <= outer.bottom) return;
    const still = typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;
    box.scrollTo({ top: box.scrollTop + at.top - outer.top - (outer.height - at.height) / 2, behavior: still ? 'auto' : 'smooth' });
  }
  private replaceSpan(span: Span, literal: string) {
    const delta = literal.length - (span.to - span.from);
    const text = this.text.slice(0, span.from) + literal + this.text.slice(span.to);
    const input = this.input;
    const { selectionStart, selectionEnd } = input;
    const shift = (at: number) => (at >= span.to ? at + delta : at);
    const spans = this.spans.map((s) => s === span ? { ...s, to: s.to + delta } : s.from >= span.to ? { ...s, from: s.from + delta, to: s.to + delta } : s);
    this.problems = this.problems.map((p) => p.from >= span.to ? { ...p, from: p.from + delta, to: p.to + delta } : p);
    this.text = text;
    this.spans = spans;
    input.value = text;
    input.setSelectionRange(shift(selectionStart), shift(selectionEnd));
    this.render();
    if (this.drag) this.drag.span = spans.find((s) => s.index === span.index)!;
  }

  private parameterAt(index: number) { return this.params.find((p) => p.index === index) ?? null; }
  private currentValue(p: FlowsParameter) { return this.flows?.getParam(p.key) ?? p.initial; }

  private pointerDown(event: PointerEvent) {
    const target = (event.target as HTMLElement).closest<HTMLElement>('.param');
    if (!target || event.button !== 0 || this.hasAttribute('readonly')) return;
    const span = this.spans.find((s) => s.index === Number(target.dataset.param));
    const parameter = span && this.parameterAt(span.index);
    if (!span || !parameter) return;
    event.preventDefault();
    target.setPointerCapture(event.pointerId);
    target.classList.add('active');
    const value = parameter.live ? this.currentValue(parameter) : Number.NaN;
    this.drag = { pointer: event.pointerId, span, parameter, x: event.clientX, start: event.clientX, position: toPosition(parameter, value), value, moved: false };
    if (!parameter.live) this.drag.value = this.literalValue(parameter, span);
    this.drag.position = toPosition(parameter, this.drag.value);
    // Make sure there is sound to hear while dragging.
    if (this.flows && this.flows.state !== 'running') void this.flows.start().catch(() => {});
  }
  private literalValue(p: FlowsParameter, span: Span) {
    const text = this.text.slice(span.from, span.to);
    const match = /^(-?(?:\d+(?:\.\d*)?|\.\d+))([a-z]*)$/i.exec(text);
    if (!match) return p.initial;
    const scale = ({ khz: 0.001, ms: 1000 } as Record<string, number>)[match[2].toLowerCase()] ?? 1;
    return Number(match[1]) / scale;
  }
  private pointerMove(event: PointerEvent) {
    const drag = this.drag;
    if (!drag || event.pointerId !== drag.pointer) return;
    const pixels = event.clientX - drag.x;
    drag.x = event.clientX;
    if (!pixels) return;
    if (Math.abs(event.clientX - drag.start) > 2) drag.moved = true;
    this.adjust(drag.parameter, pixels * (event.shiftKey ? 0.1 : 1));
  }
  /** Moves a parameter by `pixels` of travel: 240 px covers a declared range, otherwise 1% of the value per pixel. */
  private adjust(p: FlowsParameter, pixels: number) {
    const drag = this.drag;
    const current = drag ? drag.value : this.currentValue(p);
    let value: number;
    if (p.hasRange) {
      const travel = p.discrete ? Math.max(12, ((p.max - p.min) / Math.max(1, p.step)) * 12) : 240;
      const position = Math.max(0, Math.min(1, (drag ? drag.position : toPosition(p, current)) + pixels / travel));
      if (drag) drag.position = position;
      value = fromPosition(p, position);
    } else {
      const step = p.discrete ? Math.max(1, Math.round(p.step)) : p.step;
      const raw = current + pixels * Math.max(Math.abs(current), 1) * 0.01;
      value = Math.max(p.min, Math.min(p.max, Number((Math.round(raw / step) * step).toPrecision(12))));
    }
    if (drag) drag.value = value;
    if (p.live && this.flows) this.flows.setParam(p.key, value); // the param event rewrites the number
    else {
      const span = drag?.span ?? this.spans.find((s) => s.index === p.index);
      if (span) this.replaceSpan(span, formatLiteral(p, value, this.text.slice(span.from, span.to)));
    }
  }
  private pointerUp(event: PointerEvent, cancelled: boolean) {
    const drag = this.drag;
    if (!drag || event.pointerId !== drag.pointer) return;
    this.drag = null;
    this.root!.querySelectorAll('.param.active').forEach((element) => element.classList.remove('active'));
    if (drag.moved && !cancelled) { this.schedule(); return; }
    if (!drag.moved) {
      // A click without a drag places the caret, as in any text.
      const target = (event.target as HTMLElement).closest<HTMLElement>('.param');
      const rect = target?.getBoundingClientRect();
      const at = rect && rect.width ? drag.span.from + Math.round(((event.clientX - rect.left) / rect.width) * (drag.span.to - drag.span.from)) : drag.span.to;
      this.input.focus();
      this.input.setSelectionRange(at, at);
    }
  }
  private selectLiteral(event: MouseEvent) {
    const target = (event.target as HTMLElement).closest<HTMLElement>('.param');
    const span = target && this.spans.find((s) => s.index === Number(target.dataset.param));
    if (!span) return;
    this.input.focus();
    this.input.setSelectionRange(span.from, span.to);
  }

  private keyDown(event: KeyboardEvent) {
    const input = this.input;
    if (event.key === 'Escape') { this.escaped = true; return; }
    if (event.key === 'Tab' && this.escaped) { this.escaped = false; return; }
    this.escaped = false;
    if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); void this.apply(); return; }
    if (input.readOnly) return;
    if (event.altKey && (event.key === 'ArrowUp' || event.key === 'ArrowDown')) {
      const at = input.selectionStart;
      const span = this.spans.find((s) => s.from <= at && at <= s.to);
      const p = span && this.parameterAt(span.index);
      if (!span || !p) return;
      event.preventDefault();
      // One step of a discrete value; else 1% of the travel (0.1% with Shift).
      const pixels = p.discrete && p.hasRange ? 12 : (p.hasRange ? 2.4 : 1) * (event.shiftKey ? 0.1 : 1);
      this.adjust(p, event.key === 'ArrowUp' ? pixels : -pixels);
      if (!p.live) this.schedule();
      return;
    }
    if (event.key === 'Tab') {
      event.preventDefault();
      this.insert(event.shiftKey ? '' : '    ', event.shiftKey);
      return;
    }
    if (event.key === 'Enter' && !event.shiftKey && !event.altKey) {
      // Keep the indentation; one level deeper after a colon.
      const before = input.value.slice(0, input.selectionStart);
      const line = before.slice(before.lastIndexOf('\n') + 1);
      const indent = /^[ \t]*/.exec(line)![0] + (/:\s*$/.test(line) ? '    ' : '');
      event.preventDefault();
      this.insert(`\n${indent}`);
    }
  }
  /** Inserts text at the caret through the browser's editing, so undo still works. */
  private insert(text: string, outdent = false) {
    const input = this.input;
    if (outdent) {
      const start = input.value.lastIndexOf('\n', input.selectionStart - 1) + 1;
      const spaces = /^ {1,4}/.exec(input.value.slice(start))?.[0].length ?? 0;
      if (!spaces) return;
      input.setSelectionRange(start, start + spaces);
      text = '';
    }
    let done = false;
    try { done = document.execCommand('insertText', false, text); } catch { done = false; }
    if (!done) {
      input.setRangeText(text, input.selectionStart, input.selectionEnd, 'end');
      this.typed();
    }
  }

  private emit(detail: { source: string; ok: boolean; diagnostics: Diagnostic[] }) {
    this.dispatchEvent(new CustomEvent('flows-apply', { bubbles: true, composed: true, detail }));
  }
}

export const defineCode = () => define('flows-code', FlowsCodeElement);
