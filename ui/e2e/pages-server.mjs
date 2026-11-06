// Serves dist-web the way GitHub Pages does: under /flows/ and without the
// cross-origin isolation headers, so the service worker has to provide them.
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '../dist-web');
const port = Number(process.argv[2] ?? 4179);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.json': 'application/json', '.webmanifest': 'application/manifest+json' };

createServer(async (request, response) => {
  const path = decodeURIComponent(new URL(request.url ?? '/', 'http://localhost').pathname);
  if (path === '/flows') { response.writeHead(301, { Location: '/flows/' }).end(); return; }
  if (!path.startsWith('/flows/')) { response.writeHead(404).end(); return; }
  let file = normalize(join(root, path.slice('/flows/'.length)));
  if (!file.startsWith(root)) { response.writeHead(404).end(); return; }
  try {
    if ((await stat(file)).isDirectory()) file = join(file, 'index.html');
    const body = await readFile(file);
    response.writeHead(200, { 'Content-Type': types[extname(file)] ?? 'application/octet-stream' }).end(body);
  } catch {
    response.writeHead(404).end();
  }
}).listen(port, '127.0.0.1');
