// Types for the Emscripten module built from src/web/WebHost.cpp.
export interface FlowsModule {
  FS: any;
  IDBFS: { queuePersist: (mount: unknown) => void; onAutoPersistStateChanged?: (active: boolean) => void; [key: string]: unknown };
  HEAPU8: Uint8Array;
  UTF8ToString(pointer: number, maxBytes?: number): string;
  stringToNewUTF8(text: string): number;
  emscriptenRegisterAudioObject(object: AudioContext): number;
  _malloc(size: number): number;
  _free(pointer: number): void;
  _flows_start(workspace: number, settings: number, capabilities: number): number;
  _flows_request(method: number, params: number): number;
  _flows_state(): number;
  _flows_telemetry(): number;
  _flows_agent_events(): number;
  _flows_free(pointer: number): void;
  _flows_set_capability(name: number, value: number): void;
  _flows_audio_attach(context: number): void;
  _flows_audio_running(running: number, sampleRate: number): void;
  _flows_midi(status: number, data1: number, data2: number, size: number): number;
  _flows_release_midi(): void;
  _flows_fetch_complete(call: number, status: number, body: number, length: number, error: number): void;
  onFlowsAudio?: (state: string, node: number) => void;
  flowsFetch?: (call: number, url: string, body: Uint8Array<ArrayBuffer>, headers: Record<string, string>, timeoutMs: number, limit: number) => void;
  flowsFetchAbort?: (call: number) => void;
}

/** Calls a C function with UTF-8 arguments; frees everything it allocates. */
export function withStrings<T>(module: FlowsModule, values: string[], call: (...pointers: number[]) => T): T {
  const pointers = values.map(value => module.stringToNewUTF8(value));
  try {
    if (pointers.includes(0)) throw new Error('Flows ran out of memory in this tab. Export your work and reload the page.');
    return call(...pointers);
  } finally { pointers.forEach(pointer => { if (pointer) module._free(pointer); }); }
}
/** Reads and frees a malloc'd UTF-8 result. Returns null for a null pointer. */
export function takeString(module: FlowsModule, pointer: number): string | null {
  if (!pointer) return null;
  try { return module.UTF8ToString(pointer); } finally { module._flows_free(pointer); }
}
