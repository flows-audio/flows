// Cross-origin isolation on hosts that cannot send headers. public-web/isolation.js
// adds the headers to every response from this site once it controls the page,
// so the page reloads once after registering it. Hosts that send the headers
// themselves are already isolated and never register the worker.
const reloadKey = 'flows.isolationReload';
const readyTimeoutMs = 10_000;

function readFlag(): boolean {
  try { return sessionStorage.getItem(reloadKey) === '1'; } catch { return false; }
}
function writeFlag(on: boolean): boolean {
  try {
    if (on) sessionStorage.setItem(reloadKey, '1'); else sessionStorage.removeItem(reloadKey);
    return true;
  } catch { return false; }
}

// True when the page is reloading under the worker; the caller stops booting.
export async function isolateWithServiceWorker(): Promise<boolean> {
  if (globalThis.crossOriginIsolated) {
    writeFlag(false);
    return false;
  }
  if (!window.isSecureContext || !('serviceWorker' in navigator)) return false;
  // One reload per tab session: if the worker did not isolate the page, report
  // the browser as unsupported instead of reloading forever.
  if (readFlag()) return false;
  try {
    await navigator.serviceWorker.register(new URL('isolation.js', document.baseURI), { scope: './' });
    // `ready` never settles when installation fails; give up instead of
    // leaving the loading screen up.
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new Error('the worker did not activate')), readyTimeoutMs);
    });
    try { await Promise.race([navigator.serviceWorker.ready, timeout]); } finally { clearTimeout(timer); }
  } catch (error) {
    console.warn('Flows could not start its isolation service worker', error);
    return false;
  }
  if (!writeFlag(true)) return false;
  location.reload();
  return true;
}
