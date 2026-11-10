// The classic-script build: <script src=".../flows-embed.iife.js"></script>
// defines the elements and exposes window.Flows.
import { Flows, FlowsCompileError, PRESETS, configure, formatValue, noteName, notePitch, sharedContext } from './core';
import { defineFlowsElements } from './elements/index';

const api = Object.assign(Flows, { Flows, FlowsCompileError, PRESETS, configure, formatValue, noteName, notePitch, sharedContext, defineFlowsElements });
if (typeof window !== 'undefined') (window as unknown as { Flows: typeof api }).Flows = api;
