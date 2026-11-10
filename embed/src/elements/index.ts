// @flows-audio/embed/elements: the custom elements. Importing this module
// defines them (when the page has customElements; server rendering is a no-op).
import { defineCode, FlowsCodeElement } from './code';
import { defineGate, FlowsPlayGateElement } from './gate';
import { defineInstrument, FlowsInstrumentElement } from './instrument';
import { defineKeys, FlowsKeysElement } from './keys';
import { defineKnob, FlowsControlsElement, FlowsKnobElement } from './knob';
import { defineScope, FlowsScopeElement } from './scope';

export { FlowsCodeElement, FlowsControlsElement, FlowsInstrumentElement, FlowsKeysElement, FlowsKnobElement, FlowsPlayGateElement, FlowsScopeElement };

/** Defines <flows-instrument>, <flows-code>, <flows-keys>, <flows-knob>, <flows-controls>, <flows-scope> and <flows-play-gate>. */
export function defineFlowsElements() {
  // The instrument first, so children can find it as they upgrade.
  defineInstrument();
  defineGate();
  defineScope();
  defineKnob();
  defineKeys();
  defineCode();
}

defineFlowsElements();

declare global {
  interface HTMLElementTagNameMap {
    'flows-instrument': FlowsInstrumentElement;
    'flows-code': FlowsCodeElement;
    'flows-keys': FlowsKeysElement;
    'flows-knob': FlowsKnobElement;
    'flows-controls': FlowsControlsElement;
    'flows-scope': FlowsScopeElement;
    'flows-play-gate': FlowsPlayGateElement;
  }
}
