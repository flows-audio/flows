// <flows-keys>: a piano keyboard. Pointer (with glissando), keyboard focus
// (arrows + Space/Enter) and computer keys A W S E D F T G Y H U J K.
//
// The computer keys are armed by a click on a key (or focus on the keyboard) and
// stay armed while the player works the same instrument with the mouse: a knob,
// a number dragged in the code, the scope. So one hand plays while the other
// shapes the sound. Escape, a click outside the instrument, or the caret in a
// text field disarms them. Their letters show on the keys while armed.
import type { Flows } from '../core';
import { noteName, notePitch } from '../params';
import { Base, InstrumentLink, TOKENS, adoptStyles, define, findInstrument } from './shared';

const STYLE = String.raw`
:host { display: block; }
.keys {
  position: relative; display: flex; width: 100%;
  height: var(--flows-key-height, 112px);
  touch-action: none; user-select: none; -webkit-user-select: none;
  border-radius: var(--flows-key-radius, calc(var(--flows-radius, 10px) * .5));
}
.key {
  appearance: none; margin: 0; padding: 0 0 8px; font: inherit; cursor: pointer;
  display: flex; flex-direction: column; align-items: center; justify-content: flex-end; gap: 4px;
  transition: background var(--flows-transition, 90ms) ease, box-shadow var(--flows-transition, 90ms) ease, transform var(--flows-transition, 90ms) ease;
}
.white {
  flex: 1 1 0; min-width: 0; position: relative; z-index: 0;
  background: var(--flows-key-white, color-mix(in srgb, currentColor 4%, transparent));
  border: var(--flows-border-width, 1px) solid var(--flows-key-border, color-mix(in srgb, currentColor 22%, transparent));
  margin-left: calc(var(--flows-border-width, 1px) * -1);
  border-radius: 0 0 var(--flows-key-radius, calc(var(--flows-radius, 10px) * .5)) var(--flows-key-radius, calc(var(--flows-radius, 10px) * .5));
  color: var(--flows-muted, color-mix(in srgb, currentColor 55%, transparent));
}
.white:first-of-type { margin-left: 0; }
.black {
  position: absolute; top: 0; z-index: 1; height: 62%;
  /* Opaque by default: a translucent key would show the white keys' seam. */
  background: var(--flows-key-black, currentColor);
  border: var(--flows-border-width, 1px) solid var(--flows-key-border, color-mix(in srgb, currentColor 22%, transparent));
  border-top: 0;
  border-radius: 0 0 calc(var(--flows-key-radius, calc(var(--flows-radius, 10px) * .5)) * .6) calc(var(--flows-key-radius, calc(var(--flows-radius, 10px) * .5)) * .6);
}
.key.pressed {
  background: var(--flows-key-active, var(--flows-accent, color-mix(in srgb, currentColor 40%, transparent)));
  box-shadow: 0 0 18px var(--flows-glow, transparent);
}
.white.pressed { transform: translateY(1px); }
.label { font-size: var(--flows-key-label-size, 11px); line-height: 1; pointer-events: none; opacity: .85; }
/* The computer key for each note, shown while the keyboard has focus (the keys play). */
.hint {
  font-size: var(--flows-key-hint-size, var(--flows-key-label-size, 11px)); line-height: 1; pointer-events: none;
  text-transform: uppercase; opacity: 0; transition: opacity var(--flows-transition, 90ms) ease;
  color: var(--flows-key-hint, var(--flows-muted, color-mix(in srgb, currentColor 55%, transparent)));
}
.black .hint { color: var(--flows-key-hint-black, Canvas); }
.keys.armed .hint { opacity: 1; }
/* Only a sharp rises when focused: a raised natural key would cover its neighbouring sharps. */
.key:focus-visible { outline-offset: -4px; }
.black:focus-visible { z-index: 2; }
`;

const BLACK = new Set([1, 3, 6, 8, 10]);
const COMPUTER = 'awsedftgyhujkolp;';
/** Computer keys start at the first C at or above the lowest key. */
const computerBase = (first: number) => Math.ceil(first / 12) * 12;

// One keyboard on the page has the computer keys at a time.
let armed: FlowsKeysElement | null = null;
const editable = (node: EventTarget | undefined) => {
  const element = node as HTMLElement | undefined;
  return !!element?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"])') || !!element?.isContentEditable;
};
let listening = false;
function listen() {
  if (listening || typeof document === 'undefined') return;
  listening = true;
  // Capture: the keyboard sees a key before whatever has focus (a knob, the page).
  document.addEventListener('keydown', (event) => armed?.computerDown(event), true);
  document.addEventListener('keyup', (event) => armed?.computerUp(event), true);
  document.addEventListener('pointerdown', (event) => { if (armed && !armed.owns(event.composedPath())) armed.disarm(); }, true);
  document.addEventListener('focusin', (event) => {
    const path = event.composedPath();
    if (armed && (editable(path[0]) || !armed.owns(path))) armed.disarm();
  }, true);
  // Key releases are lost while the window is in the background.
  addEventListener('blur', () => armed?.releaseComputer());
}

export class FlowsKeysElement extends Base {
  static observedAttributes = ['start', 'octaves', 'labels', 'instrument', 'velocity', 'computer-keys'];
  private link = new InstrumentLink(this, (flows) => this.bind(flows));
  private root: ShadowRoot | null = null;
  private keys = new Map<number, HTMLButtonElement>();
  private pointers = new Map<number, number>();
  private keyboard = new Map<string, number>();
  private offNote: (() => void) | null = null;
  private focusPitch = -1;

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.render();
    }
    this.link.connect();
  }
  disconnectedCallback() {
    if (armed === this) this.disarm();
    this.releaseAll();
    this.link.disconnect();
  }
  attributeChangedCallback(name: string) {
    if (!this.root) return;
    if (name === 'instrument') this.link.connect();
    else if (name !== 'velocity') this.render();
  }

  /** Lowest pitch (default C3 = 48). */
  get start(): number {
    const value = this.getAttribute('start')?.trim();
    if (!value) return 48;
    try { return notePitch(/^\d+$/.test(value) ? Number(value) : value); } catch { return 48; }
  }
  get octaves(): number { return Math.max(1, Math.min(8, Math.round(Number(this.getAttribute('octaves') ?? 2)) || 2)); }
  private get velocity(): number { const v = Number(this.getAttribute('velocity') ?? 0.8); return Number.isFinite(v) ? Math.max(0, Math.min(1, v)) : 0.8; }

  private render() {
    const root = this.root!;
    this.releaseAll();
    root.querySelector('.keys')?.remove();
    const container = document.createElement('div');
    container.className = 'keys';
    container.setAttribute('part', 'keys');
    container.setAttribute('role', 'group');
    container.setAttribute('aria-label', this.getAttribute('aria-label') ?? 'Piano keyboard');
    const first = this.start, last = Math.min(127, first + this.octaves * 12);
    const whites = [...Array(last - first + 1).keys()].filter((i) => !BLACK.has((first + i) % 12)).length;
    const labels = this.getAttribute('labels') ?? 'c';
    const computer = this.getAttribute('computer-keys') !== 'off';
    const base = computerBase(first);
    let whiteIndex = 0;
    this.keys.clear();
    for (let pitch = first; pitch <= last; pitch++) {
      const black = BLACK.has(pitch % 12);
      const key = document.createElement('button');
      key.type = 'button';
      key.className = `key ${black ? 'black' : 'white'}`;
      key.dataset.pitch = String(pitch);
      key.tabIndex = -1;
      key.setAttribute('aria-label', noteName(pitch));
      key.setAttribute('aria-pressed', 'false');
      key.setAttribute('part', `key ${black ? 'key-black' : 'key-white'}`);
      if (black) {
        const width = 62 / whites;
        key.style.left = `calc(${(whiteIndex / whites) * 100}% - ${width / 2}%)`;
        key.style.width = `${width}%`;
      } else {
        whiteIndex++;
        if (labels === 'all' || (labels === 'c' && pitch % 12 === 0)) {
          const label = document.createElement('span');
          label.className = 'label';
          label.setAttribute('part', 'key-label');
          label.setAttribute('aria-hidden', 'true');
          label.textContent = noteName(pitch);
          key.appendChild(label);
        }
      }
      const letter = computer ? COMPUTER[pitch - base] : undefined;
      if (letter) {
        const hint = document.createElement('span');
        hint.className = 'hint';
        hint.setAttribute('part', 'key-hint');
        hint.setAttribute('aria-hidden', 'true');
        hint.textContent = letter;
        key.prepend(hint);
        key.setAttribute('aria-keyshortcuts', letter.toUpperCase());
      }
      this.keys.set(pitch, key);
      container.appendChild(key);
    }
    // Roving focus: one tab stop, on middle C when shown.
    this.focusPitch = this.keys.has(60) ? 60 : first;
    this.keys.get(this.focusPitch)!.tabIndex = 0;
    container.addEventListener('pointerdown', (event) => this.pointerDown(event));
    container.addEventListener('pointermove', (event) => this.pointerMove(event));
    for (const type of ['pointerup', 'pointercancel', 'lostpointercapture'] as const)
      container.addEventListener(type, (event) => this.pointerUp(event as PointerEvent));
    container.addEventListener('keydown', (event) => this.keyDown(event));
    container.addEventListener('keyup', (event) => this.keyUp(event));
    container.addEventListener('focusin', () => this.arm());
    container.classList.toggle('armed', armed === this);
    container.addEventListener('contextmenu', (event) => event.preventDefault());
    root.appendChild(container);
    this.syncPressed();
  }

  private bind(flows: Flows | null) {
    this.offNote?.();
    this.offNote = null;
    if (flows) this.offNote = flows.on('note', ({ pitch, on }) => this.show(pitch, on));
    this.syncPressed();
  }
  private syncPressed() {
    const held = new Set(this.link.flows?.heldNotes() ?? []);
    for (const [pitch] of this.keys) this.show(pitch, held.has(pitch));
  }
  private show(pitch: number, on: boolean) {
    const key = this.keys.get(pitch);
    if (!key) return;
    key.classList.toggle('pressed', on);
    key.setAttribute('aria-pressed', String(on));
    key.setAttribute('part', `key ${key.classList.contains('black') ? 'key-black' : 'key-white'}${on ? ' key-pressed' : ''}`);
  }

  private play(pitch: number) {
    const flows = this.link.flows;
    if (!flows) return;
    // The first touch both unlocks audio and plays; the note waits for the worklet.
    if (flows.state !== 'running') void this.link.start().catch(() => {});
    flows.noteOn(pitch, this.velocity);
  }
  private stop(pitch: number) { this.link.flows?.noteOff(pitch); }
  private releaseAll() {
    for (const pitch of this.pointers.values()) this.stop(pitch);
    for (const pitch of this.keyboard.values()) this.stop(pitch);
    this.pointers.clear();
    this.keyboard.clear();
  }

  private pitchAt(x: number, y: number): number | undefined {
    const target = this.root!.elementFromPoint?.(x, y) as HTMLElement | null;
    const key = target?.closest?.('.key') as HTMLElement | null;
    return key?.dataset.pitch ? Number(key.dataset.pitch) : undefined;
  }
  private pointerDown(event: PointerEvent) {
    if (event.button > 0) return;
    const pitch = Number((event.target as HTMLElement).closest<HTMLElement>('.key')?.dataset.pitch);
    if (!Number.isFinite(pitch)) return;
    event.preventDefault();
    // preventDefault keeps the page from selecting text, and also from focusing the key: focus it here.
    this.moveFocus(pitch, false);
    this.arm();
    (event.currentTarget as HTMLElement).setPointerCapture?.(event.pointerId);
    this.pointers.set(event.pointerId, pitch);
    this.play(pitch);
  }
  private pointerMove(event: PointerEvent) {
    const current = this.pointers.get(event.pointerId);
    if (current === undefined) return;
    const pitch = this.pitchAt(event.clientX, event.clientY);
    if (pitch === undefined || pitch === current) return;
    this.stop(current);
    this.pointers.set(event.pointerId, pitch);
    this.play(pitch);
  }
  private pointerUp(event: PointerEvent) {
    const pitch = this.pointers.get(event.pointerId);
    if (pitch === undefined) return;
    this.pointers.delete(event.pointerId);
    if (![...this.pointers.values(), ...this.keyboard.values()].includes(pitch)) this.stop(pitch);
  }

  private moveFocus(pitch: number, scroll = true) {
    const key = this.keys.get(pitch);
    if (!key) return;
    this.keys.get(this.focusPitch)!.tabIndex = -1;
    key.tabIndex = 0;
    key.focus({ preventScroll: !scroll });
    this.focusPitch = pitch;
  }
  private keyDown(event: KeyboardEvent) {
    if (event.metaKey || event.ctrlKey || event.altKey) return;
    // A key handled here is the keyboard's alone: the page's own shortcuts must not also act on it.
    const handled = () => { event.preventDefault(); event.stopPropagation(); };
    const first = this.start, last = first + this.octaves * 12;
    switch (event.key) {
      case 'ArrowRight': case 'ArrowUp': handled(); this.moveFocus(Math.min(last, this.focusPitch + 1)); return;
      case 'ArrowLeft': case 'ArrowDown': handled(); this.moveFocus(Math.max(first, this.focusPitch - 1)); return;
      case 'Home': handled(); this.moveFocus(first); return;
      case 'End': handled(); this.moveFocus(last); return;
    }
    if (event.key !== ' ' && event.key !== 'Enter') return; // computer keys: computerDown
    handled();
    this.press(event, this.focusPitch);
  }
  private keyUp(event: KeyboardEvent) {
    if (this.keyboard.has(event.code)) { event.stopPropagation(); this.release(event.code); }
  }
  private press(event: KeyboardEvent, pitch: number) {
    if (event.repeat || this.keyboard.has(event.code) || pitch > 127) return;
    this.keyboard.set(event.code, pitch);
    this.play(pitch);
  }
  private release(code: string) {
    const pitch = this.keyboard.get(code);
    if (pitch === undefined) return;
    this.keyboard.delete(code);
    if (![...this.pointers.values(), ...this.keyboard.values()].includes(pitch)) this.stop(pitch);
  }

  /** Whether an event path is on this keyboard's instrument: the keys, the instrument, or an element bound to it. */
  owns(path: EventTarget[]) {
    const host = this.link.instrument as Element | null;
    return path.some((node) => node === this || (host && (node === host
      || ((node as Element).localName?.startsWith('flows-') && findInstrument(node as Element) === host))));
  }
  arm() {
    if (armed === this) return;
    armed?.disarm();
    armed = this;
    listen();
    this.toggleAttribute('armed', true);
    this.root?.querySelector('.keys')?.classList.add('armed');
  }
  disarm() {
    if (armed === this) armed = null;
    this.releaseComputer();
    this.toggleAttribute('armed', false);
    this.root?.querySelector('.keys')?.classList.remove('armed');
  }
  releaseComputer() { for (const code of [...this.keyboard.keys()]) this.release(code); }
  /** A key anywhere on the page while armed (not while typing in a text field). */
  computerDown(event: KeyboardEvent) {
    if (event.metaKey || event.ctrlKey || event.altKey || editable(event.composedPath()[0])) return;
    if (event.key === 'Escape') { event.stopPropagation(); this.disarm(); return; }
    if (this.getAttribute('computer-keys') === 'off') return;
    const index = COMPUTER.indexOf(event.key.toLowerCase());
    if (index < 0 || event.key.length !== 1) return;
    // The note is the keyboard's alone: neither the focused control nor the page's shortcuts see it.
    event.preventDefault();
    event.stopPropagation();
    this.press(event, computerBase(this.start) + index);
  }
  computerUp(event: KeyboardEvent) {
    if (!this.keyboard.has(event.code)) return;
    event.stopPropagation();
    this.release(event.code);
  }
}

export function defineKeys() { define('flows-keys', FlowsKeysElement); }
