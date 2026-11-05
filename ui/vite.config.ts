import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

// Shared memory between the page, its threads and the audio worklet needs
// cross-origin isolation. Production hosts send the same headers (public-web/_headers).
const isolation = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' };

// Serves and bundles the WebAssembly core from `cmake --build --preset web`.
function flowsCore(directory: string): Plugin {
  const files: Record<string, { path: string; type: string }> = {
    'core/flows-core.mjs': { path: resolve(directory, 'flows-core.mjs'), type: 'text/javascript' },
    'core/flows-core.wasm': { path: resolve(directory, 'flows-core.wasm'), type: 'application/wasm' },
    'icon.svg': { path: resolve(__dirname, '../assets/flows-icon.svg'), type: 'image/svg+xml' },
  };
  const missing = (path: string) => `Missing ${path}. Build the web core first: emcmake cmake --preset web && cmake --build --preset web`;
  return {
    name: 'flows-core',
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const name = (request.url ?? '').split('?')[0].replace(/^\//, '');
        const file = files[name];
        if (!file) return next();
        if (!existsSync(file.path)) { response.statusCode = 404; response.end(missing(file.path)); return; }
        response.setHeader('Content-Type', file.type);
        for (const [header, value] of Object.entries(isolation)) response.setHeader(header, value);
        response.end(readFileSync(file.path));
      });
    },
    generateBundle() {
      for (const [fileName, file] of Object.entries(files)) {
        if (!existsSync(file.path)) this.error(missing(file.path));
        this.emitFile({ type: 'asset', fileName, source: readFileSync(file.path) });
      }
    },
    transformIndexHtml: html => html.replace('</head>', [
      '<link rel="icon" href="./icon.svg" type="image/svg+xml"/>',
      '<link rel="manifest" href="./manifest.webmanifest"/>',
      '<meta name="description" content="Build a sound in code. Listen, observe and compare at every step."/>',
      '<meta name="theme-color" content="#121716"/>',
      '</head>'].join('')),
  };
}

export default defineConfig(({ mode }) => {
  const web = mode === 'web';
  return {
    base: './',
    plugins: [react(), ...(web ? [flowsCore(resolve(__dirname, process.env.FLOWS_WASM_DIR ?? '../build-web/web'))] : [])],
    define: { __FLOWS_WEB__: JSON.stringify(web) },
    publicDir: web ? 'public-web' : false,
    server: { headers: isolation, fs: { allow: ['..'] } },
    preview: { headers: isolation },
    build: web
      ? { outDir: 'dist-web', target: 'es2020', emptyOutDir: true }
      : { target: 'es2020', cssCodeSplit: false,
          rollupOptions: { output: { entryFileNames: 'assets/index.js', chunkFileNames: 'assets/[name].js',
            assetFileNames: asset => asset.name?.endsWith('.css') ? 'assets/index.css' : 'assets/[name][extname]' } } },
  };
});
