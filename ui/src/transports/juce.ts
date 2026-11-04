// The JUCE WebView bridge used by the macOS app and plugins.
import type { Transport } from '../client';

type ListenerToken = [string, number];
interface Backend { emitEvent(name: string, payload: unknown): void; addEventListener(name: string, listener: (payload: any) => void): ListenerToken; removeEventListener(token: ListenerToken): void }
declare global { interface Window { __JUCE__?: { backend?: Backend; initialisationData?: { __juce__functions?: string[] } } } }

/** The bridge, or undefined outside the native WebView. */
export function juceTransport(): Transport | undefined {
  const juce = typeof window === 'undefined' ? undefined : window.__JUCE__;
  const backend = juce?.backend;
  if (!backend || !juce?.initialisationData?.__juce__functions?.includes('flowsRequest')) return undefined;
  let sequence = 0;
  const waiting = new Map<number, { resolve: (value: unknown) => void; reject: (reason: Error) => void; timer: ReturnType<typeof setTimeout> }>();
  backend.addEventListener('__juce__complete', ({ promiseId, result }) => {
    const pending = waiting.get(promiseId);
    if (!pending) return;
    clearTimeout(pending.timer); waiting.delete(promiseId);
    if (result?.ok === false) pending.reject(new Error(result.error?.message || result.error || 'Native request failed'));
    else pending.resolve(result?.result ?? result);
  });
  return {
    kind: 'native',
    call(method, params, timeoutMs) {
      const resultId = ++sequence;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { waiting.delete(resultId); reject(new Error(`${method} timed out`)); }, timeoutMs);
        waiting.set(resultId, { resolve, reject, timer });
        backend.emitEvent('__juce__invoke', { name: 'flowsRequest', params: [{ id: String(resultId), method, params }], resultId });
      });
    },
    listen(event, callback) {
      const token = backend.addEventListener(event, callback);
      return () => backend.removeEventListener(token);
    },
  };
}
