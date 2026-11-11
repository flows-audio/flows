// Builds @flows-audio/embed into embed/dist:
//   flows-embed.js           ES module: engine + elements (defines the elements)
//   flows-embed-core.js      ES module: engine only, no elements
//   flows-embed-elements.js  ES module: elements (same as flows-embed.js)
//   flows-embed.iife.js      classic <script>: window.Flows, defines the elements
//   flows-embed-worklet.js   the AudioWorklet processor (also inlined as a Blob)
//   flows-embed.wasm         compiler + DSP engine (cmake --build --preset web)
//   presets/                 bundled instruments and their samples
//   skill/SKILL.md, examples/ and types/
//
// Usage: node scripts/build.mjs [--wasm path/to/flows-embed.wasm]
import { build } from 'esbuild';
import { execFileSync } from 'node:child_process';
import { brotliCompressSync, gzipSync, constants } from 'node:zlib';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repo = resolve(here, '..');
const dist = join(here, 'dist');
const argument = (name) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : undefined; };
const wasm = resolve(argument('--wasm') ?? process.env.FLOWS_EMBED_WASM ?? join(repo, 'build-web/embed/flows-embed.wasm'));
if (!existsSync(wasm)) {
  console.error(`Missing ${wasm}.\nBuild it first: emcmake cmake --preset web && cmake --build --preset web --target flows_embed`);
  process.exit(1);
}
const version = JSON.parse(readFileSync(join(here, 'package.json'), 'utf8')).version;
const banner = `/*! @flows-audio/embed ${version} | Apache-2.0 | https://github.com/flows-audio/flows */`;

rmSync(dist, { recursive: true, force: true });
mkdirSync(dist, { recursive: true });

// 1. The worklet, as a file and as a string inlined into the main bundles.
const worklet = await build({
  entryPoints: [join(here, 'src/worklet.ts')], bundle: true, format: 'iife', minify: true,
  target: 'es2020', write: false, legalComments: 'none', banner: { js: banner },
});
const workletSource = worklet.outputFiles[0].text;
writeFileSync(join(dist, 'flows-embed-worklet.js'), workletSource);
const inlineWorklet = {
  name: 'flows-worklet',
  setup(b) {
    b.onResolve({ filter: /^flows:worklet$/ }, () => ({ path: 'worklet', namespace: 'flows' }));
    b.onLoad({ filter: /.*/, namespace: 'flows' }, () => ({ contents: `export const WORKLET_SOURCE = ${JSON.stringify(workletSource)};`, loader: 'js' }));
  },
};
const common = { bundle: true, minify: true, target: 'es2020', legalComments: 'none', banner: { js: banner }, plugins: [inlineWorklet], logLevel: 'warning' };

// 2. ES modules. Splitting keeps one copy of the engine (one shared AudioContext)
// when a page imports both entry points. Chunks stay beside the entries so
// new URL('./flows-embed.wasm', import.meta.url) resolves from any of them.
await build({
  ...common,
  entryPoints: { 'flows-embed': join(here, 'src/index.ts'), 'flows-embed-core': join(here, 'src/core.ts'), 'flows-embed-elements': join(here, 'src/elements/index.ts') },
  outdir: dist, format: 'esm', splitting: true, chunkNames: 'flows-embed-[hash]',
});

// 3. A classic script for <script src> pages (Jekyll, WordPress, plain HTML).
await build({
  ...common,
  entryPoints: [join(here, 'src/iife.ts')], outfile: join(dist, 'flows-embed.iife.js'), format: 'iife',
  define: { 'import.meta.url': '__flowsScript' },
  banner: { js: `${banner}\nvar __flowsScript=(typeof document!=="undefined"&&document.currentScript&&document.currentScript.src)||"";` },
});

// 4. Engine, presets, samples, skill and examples.
cpSync(wasm, join(dist, 'flows-embed.wasm'));
mkdirSync(join(dist, 'presets/Samples'), { recursive: true });
for (const folder of [join(repo, 'test/presets'), join(here, 'presets')])
  for (const name of readdirSync(folder)) if (name.endsWith('.flows')) cpSync(join(folder, name), join(dist, 'presets', name));
for (const name of ['flows-kick.wav', 'flows-stereo.wav']) cpSync(join(repo, 'test/fixtures/samples', name), join(dist, 'presets/Samples', name));
cpSync(join(here, 'skill'), join(dist, 'skill'), { recursive: true });
cpSync(join(repo, 'licenses/third-party/emscripten'), join(dist, 'licenses'), { recursive: true });
cpSync(join(here, 'examples'), join(dist, 'examples'), { recursive: true });
// The gallery also serves as <site>/embed/index.html.
writeFileSync(join(dist, 'index.html'), readFileSync(join(here, 'examples/index.html'), 'utf8')
  .replaceAll('"../', '"./').replace(/href="([a-z-]+\.html)"/g, 'href="examples/$1"'));

// 5. Type declarations.
execFileSync(process.execPath, [join(here, 'node_modules/typescript/bin/tsc'), '-p', join(here, 'tsconfig.json')], { stdio: 'inherit' });

// 6. Transfer sizes (what a visitor downloads).
const sizes = [];
const measure = (file) => {
  const data = readFileSync(join(dist, file));
  return { file, raw: data.length, gzip: gzipSync(data, { level: 9 }).length, brotli: brotliCompressSync(data, { params: { [constants.BROTLI_PARAM_QUALITY]: 11 } }).length };
};
const chunks = readdirSync(dist).filter((name) => /^flows-embed-[A-Z0-9]+\.js$/.test(name));
for (const file of ['flows-embed.js', ...chunks, 'flows-embed-core.js', 'flows-embed-elements.js', 'flows-embed.iife.js', 'flows-embed-worklet.js', 'flows-embed.wasm']) sizes.push(measure(file));
const kb = (n) => `${(n / 1024).toFixed(1)} KB`;
console.log('file'.padEnd(28), 'raw'.padStart(10), 'gzip'.padStart(10), 'brotli'.padStart(10));
for (const s of sizes) console.log(s.file.padEnd(28), kb(s.raw).padStart(10), kb(s.gzip).padStart(10), kb(s.brotli).padStart(10));
const page = sizes.filter((s) => s.file === 'flows-embed.iife.js' || s.file === 'flows-embed.wasm');
console.log(`<script> page download (iife + wasm): ${kb(page.reduce((a, s) => a + s.gzip, 0))} gzip, ${kb(page.reduce((a, s) => a + s.brotli, 0))} brotli`);
writeFileSync(join(dist, 'sizes.json'), JSON.stringify(sizes, null, 2) + '\n');
