// <flows-play-gate>: the "tap to play" button. Browsers only start audio
// after a user gesture; this button is that gesture. It hides itself while
// the instrument is running and comes back if the browser suspends audio.
// Put your own content inside it: <flows-play-gate>Play the demo</flows-play-gate>
import type { Flows, FlowsState } from '../core';
import { Base, InstrumentLink, TOKENS, adoptStyles, define } from './shared';

const STYLE = String.raw`
:host { display: inline-block; }
button {
  appearance: none; font: inherit; color: inherit; cursor: pointer;
  display: inline-flex; align-items: center; gap: .5em;
  padding: var(--flows-gate-padding, .45em .9em);
  border-radius: var(--flows-gate-radius, 999px);
  border: var(--flows-border-width, 1px) solid var(--flows-border-color, color-mix(in srgb, currentColor 25%, transparent));
  background: var(--flows-gate-background, var(--flows-surface-raised, transparent));
  box-shadow: 0 0 0 0 var(--flows-glow, transparent);
  transition: box-shadow var(--flows-transition, 160ms) ease, background var(--flows-transition, 160ms) ease;
}
button:hover:not(:disabled) { box-shadow: 0 0 22px 0 var(--flows-glow, transparent); }
button:disabled { cursor: progress; opacity: .7; }
.icon { width: .8em; height: .8em; fill: var(--flows-accent, currentColor); flex: none; }
`;

export class FlowsPlayGateElement extends Base {
  static observedAttributes = ['instrument'];
  private link = new InstrumentLink(this, (flows) => this.bind(flows));
  private root: ShadowRoot | null = null;
  private offState: (() => void) | null = null;

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.root.innerHTML = `<button type="button" part="gate gate-button">
        <svg class="icon" part="gate-icon" viewBox="0 0 10 10" aria-hidden="true"><path d="M2 1.2v7.6L8.6 5z"/></svg>
        <span part="gate-label"><slot>Tap to play</slot></span></button>`;
      this.root.querySelector('button')!.addEventListener('click', () => {
        void this.link.start().catch((error) => console.warn('Flows could not start audio:', error));
      });
    }
    this.hidden = true;
    this.link.connect();
  }
  disconnectedCallback() { this.link.disconnect(); }
  attributeChangedCallback() { if (this.root) this.link.connect(); }

  private bind(flows: Flows | null) {
    this.offState?.();
    this.offState = flows ? flows.on('state', ({ state }) => this.show(state)) : null;
    this.show(flows?.state ?? 'loading');
  }
  private show(state: FlowsState) {
    const button = this.root?.querySelector('button');
    if (!button) return;
    this.hidden = !(state === 'ready' || state === 'starting' || state === 'suspended');
    button.disabled = state === 'starting';
    button.setAttribute('aria-busy', String(state === 'starting'));
  }
}

export function defineGate() { define('flows-play-gate', FlowsPlayGateElement); }
