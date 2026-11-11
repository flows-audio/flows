// <flows-knob param="cutoff">: one parameter as an accessible slider.
// Drag up/right to raise (Shift for fine), arrows/PageUp/PageDown/Home/End,
// double-click or Delete to restore the value written in the source.
import type { Flows } from '../core';
import { defaultControls, findParameter, formatValue, fromPosition, toPosition, type FlowsParameter } from '../params';
import { Base, InstrumentLink, TOKENS, adoptStyles, define, escapeHtml } from './shared';

const STYLE = String.raw`
:host { display: inline-block; }
.knob {
  display: grid; justify-items: center; gap: 4px;
  width: max-content; min-width: var(--flows-knob-size, 48px);
}
.dial {
  position: relative; width: var(--flows-knob-size, 48px); height: var(--flows-knob-size, 48px);
  border-radius: 50%; cursor: ns-resize; touch-action: none;
  background: var(--flows-knob-face, var(--flows-surface-raised, transparent));
  box-shadow: var(--flows-knob-shadow, none);
}
:host([disabled]) .dial, .dial[aria-disabled="true"] { cursor: default; opacity: .5; }
svg { position: absolute; inset: 0; width: 100%; height: 100%; overflow: visible; }
.track, .arc { fill: none; stroke-linecap: round; stroke-width: var(--flows-knob-stroke, 8); }
.track { stroke: var(--flows-knob-track, color-mix(in srgb, currentColor 16%, transparent)); }
.arc {
  stroke: var(--flows-knob-value, var(--flows-accent, currentColor));
  filter: drop-shadow(0 0 4px var(--flows-glow, transparent));
  transition: stroke-dasharray var(--flows-transition, 90ms) ease;
}
.indicator { stroke: var(--flows-knob-indicator, var(--flows-text, currentColor)); stroke-width: 6; stroke-linecap: round; }
.label { font-size: var(--flows-label-size, 12px); line-height: 1.2; text-align: center; }
.value {
  font-size: var(--flows-value-size, 11px); line-height: 1.2; font-variant-numeric: tabular-nums;
  color: var(--flows-muted, color-mix(in srgb, currentColor 60%, transparent));
}
`;
// A 270° arc from 7:30 to 4:30, as a path of length 100.
const ARC = 'M 21.7 78.3 A 40 40 0 1 1 78.3 78.3';

export class FlowsKnobElement extends Base {
  static observedAttributes = ['param', 'label', 'instrument'];
  private link = new InstrumentLink(this, (flows) => this.bind(flows));
  private root: ShadowRoot | null = null;
  private parameter: FlowsParameter | null = null;
  private value = 0;
  private offs: (() => void)[] = [];
  private drag: { id: number; x: number; y: number; position: number } | null = null;

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.root.innerHTML = `
        <div class="knob" part="knob">
          <div class="dial" part="knob-dial" role="slider" tabindex="0" aria-disabled="true">
            <svg viewBox="0 0 100 100" aria-hidden="true">
              <path class="track" part="knob-track" d="${ARC}" pathLength="100"/>
              <path class="arc" part="knob-arc" d="${ARC}" pathLength="100" stroke-dasharray="0 100"/>
              <line class="indicator" part="knob-indicator" x1="50" y1="50" x2="50" y2="22"/>
            </svg>
          </div>
          <div class="label" part="knob-label"><slot></slot></div>
          <div class="value" part="knob-value" aria-hidden="true"></div>
        </div>`;
      const dial = this.dial;
      dial.addEventListener('pointerdown', (event) => this.pointerDown(event));
      dial.addEventListener('pointermove', (event) => this.pointerMove(event));
      for (const type of ['pointerup', 'pointercancel', 'lostpointercapture'] as const) dial.addEventListener(type, () => { this.drag = null; });
      dial.addEventListener('keydown', (event) => this.keyDown(event));
      dial.addEventListener('dblclick', () => this.reset());
    }
    this.link.connect();
  }
  disconnectedCallback() { this.link.disconnect(); }
  attributeChangedCallback(name: string) {
    if (name === 'instrument' && this.root) this.link.connect();
    else this.bind(this.link.flows);
  }

  private get dial() { return this.root!.querySelector('.dial') as HTMLElement; }
  /** The bound parameter (key, range, label), once the instrument loaded. */
  get param(): FlowsParameter | null { return this.parameter; }

  private bind(flows: Flows | null) {
    for (const off of this.offs) off();
    this.offs = [];
    this.parameter = null;
    if (flows) {
      const key = this.getAttribute('param') ?? '';
      this.parameter = findParameter(flows.params(), key) ?? null;
      if (!this.parameter && key) console.warn(`<flows-knob>: no parameter "${key}". Available: ${flows.params().map((p) => p.key).join(', ')}`);
      this.value = this.parameter ? flows.getParam(this.parameter.key) ?? this.parameter.initial : 0;
      this.offs.push(flows.on('param', ({ parameter, value }) => { if (parameter.index === this.parameter?.index) { this.value = value; this.update(); } }));
      this.offs.push(flows.on('load', () => this.bind(flows)));
    }
    this.update();
  }

  private update() {
    if (!this.root) return;
    const p = this.parameter;
    const dial = this.dial;
    const label = this.getAttribute('label') ?? p?.label ?? this.getAttribute('param') ?? '';
    const slot = this.root.querySelector('slot')!;
    slot.textContent = label; // fallback when the page gives no label content
    dial.setAttribute('aria-label', label);
    const usable = !!p && p.live;
    dial.setAttribute('aria-disabled', String(!usable));
    const position = p ? toPosition(p, this.value) : 0;
    (this.root.querySelector('.arc') as SVGPathElement).setAttribute('stroke-dasharray', `${(position * 100).toFixed(2)} 100`);
    (this.root.querySelector('.indicator') as SVGLineElement).setAttribute('transform', `rotate(${-135 + position * 270} 50 50)`);
    const text = p ? formatValue(p, this.value) : '';
    (this.root.querySelector('.value') as HTMLElement).innerHTML = escapeHtml(text);
    if (p) {
      dial.setAttribute('aria-valuemin', String(p.min));
      dial.setAttribute('aria-valuemax', String(p.max));
      dial.setAttribute('aria-valuenow', String(this.value));
      dial.setAttribute('aria-valuetext', text);
    } else for (const name of ['aria-valuemin', 'aria-valuemax', 'aria-valuenow', 'aria-valuetext']) dial.removeAttribute(name);
  }

  private set(value: number) {
    const flows = this.link.flows, p = this.parameter;
    if (!flows || !p || !p.live) return;
    const applied = flows.setParam(p.key, value);
    this.dispatchEvent(new CustomEvent('flows-input', { bubbles: true, composed: true, detail: { key: p.key, value: applied } }));
  }
  private nudge(positionDelta: number) {
    const p = this.parameter;
    if (!p) return;
    if (p.discrete) { this.set(this.value + Math.sign(positionDelta) * Math.max(1, Math.round(p.step))); return; }
    const next = fromPosition(p, toPosition(p, this.value) + positionDelta);
    // Always move by at least one step so arrows never get stuck on rounding.
    this.set(next === this.value ? this.value + Math.sign(positionDelta) * p.step : next);
  }
  reset() { if (this.parameter) this.set(this.parameter.initial); }

  private pointerDown(event: PointerEvent) {
    if (!this.parameter?.live || event.button > 0) return;
    event.preventDefault();
    this.dial.focus();
    this.dial.setPointerCapture?.(event.pointerId);
    this.drag = { id: event.pointerId, x: event.clientX, y: event.clientY, position: toPosition(this.parameter, this.value) };
  }
  private pointerMove(event: PointerEvent) {
    const drag = this.drag, p = this.parameter;
    if (!drag || !p || drag.id !== event.pointerId) return;
    const travel = event.shiftKey ? 1600 : 200;
    const position = Math.max(0, Math.min(1, drag.position + ((event.clientX - drag.x) - (event.clientY - drag.y)) / travel));
    drag.position = position;
    drag.x = event.clientX;
    drag.y = event.clientY;
    this.set(fromPosition(p, position));
  }
  private keyDown(event: KeyboardEvent) {
    const p = this.parameter;
    if (!p?.live) return;
    const fine = event.shiftKey ? 0.1 : 1;
    const actions: Record<string, () => void> = {
      ArrowUp: () => this.nudge(0.01 * fine), ArrowRight: () => this.nudge(0.01 * fine),
      ArrowDown: () => this.nudge(-0.01 * fine), ArrowLeft: () => this.nudge(-0.01 * fine),
      PageUp: () => this.nudge(0.1), PageDown: () => this.nudge(-0.1),
      Home: () => this.set(p.min), End: () => this.set(p.max), Delete: () => this.reset(), Backspace: () => this.reset(),
    };
    const action = actions[event.key];
    if (!action) return;
    event.preventDefault();
    action();
  }
}

// <flows-controls params="filtered.cutoff, amp.R">: a row of knobs.
// Without params it shows the most useful live parameters (max="6").
const CONTROLS_STYLE = String.raw`
:host { display: block; }
.controls { display: flex; flex-wrap: wrap; gap: var(--flows-gap, 14px); align-items: flex-start; }
`;
const KNOB_PARTS = 'knob, knob-dial, knob-track, knob-arc, knob-indicator, knob-label, knob-value';
export class FlowsControlsElement extends Base {
  static observedAttributes = ['params', 'max', 'instrument'];
  private link = new InstrumentLink(this, (flows) => this.render(flows));
  private root: ShadowRoot | null = null;
  private offLoad: (() => void) | null = null;
  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, CONTROLS_STYLE);
      this.root.innerHTML = '<div class="controls" part="controls" role="group" aria-label="Instrument controls"></div>';
    }
    this.link.connect();
  }
  disconnectedCallback() { this.link.disconnect(); }
  attributeChangedCallback(name: string) {
    if (name === 'instrument' && this.root) this.link.connect();
    else this.render(this.link.flows);
  }
  private render(flows: Flows | null) {
    if (!this.root) return;
    this.offLoad?.();
    this.offLoad = flows ? flows.on('load', () => this.render(flows)) : null;
    const container = this.root.querySelector('.controls')!;
    container.replaceChildren();
    if (!flows) return;
    const requested = (this.getAttribute('params') ?? '').split(',').map((s) => s.trim()).filter(Boolean);
    let keys: string[];
    if (requested.length === 1 && requested[0] === 'none') keys = [];
    else if (requested.length === 1 && requested[0] === 'all') keys = flows.params().filter((p) => p.live && !p.discrete && p.hasRange).map((p) => p.key);
    else if (requested.length) keys = requested;
    else {
      const max = Math.max(0, Number(this.getAttribute('max') ?? 6) || 6);
      keys = defaultKeys(flows, max);
    }
    this.hidden = keys.length === 0;
    for (const key of keys) {
      const knob = document.createElement('flows-knob');
      knob.setAttribute('param', key);
      knob.setAttribute('exportparts', KNOB_PARTS);
      container.appendChild(knob);
    }
  }
}
const defaultKeys = (flows: Flows, max: number) => defaultControls(flows.params(), max).map((p) => p.key);

export function defineKnob() {
  define('flows-knob', FlowsKnobElement);
  define('flows-controls', FlowsControlsElement);
}
