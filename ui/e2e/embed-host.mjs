// A stand-in for "someone else's website" in e2e/embed.spec.ts. It serves the
// embed examples and test pages from its own origin (localhost:<port>) with no
// isolation headers and no service worker. Their production script URLs are
// pointed at the local GitHub Pages stand-in (pages-server.mjs, 127.0.0.1:4179),
// a different origin, so every script, .wasm, worklet and preset loads cross-origin.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const port = Number(process.argv[2] ?? 4180);
const assets = process.argv[3] ?? 'http://127.0.0.1:4179/flows/embed/';
const folders = { examples: resolve(import.meta.dirname, '../../embed/examples'), fixtures: resolve(import.meta.dirname, '../../embed/test/fixtures') };
const production = 'https://flows-audio.github.io/flows/embed/';

createServer(async (request, response) => {
  const url = new URL(request.url ?? '/', 'http://localhost');
  const [, folder, name] = url.pathname.split('/');
  if (!folders[folder] || !/^[a-z0-9-]+\.html$/.test(name ?? '')) { response.writeHead(404).end(); return; }
  try {
    const html = (await readFile(join(folders[folder], name), 'utf8')).replaceAll(production, assets).replaceAll('{{ASSETS}}', assets);
    const headers = { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' };
    // A strict policy: no blob: scripts, so the worklet must load from the hosted file.
    if (name === 'csp.html') headers['Content-Security-Policy'] = `default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval' ${new URL(assets).origin}; connect-src ${new URL(assets).origin}; style-src 'self' 'unsafe-inline'`;
    response.writeHead(200, headers).end(html);
  } catch {
    response.writeHead(404).end();
  }
}).listen(port, 'localhost');
