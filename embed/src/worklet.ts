// The AudioWorkletProcessor. Each <flows-instrument> gets its own node and
// its own instance of the WebAssembly module (own memory), so instruments on
// one page never share DSP state. The page posts the compiled
// WebAssembly.Module (or its bytes), the source and any sample files; the
// processor compiles the source with the same compiler and renders with the
// same engine as the Flows app. No SharedArrayBuffer, no threads.
import { FlowsWasm } from './wasm';

declare const sampleRate: number;
declare const currentTime: number;
declare function registerProcessor(name: string, processor: unknown): void;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: unknown);
}

export interface ProcessorOptions {
  module?: WebAssembly.Module;
  bytes?: ArrayBuffer;
  source: string;
  samples: (ArrayBuffer | null)[];
  boundarySeconds: number;
  bpm: number;
}
export type ToProcessor =
  | { type: 'on'; id: number; pitch: number; velocity: number; time?: number }
  | { type: 'off'; id: number; time?: number }
  | { type: 'param'; index: number; value: number }
  | { type: 'bpm'; bpm: number }
  | { type: 'panic' }
  | { type: 'load'; source: string; samples: (ArrayBuffer | null)[]; request: number }
  | { type: 'dispose' };
export type FromProcessor =
  | { type: 'ready' }
  | { type: 'loaded'; request: number }
  | { type: 'error'; message: string; request?: number }
  | { type: 'meter'; peak: number; voices: number };

const METER_BLOCKS = 8;

class FlowsProcessor extends AudioWorkletProcessor {
  private wasm: FlowsWasm | null = null;
  private handle = 0;
  private boundary: number;
  private bpm: number;
  private disposed = false;
  private peak = 0;
  private reported = 0;
  private blocks = 0;
  private resetReported = false;

  constructor(options: { processorOptions: ProcessorOptions }) {
    super();
    const settings = options.processorOptions;
    this.boundary = settings.boundarySeconds;
    this.bpm = settings.bpm;
    this.port.onmessage = (event: MessageEvent<ToProcessor>) => this.receive(event.data);
    try {
      const module = settings.module ?? new WebAssembly.Module(settings.bytes!);
      this.wasm = new FlowsWasm(module);
      this.handle = this.load(settings.source, settings.samples);
      this.post({ type: 'ready' });
    } catch (error) {
      this.post({ type: 'error', message: String((error as Error)?.message ?? error) });
    }
  }

  private post(message: FromProcessor) { this.port.postMessage(message); }

  private load(source: string, samples: (ArrayBuffer | null)[]): number {
    const wasm = this.wasm!;
    const { handle, info } = wasm.compile(source);
    try {
      if (!info.ok) throw new Error(info.diagnostics.map((d) => `${d.line}:${d.column} ${d.message}`).join('\n') || 'The instrument did not compile');
      (info.samples ?? []).forEach((path, index) => {
        const bytes = samples[index];
        if (!bytes) throw new Error(`Sample not loaded: ${path}`);
        wasm.sample(handle, index, bytes);
      });
      wasm.prepare(handle, sampleRate, this.boundary, this.bpm);
      return handle;
    } catch (error) {
      wasm.free(handle);
      throw error;
    }
  }

  // Context seconds to engine samples; past or missing times play now.
  private at(time: number | undefined): number {
    if (time === undefined || !(time > currentTime)) return 0;
    return this.wasm!.clock(this.handle) + Math.round((time - currentTime) * sampleRate);
  }

  private receive(message: ToProcessor) {
    const wasm = this.wasm;
    if (message.type === 'dispose') {
      this.disposed = true;
      if (wasm && this.handle) wasm.free(this.handle);
      this.handle = 0;
      return;
    }
    if (!wasm) return;
    if (message.type === 'load') {
      try {
        const next = this.load(message.source, message.samples);
        if (this.handle) wasm.free(this.handle);
        this.handle = next;
        this.post({ type: 'loaded', request: message.request });
      } catch (error) {
        this.post({ type: 'error', message: String((error as Error)?.message ?? error), request: message.request });
      }
      return;
    }
    if (!this.handle) return;
    switch (message.type) {
      case 'on': wasm.noteOn(this.handle, message.id, message.pitch, message.velocity, this.at(message.time)); break;
      case 'off': wasm.noteOff(this.handle, message.id, this.at(message.time)); break;
      case 'param': wasm.param(this.handle, message.index, message.value); break;
      case 'bpm': this.bpm = message.bpm; wasm.bpm(this.handle, message.bpm); break;
      case 'panic': wasm.panic(this.handle); break;
    }
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    if (this.disposed) return false;
    const output = outputs[0];
    const wasm = this.wasm;
    if (!wasm || !this.handle || !output || !output.length) return true;
    let voices = 0;
    // Render quanta larger than the engine block are split.
    for (let offset = 0; offset < output[0].length;) {
      const frames = Math.min(1024, output[0].length - offset);
      voices = wasm.render(this.handle, frames);
      if (voices < 0 && !this.resetReported) {
        this.resetReported = true;
        this.post({ type: 'error', message: 'The instrument produced invalid audio and was reset' });
      }
      const [left, right] = wasm.output(this.handle, frames);
      output[0].set(left, offset);
      if (output.length > 1) output[1].set(right, offset);
      for (let i = 0; i < frames; i++) {
        const value = Math.abs(left[i]) > Math.abs(right[i]) ? Math.abs(left[i]) : Math.abs(right[i]);
        if (value > this.peak) this.peak = value;
      }
      offset += frames;
    }
    if (++this.blocks >= METER_BLOCKS) {
      if (this.peak > 0 || this.reported > 0) this.post({ type: 'meter', peak: this.peak, voices: Math.max(0, voices) });
      this.reported = this.peak;
      this.peak = 0;
      this.blocks = 0;
    }
    return true;
  }
}

registerProcessor('flows-embed', FlowsProcessor);
