// Flows' audio engine shares memory with its audio thread, which browsers allow
// only on cross-origin isolated pages. Hosts that cannot send the isolation
// headers (GitHub Pages) get them from this service worker instead; hosts that
// can send them never register it (see ui/src/web/isolation.ts).
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);
  // Other origins (the assistant's API) keep their own CORS handling.
  if (request.method !== 'GET' || url.origin !== self.location.origin) return;
  // The engine keeps fixed file names; revalidate it so a new page never
  // starts with the previous engine.
  const fresh = request.mode !== 'navigate' && url.pathname.includes('/core/');
  event.respondWith(fetch(fresh ? new Request(request, { cache: 'no-cache' }) : request).then(response => {
    if (response.status === 0) return response;
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
