// The UI's single connection to a Flows host. Components call request() and
// subscribe(); a transport carries the messages (the JUCE WebView bridge on
// macOS, the WebAssembly core in the browser). Features branch on the host's
// capabilities, never on which transport is active.
import { METHOD_TIMEOUTS, PROTOCOL_VERSION, type Capabilities, type EventName, type Events, type MethodName, type Methods, type Snapshot } from './protocol.gen';
import { juceTransport } from './transports/juce';

export interface Transport {
  readonly kind: 'native' | 'web' | 'none';
  call(method: string, params: Record<string, unknown>, timeoutMs: number): Promise<unknown>;
  listen(event: string, callback: (payload: any) => void): () => void;
}

const noTransport: Transport = {
  kind: 'none',
  call: () => Promise.reject(new Error('Open Flows in its native app or the web app to use audio, files and AI.')),
  listen: () => () => {},
};

let transport: Transport = juceTransport() ?? noTransport;
let capabilities: Capabilities | undefined;
let base: Record<string, unknown> | null = null;
const capabilityListeners = new Set<(value: Capabilities | undefined) => void>();
const stateListeners = new Set<(value: Snapshot) => void>();
let stopState: (() => void) | undefined;

/** Installs the browser host before the UI renders. */
export function setTransport(next: Transport) {
  stopState?.(); stopState = undefined;
  transport = next; base = null; capabilities = undefined;
  if (stateListeners.size) listenForState();
}
export function hostAvailable() { return transport.kind !== 'none'; }
export function transportKind() { return transport.kind; }
export function hostCapabilities() { return capabilities; }
export function onCapabilities(listener: (value: Capabilities | undefined) => void) {
  capabilityListeners.add(listener); return () => { capabilityListeners.delete(listener); };
}

function isSnapshot(value: unknown): value is Snapshot & Record<string, unknown> {
  return !!value && typeof value === 'object' && 'sessionId' in value && 'document' in value;
}
function observe(snapshot: Snapshot & Record<string, unknown>) {
  base = { ...snapshot };
  const next = snapshot.capabilities;
  if (next && JSON.stringify(next) !== JSON.stringify(capabilities)) {
    capabilities = next;
    capabilityListeners.forEach(listener => listener(capabilities));
  }
}

/** Merges a flowsState payload into the latest full snapshot. A slice event
 * that arrives before any full snapshot is dropped; the UI asks for one. */
export function assembleState(payload: unknown): Snapshot | null {
  if (payload && typeof payload === 'object' && 'slices' in payload) {
    const event = payload as { protocol?: number; full?: boolean; slices?: Record<string, unknown> };
    if (event.protocol !== undefined && event.protocol > PROTOCOL_VERSION) return null;
    if (!event.full && !base) return null;
    const next: Record<string, unknown> = event.full ? {} : { ...base };
    for (const [key, value] of Object.entries(event.slices ?? {})) {
      if (value === null) delete next[key]; else next[key] = value;
    }
    if (!isSnapshot(next)) return null;
    observe(next);
    return next;
  }
  if (!isSnapshot(payload)) return null;
  observe(payload);
  return payload;
}

export function request<M extends MethodName>(method: M, params?: Methods[M]['params']): Promise<Methods[M]['result']>;
export function request<T = any>(method: string, params?: Record<string, unknown>): Promise<T>;
export async function request(method: string, params: Record<string, unknown> = {}) {
  const result = await transport.call(method, params, METHOD_TIMEOUTS[method as MethodName] ?? 30000);
  if (isSnapshot(result)) observe(result);
  return result;
}

function listenForState() {
  stopState = transport.listen('flowsState', payload => {
    const snapshot = assembleState(payload);
    if (snapshot) stateListeners.forEach(listener => listener(snapshot));
  });
}
export function subscribe(event: 'flowsState', callback: (value: Snapshot) => void): () => void;
export function subscribe<E extends Exclude<EventName, 'flowsState'>>(event: E, callback: (value: Events[E]) => void): () => void;
export function subscribe<T>(event: string, callback: (value: T) => void): () => void;
export function subscribe(event: string, callback: (value: any) => void) {
  if (event !== 'flowsState') return transport.listen(event, callback);
  stateListeners.add(callback);
  if (!stopState) listenForState();
  return () => {
    stateListeners.delete(callback);
    if (!stateListeners.size) { stopState?.(); stopState = undefined; }
  };
}
