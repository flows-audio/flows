// Shared pieces of the Flows elements: an SSR-safe base class, the design
// tokens every element reads, the optional themes and instrument lookup.
import type { Flows } from '../core';

/** HTMLElement in browsers; a stub during server rendering so imports never throw. */
export const Base: typeof HTMLElement = typeof HTMLElement !== 'undefined' ? HTMLElement : (class {} as unknown as typeof HTMLElement);

// Every visual value is a --flows-* custom property. Elements never set
// these on :host without a theme, so values from the host page always win
// and inherit through nested elements. theme="none" (default) is transparent
// and takes font and colour from the page.
export const TOKENS = String.raw`
:host {
  box-sizing: border-box;
  font-family: var(--flows-font, inherit);
  color: var(--flows-text, inherit);
  -webkit-tap-highlight-color: transparent;
}
:host([hidden]) { display: none !important; }
*, *::before, *::after { box-sizing: inherit; }
[hidden] { display: none !important; }
:focus { outline: none; }
:focus-visible {
  outline: var(--flows-focus-width, 2px) solid var(--flows-focus, var(--flows-accent, currentColor));
  outline-offset: 2px;
}
.sr-only {
  position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden;
  clip: rect(0 0 0 0); white-space: nowrap; border: 0;
}

/* theme="glass": translucent, blurred and lit from above. */
:host([theme="glass"]) {
  --flows-border-width: 1px;
  --flows-surface: rgba(255, 255, 255, .16);
  --flows-surface-raised: rgba(255, 255, 255, .26);
  --flows-border-color: rgba(255, 255, 255, .5);
  --flows-blur: 22px;
  --flows-saturate: 170%;
  --flows-radius: 26px;
  --flows-padding: 18px;
  --flows-shadow: inset 0 1px 0 rgba(255, 255, 255, .65), inset 0 -1px 0 rgba(255, 255, 255, .12), 0 20px 44px -18px rgba(18, 26, 46, .38);
  --flows-accent: #3d7bff;
  --flows-glow: rgba(61, 123, 255, .45);
  --flows-key-white: linear-gradient(180deg, rgba(255, 255, 255, .78), rgba(255, 255, 255, .42));
  --flows-key-black: linear-gradient(180deg, rgb(52, 60, 84), rgba(30, 38, 60, .82));
  --flows-key-border: rgba(255, 255, 255, .7);
  --flows-key-active: linear-gradient(180deg, rgba(120, 165, 255, .95), rgba(61, 123, 255, .75));
  --flows-knob-face: radial-gradient(circle at 35% 30%, rgba(255, 255, 255, .85), rgba(255, 255, 255, .25) 70%);
  --flows-knob-track: rgba(255, 255, 255, .45);
}
@media (prefers-color-scheme: dark) {
  :host([theme="glass"]) {
    --flows-surface: rgba(255, 255, 255, .07);
    --flows-surface-raised: rgba(255, 255, 255, .12);
    --flows-border-color: rgba(255, 255, 255, .18);
    --flows-shadow: inset 0 1px 0 rgba(255, 255, 255, .2), 0 24px 48px -20px rgba(0, 0, 0, .7);
    --flows-accent: #8db4ff;
    --flows-glow: rgba(141, 180, 255, .5);
    --flows-key-white: linear-gradient(180deg, rgba(255, 255, 255, .2), rgba(255, 255, 255, .08));
    --flows-key-black: linear-gradient(180deg, rgb(12, 14, 20), rgba(0, 0, 0, .78));
    --flows-key-border: rgba(255, 255, 255, .22);
    --flows-key-active: linear-gradient(180deg, rgba(141, 180, 255, .8), rgba(90, 140, 255, .55));
    --flows-knob-face: radial-gradient(circle at 35% 30%, rgba(255, 255, 255, .3), rgba(255, 255, 255, .05) 70%);
    --flows-knob-track: rgba(255, 255, 255, .18);
  }
}

/* theme="studio": opaque hardware panel. */
:host([theme="studio"]) {
  --flows-border-width: 1px;
  --flows-font: ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  --flows-text: #1d1f22;
  --flows-muted: #6b6f76;
  --flows-surface: #f3f2ef;
  --flows-surface-raised: #ffffff;
  --flows-border-color: #d9d6cf;
  --flows-radius: 12px;
  --flows-padding: 16px;
  --flows-shadow: 0 1px 2px rgba(0, 0, 0, .08), 0 8px 24px -12px rgba(0, 0, 0, .25);
  --flows-accent: #e4572e;
  --flows-key-white: #fffefb;
  --flows-key-black: #24262a;
  --flows-key-border: #cfcbc2;
  --flows-knob-face: #ffffff;
}
@media (prefers-color-scheme: dark) {
  :host([theme="studio"]) {
    --flows-text: #ece9e3;
    --flows-muted: #9a9ca1;
    --flows-surface: #17191c;
    --flows-surface-raised: #222529;
    --flows-border-color: #2e3237;
    --flows-accent: #ff7a45;
    --flows-key-white: #e9e6df;
    --flows-key-black: #0d0e10;
    --flows-key-border: #3a3e44;
    --flows-knob-face: #2a2d32;
  }
}
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { transition-duration: 0s !important; animation: none !important; }
}
`;

const sheets = new Map<string, CSSStyleSheet | null>();
/** Adopts shared constructable stylesheets, or falls back to <style>. */
export function adoptStyles(root: ShadowRoot, ...texts: string[]) {
  const adopted: CSSStyleSheet[] = [];
  for (const text of texts) {
    let sheet = sheets.get(text);
    if (sheet === undefined) {
      try {
        sheet = new CSSStyleSheet();
        sheet.replaceSync(text);
      } catch {
        sheet = null;
      }
      sheets.set(text, sheet);
    }
    if (sheet && 'adoptedStyleSheets' in root) adopted.push(sheet);
    else {
      const style = document.createElement('style');
      style.textContent = text;
      root.appendChild(style);
    }
  }
  if (adopted.length) root.adoptedStyleSheets = [...root.adoptedStyleSheets, ...adopted];
}

/** What child elements need from an instrument element. */
export interface InstrumentHost extends HTMLElement {
  readonly flows: Flows | null;
  /** Calls back now and whenever the instrument (re)loads or goes away. */
  subscribe(callback: (flows: Flows | null) => void): () => void;
  start(): Promise<void>;
}
const isInstrument = (node: unknown): node is InstrumentHost =>
  !!node && typeof (node as InstrumentHost).subscribe === 'function' && (node as Element).localName === 'flows-instrument';

/** The instrument for a child element: instrument="id", else the nearest ancestor (across shadow roots). */
export function findInstrument(element: Element): InstrumentHost | null {
  const id = element.getAttribute('instrument');
  if (id) {
    const root = element.getRootNode() as Document | ShadowRoot;
    const found = (root.getElementById?.(id) ?? document.getElementById(id)) as Element | null;
    return isInstrument(found) ? found : null;
  }
  let node: Node | null = element;
  while (node) {
    node = node.parentNode ?? (node as ShadowRoot).host ?? null;
    if (isInstrument(node)) return node;
  }
  return null;
}

/**
 * Keeps a child element bound to its instrument. The instrument may upgrade
 * after the child, so lookup retries once the element is defined.
 */
export class InstrumentLink {
  private host: InstrumentHost | null = null;
  private unsubscribe: (() => void) | null = null;
  flows: Flows | null = null;
  constructor(private readonly element: Element, private readonly changed: (flows: Flows | null) => void) {}
  connect() {
    this.disconnect();
    const host = findInstrument(this.element);
    if (!host) {
      if (typeof customElements !== 'undefined' && !customElements.get('flows-instrument'))
        void customElements.whenDefined('flows-instrument').then(() => { if (this.element.isConnected && !this.host) this.connect(); });
      else if (this.element.getAttribute('instrument'))
        // The referenced instrument may appear later in the document.
        requestAnimationFrame(() => { if (this.element.isConnected && !this.host) this.connect(); });
      return;
    }
    this.host = host;
    this.unsubscribe = host.subscribe((flows) => { this.flows = flows; this.changed(flows); });
  }
  disconnect() {
    this.unsubscribe?.();
    this.unsubscribe = null;
    this.host = null;
    if (this.flows) { this.flows = null; this.changed(null); }
  }
  get instrument() { return this.host; }
  /** Starts sound from a gesture, then runs `then` (e.g. play a note). */
  start() { return this.host ? this.host.start() : this.flows ? this.flows.start() : Promise.reject(new Error('No instrument')); }
}

export function define(name: string, constructor: CustomElementConstructor) {
  if (typeof customElements !== 'undefined' && !customElements.get(name)) customElements.define(name, constructor);
}
export const html = (strings: TemplateStringsArray, ...values: unknown[]) => strings.reduce((out, part, i) => out + part + (i < values.length ? String(values[i]) : ''), '');
export const escapeHtml = (text: string) => text.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
