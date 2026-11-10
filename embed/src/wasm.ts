// The Flows compiler and DSP engine (embed/native/EmbedHost.cpp) as a
// runtime-free WebAssembly module. This wrapper runs unchanged on the page,
// inside the AudioWorkletProcessor and in Node, so it uses no DOM APIs and no
// TextEncoder/TextDecoder (several AudioWorklet scopes lack them).

export interface ParameterInfo {
  index: number;
  id: string;
  name: string;
  unit: 'scalar' | 'pitch' | 'hertz' | 'decibels' | 'seconds' | 'boolean';
  value: number;
  min: number | null;
  max: number | null;
  step: number;
  default: number;
  hasDefault: boolean;
  hasRange: boolean;
  live: boolean;
  discrete: boolean;
  logarithmic: boolean;
  scale: string;
  scaleOffset: number;
  /** The literal that sets this value: [from, to) as string indices into the source; -1 when it has none. */
  from: number;
  to: number;
}
/** A compile problem. `from`/`to` index the source string (to === from when the span is empty). */
export interface Diagnostic { line: number; column: number; message: string; from: number; to: number }
export interface ProgramInfo {
  ok: boolean;
  diagnostics: Diagnostic[];
  channels?: 1 | 2;
  /** Equal for two sources that differ only in live parameter values. */
  structure?: string;
  voice?: 'poly' | 'mono';
  parameters?: ParameterInfo[];
  samples?: string[];
}

interface Exports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  flows_embed_alloc(size: number): number;
  flows_embed_release(pointer: number): void;
  flows_embed_compile(source: number, length: number): number;
  flows_embed_info(instrument: number): number;
  flows_embed_error(instrument: number): number;
  flows_embed_sample(instrument: number, index: number, bytes: number, length: number): number;
  flows_embed_prepare(instrument: number, sampleRate: number, boundarySeconds: number, bpm: number): number;
  flows_embed_clock(instrument: number): number;
  flows_embed_note_on(instrument: number, id: number, pitch: number, velocity: number, at: number): number;
  flows_embed_note_off(instrument: number, id: number, at: number): number;
  flows_embed_param(instrument: number, index: number, value: number): number;
  flows_embed_panic(instrument: number): void;
  flows_embed_set_bpm(instrument: number, bpm: number): void;
  flows_embed_render(instrument: number, frames: number): number;
  flows_embed_left(instrument: number): number;
  flows_embed_right(instrument: number): number;
  flows_embed_free(instrument: number): void;
}

interface Spanned { offset: number; length: number; from: number; to: number }

/** Maps UTF-8 byte offsets (as the compiler reports them) to indices into `source`. */
export function sourceIndex(source: string): (byte: number) => number {
  if (!/[^\x00-\x7f]/.test(source)) return (byte) => Math.min(byte, source.length);
  const starts: number[] = [];
  for (let i = 0; i < source.length; i++) {
    const code = source.codePointAt(i)!;
    const width = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    for (let b = 0; b < width; b++) starts.push(i);
    if (code >= 0x10000) i++;
  }
  return (byte) => (byte >= starts.length ? source.length : starts[byte]);
}

export function encodeUtf8(text: string): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < text.length; i++) {
    let code = text.charCodeAt(i);
    if (code >= 0xd800 && code < 0xdc00 && i + 1 < text.length) {
      const low = text.charCodeAt(i + 1);
      if (low >= 0xdc00 && low < 0xe000) { code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00); i++; }
    }
    if (code < 0x80) out.push(code);
    else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 63));
    else if (code < 0x10000) out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
    else out.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 63), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
  }
  return Uint8Array.from(out);
}
export function decodeUtf8(bytes: Uint8Array): string {
  let text = '';
  for (let i = 0; i < bytes.length;) {
    const a = bytes[i++];
    let code = a;
    if (a >= 0xf0) code = ((a & 7) << 18) | ((bytes[i++] & 63) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
    else if (a >= 0xe0) code = ((a & 15) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
    else if (a >= 0xc0) code = ((a & 31) << 6) | (bytes[i++] & 63);
    text += String.fromCodePoint(code);
  }
  return text;
}

/** One instance of the module: its own memory, any number of instruments. */
export class FlowsWasm {
  private readonly x: Exports;
  constructor(module: WebAssembly.Module) {
    let memory: WebAssembly.Memory | undefined;
    const zero = (...pointers: number[]) => {
      const view = new DataView(memory!.buffer);
      for (const pointer of pointers) view.setUint32(pointer, 0, true);
      return 0;
    };
    const instance = new WebAssembly.Instance(module, {
      env: { emscripten_notify_memory_growth: () => {} },
      wasi_snapshot_preview1: {
        environ_sizes_get: (count: number, size: number) => zero(count, size),
        environ_get: () => 0,
        fd_close: () => 0,
      },
    });
    this.x = instance.exports as unknown as Exports;
    memory = this.x.memory;
    this.x._initialize();
  }
  private text(pointer: number): string {
    const heap = new Uint8Array(this.x.memory.buffer);
    let end = pointer;
    while (heap[end]) end++;
    return decodeUtf8(heap.subarray(pointer, end));
  }
  private withBytes<T>(bytes: Uint8Array, use: (pointer: number) => T): T {
    const pointer = this.x.flows_embed_alloc(bytes.length);
    if (!pointer) throw new Error('Flows ran out of memory');
    try {
      new Uint8Array(this.x.memory.buffer, pointer, bytes.length).set(bytes);
      return use(pointer);
    } finally {
      this.x.flows_embed_release(pointer);
    }
  }
  /** Compiles source; returns the instrument handle and its description. Free the handle. */
  compile(source: string): { handle: number; info: ProgramInfo } {
    const bytes = encodeUtf8(source);
    const handle = this.withBytes(bytes, (pointer) => this.x.flows_embed_compile(pointer, bytes.length));
    if (!handle) throw new Error('Flows ran out of memory while compiling');
    const info = JSON.parse(this.text(this.x.flows_embed_info(handle))) as ProgramInfo;
    const index = sourceIndex(source);
    const place = (item: Spanned, always = false) => {
      const spanned = always || item.length > 0;
      item.from = spanned ? index(item.offset) : -1;
      item.to = spanned ? index(item.offset + item.length) : -1;
      delete (item as Partial<Spanned>).offset;
      delete (item as Partial<Spanned>).length;
    };
    info.diagnostics.forEach((d) => place(d as unknown as Spanned, true));
    info.parameters?.forEach((p) => place(p as unknown as Spanned));
    return { handle, info };
  }
  sample(handle: number, index: number, bytes: ArrayBuffer | Uint8Array): void {
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (!this.withBytes(data, (pointer) => this.x.flows_embed_sample(handle, index, pointer, data.length)))
      throw new Error(this.text(this.x.flows_embed_error(handle)) || `Sample ${index} could not be loaded`);
  }
  prepare(handle: number, sampleRate: number, boundarySeconds: number, bpm: number): void {
    if (!this.x.flows_embed_prepare(handle, sampleRate, boundarySeconds, bpm))
      throw new Error(this.text(this.x.flows_embed_error(handle)) || 'The instrument could not be prepared');
  }
  clock(handle: number): number { return this.x.flows_embed_clock(handle); }
  noteOn(handle: number, id: number, pitch: number, velocity: number, at = 0): boolean { return !!this.x.flows_embed_note_on(handle, id, pitch, velocity, at); }
  noteOff(handle: number, id: number, at = 0): boolean { return !!this.x.flows_embed_note_off(handle, id, at); }
  param(handle: number, index: number, value: number): boolean { return !!this.x.flows_embed_param(handle, index, value); }
  panic(handle: number): void { this.x.flows_embed_panic(handle); }
  bpm(handle: number, bpm: number): void { this.x.flows_embed_set_bpm(handle, bpm); }
  /** Renders up to 1024 frames. Returns active voices, or -1 when the engine reset. */
  render(handle: number, frames: number): number { return this.x.flows_embed_render(handle, frames); }
  /** Views of the last render (valid until the next call that may grow memory). */
  output(handle: number, frames: number): [Float32Array, Float32Array] {
    const buffer = this.x.memory.buffer;
    return [new Float32Array(buffer, this.x.flows_embed_left(handle), frames), new Float32Array(buffer, this.x.flows_embed_right(handle), frames)];
  }
  free(handle: number): void { this.x.flows_embed_free(handle); }
}
