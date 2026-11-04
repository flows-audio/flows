# Flows in the browser

The browser build runs the same instrument as the macOS app. The C++ app core,
compiler and DSP engine are compiled to WebAssembly; the interface is the same
React editor. Nothing is re-implemented in JavaScript, so an instrument sounds,
compiles and reports diagnostics identically in both hosts.

## How it runs

| Part | macOS app | Browser |
| --- | --- | --- |
| Requests and documents | `AppCore` on the WebView's message thread | `AppCore` on the page's main thread |
| Compile, analysis, assistant | `std::thread` workers | WebAssembly pthreads (Web Workers) |
| Audio callback | JUCE `processBlock` → `AudioRuntime` | Wasm Audio Worklet → `AudioRuntime` |
| Workspace files | Application Support folder | Emscripten file system persisted to IndexedDB |
| Assistant HTTP | JUCE `WebInputStream` | the page's `fetch()`; the AI thread waits |
| API keys | macOS Keychain | memory for this tab only |
| MIDI | CoreMIDI through JUCE | Web MIDI |
| Device settings | native Audio & MIDI dialog | in-app Audio & MIDI panel |

The audio worklet and the core share one WebAssembly memory, so the
lock-free queues between the editor side and the audio callback work exactly
as they do natively. Browsers allow shared memory only on
**cross-origin isolated** pages, which is why hosting needs two headers (below).

`src/web/WebHost.cpp` exposes a small C API (`flows_request`, `flows_state`,
`flows_telemetry`, `flows_audio_attach`, `flows_midi`, …).
`ui/src/web/host.ts` turns it into the same transport the native WebView
uses, and adds the browser-only methods: audio start/pause, Web MIDI, file
pickers, clipboard and links. `ui/src/web/boot.tsx` checks browser support,
claims the workspace for the tab, loads the core and renders the editor.

## Build and run locally

You need everything for the portable build (CMake, Ninja, Python 3, Node 22+)
plus **Emscripten 6.0.13**:

```sh
# Emscripten via emsdk (any platform)
git clone --branch 6.0.13 https://github.com/emscripten-core/emsdk.git ../emsdk
../emsdk/emsdk install 6.0.13 && ../emsdk/emsdk activate 6.0.13
source ../emsdk/emsdk_env.sh

emcmake cmake --preset web
cmake --build --preset web --parallel 2
ctest --preset web          # the portable suites, run as WebAssembly in Node

npm ci --prefix ui
npm run build:web --prefix ui    # ui/dist-web: a complete static site
npm run preview:web --prefix ui  # http://127.0.0.1:4173 with the right headers
```

`npm run dev:web --prefix ui` serves the editor with hot reload against the
core in `build-web/web` (override with `FLOWS_WASM_DIR`).

## Hosting

Serve `ui/dist-web` (or the `Flows-<version>-web.zip` release asset) from any
static host that can send these headers on every response:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Cloudflare Pages and Netlify read them from the bundled `_headers` file, which
also sets long-lived caching for hashed assets. Without the headers the page
explains that the browser refused shared memory instead of starting.

GitHub Pages cannot send custom headers. A service-worker shim such as
coi-serviceworker can add them after a reload, but it is a workaround: the
first visit reloads, and some browsers or privacy settings block it. Prefer a
host that supports headers for the editor, and keep the project site on
GitHub Pages with a link to it.

The site is free software under AGPL-3.0-only. Anyone hosting it must offer
users the corresponding source: the editor links to the matching tag
(*Appearance → Source code*), and the release ZIP includes `LICENSE`,
`SOURCE.txt` and third-party notices.

## Browser support

| Feature | Chrome / Edge | Firefox | Safari |
| --- | --- | --- | --- |
| Editor, compile, Signal Flow, probes | ✓ | ✓ | ✓ |
| Audio (Wasm Audio Worklet) | ✓ | ✓ | ✓ |
| MIDI keyboards (Web MIDI) | ✓ | ✓ | — use on-screen or computer keys |
| Workspace in browser storage | ✓ | ✓ | ✓ |

Continuous integration runs the browser tests in Chromium, Firefox and WebKit.

**Starting sound.** Browsers only start audio after a user gesture. The first
key press or click anywhere starts it; until then the instrument still compiles
and edits. The header shows *Sound off* and the Audio & MIDI panel explains.

**Your workspace.** Instruments, samples, settings and recovery live in this
browser's storage on this device, not on a server. Export instrument packages
to keep copies or move them to another computer. Browsers may evict storage
under pressure unless the site is allowed to keep it; the Audio & MIDI panel
shows usage and offers *Keep this workspace*.

**One tab at a time.** The workspace opens in one tab. Opening Flows in a
second tab offers *Use Flows here*: the first tab saves, stops and shows that
Flows moved.

**Imports.** WAV and AIFF import as they are. Other formats (MP3, FLAC, Ogg,
M4A) are decoded by the browser and stored as 48 kHz float WAV.

**Assistant.** API keys stay in memory for the current tab and are never
written to storage. The endpoint must allow requests from the page's origin
(CORS). A local model server usually needs its allowed origins configured.

## Keyboard shortcuts

Browsers reserve some shortcuts for their own tabs, so the web build moves
those to Option (macOS) or Alt:

| Action | macOS app | Browser |
| --- | --- | --- |
| Close editor tab | ⌘W | ⌥W / Alt+W |
| Focus editor group 1 / 2 | ⌘1 / ⌘2 | ⌥1 / ⌥2 |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab | ⌥] / ⌥[ |
| Split editors | ⌘\ | ⌘\ |
| Interface text size | ⌘+ / ⌘− / ⌘0 | Appearance slider; ⌘± zooms the page |

Everything else (⌘K, ⌘S, panel toggles, computer-key playing) is the same.

## Keeping the hosts together

- New behaviour belongs in `src/app` and the protocol, not in a host. A host
  implements only platform services through the core's ports.
- Every UI request is defined in `protocol/flows-protocol.json`. After editing
  it, run `npm run protocol --prefix ui` to regenerate `ui/src/protocol.gen.ts`.
- When a host cannot offer a feature, publish a capability and let the UI hide
  or explain it. Never branch on the transport.
- `ctest --preset web` runs the portable suites as WebAssembly. CI also runs
  the scripted conformance session on both cores and requires identical
  responses, renders every audio test case natively and as WebAssembly and
  compares the results, then runs the browser tests in Chromium, Firefox and
  WebKit (`npm run test:web --prefix ui`).
- A release builds the macOS bundle and the web site from the same tag; both
  are tested before either is published.
