// Assistant HTTP for the browser host. The core's AI thread blocks while the
// page's main thread performs fetch(); completion is posted back to C++.
import type { FlowsModule } from './module';

// Matches HttpResult::Error in src/app/Platform.hpp.
const enum FetchError { none = 0, cancelled = 1, timeout = 2, connect = 3, tooLarge = 4 }

export function installFetch(module: FlowsModule) {
  const pending = new Map<number, AbortController>();
  module.flowsFetch = (call, url, body, headers, timeoutMs, limit) => {
    const controller = new AbortController();
    pending.set(call, controller);
    let timedOut = false;
    const timer = setTimeout(() => { timedOut = true; controller.abort(); }, timeoutMs);
    const finish = (status: number, bytes: Uint8Array | null, error: FetchError) => {
      clearTimeout(timer);
      pending.delete(call);
      let pointer = 0;
      if (bytes?.length) {
        pointer = module._malloc(bytes.length);
        // The core's memory is exhausted: report the body as too large to keep.
        if (!pointer) { module._flows_fetch_complete(call, status, 0, 0, FetchError.tooLarge); return; }
        module.HEAPU8.set(bytes, pointer);
      }
      module._flows_fetch_complete(call, status, pointer, bytes?.length ?? 0, error);
    };
    // Keys go only to the configured endpoint; no cookies, no referrer.
    fetch(url, { method: 'POST', headers, body, signal: controller.signal, redirect: 'error', credentials: 'omit', referrerPolicy: 'no-referrer', cache: 'no-store' })
      .then(async response => {
        const reader = response.body?.getReader();
        const chunks: Uint8Array[] = [];
        let total = 0;
        while (reader) {
          const { done, value } = await reader.read();
          if (done || !value) break;
          total += value.length;
          if (total > limit) { controller.abort(); finish(response.status, null, FetchError.tooLarge); return; }
          chunks.push(value);
        }
        const bytes = new Uint8Array(total);
        let at = 0;
        for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.length; }
        finish(response.status, bytes, FetchError.none);
      })
      .catch(() => finish(0, null, timedOut ? FetchError.timeout : controller.signal.aborted ? FetchError.cancelled : FetchError.connect));
  };
  module.flowsFetchAbort = call => pending.get(call)?.abort();
}
