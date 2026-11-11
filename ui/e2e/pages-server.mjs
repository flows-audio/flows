// Serves dist-web the way GitHub Pages does: under /flows/, without the
// cross-origin isolation headers (the service worker has to provide them) and
// with Access-Control-Allow-Origin: * on every file, so other sites can load
// the embed (e2e/embed.spec.ts).
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '../dist-web');
const port = Number(process.argv[2] ?? 4179);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.json': 'application/json', '.webmanifest': 'application/manifest+json',
  '.flows': 'text/plain; charset=utf-8', '.wav': 'audio/wav', '.md': 'text/markdown; charset=utf-8' };
const cors = { 'Access-Control-Allow-Origin': '*' };

createServer(async (request, response) => {
  const path = decodeURIComponent(new URL(request.url ?? '/', 'http://localhost').pathname);
  if (path === '/flows') { response.writeHead(301, { Location: '/flows/' }).end(); return; }
  if (!path.startsWith('/flows/')) { response.writeHead(404).end(); return; }
  let file = normalize(join(root, path.slice('/flows/'.length)));
  if (!file.startsWith(root)) { response.writeHead(404).end(); return; }
  try {
    if ((await stat(file)).isDirectory()) file = join(file, 'index.html');
    const body = await readFile(file);
    response.writeHead(200, { ...cors, 'Content-Type': types[extname(file)] ?? 'application/octet-stream' }).end(body);
  } catch {
    response.writeHead(404, cors).end();
  }
}).listen(port, '127.0.0.1');
