// Starts Flows in the browser: checks support, claims the workspace for this
// tab, loads the WebAssembly core, mounts storage and renders the editor.
import { StrictMode, type ReactElement } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from '../App';
import { setTransport } from '../client';
import type { Capabilities } from '../protocol.gen';
import { BootScreen } from './BootScreen';
import { WebHost } from './host';
import { claimWorkspace } from './locks';
import { withStrings, type FlowsModule } from './module';
import { flushWorkspace, mountWorkspace, settingsPath, workspacePath } from './storage';

function unsupportedReason(): string | undefined {
  if (typeof WebAssembly !== 'object') return 'This browser has no WebAssembly.';
  if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined')
    return 'This page was served without cross-origin isolation, which the audio engine needs to share memory with its audio thread. If you host Flows yourself, send the Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers described in docs/WEB.md.';
  return undefined;
}
function applySavedTheme() {
  const root = document.documentElement;
  root.dataset.host = 'web';
  try { root.dataset.theme = localStorage.getItem('flows.theme') || 'moss'; } catch { root.dataset.theme = 'moss'; }
}

export async function bootWeb(container: HTMLElement) {
  applySavedTheme();
  const root = createRoot(container);
  const show = (element: ReactElement) => root.render(element);
  show(<BootScreen kind="loading"/>);
  const problem = unsupportedReason();
  if (problem) { show(<BootScreen kind="unsupported" detail={problem}/>); return; }

  let module: FlowsModule | undefined;
  let host: WebHost | undefined;
  const claim = await claimWorkspace({
    busy: takeOver => show(<BootScreen kind="busy" onTakeOver={() => { show(<BootScreen kind="loading" detail="Waiting for the other tab to save…"/>); takeOver(); }}/>),
    lost: async () => {
      host?.stop();
      if (module) await flushWorkspace(module);
      show(<BootScreen kind="moved"/>);
    },
  });
  show(<BootScreen kind="loading"/>);
  try {
    const url = new URL('core/flows-core.mjs', document.baseURI).href;
    const { default: create } = await import(/* @vite-ignore */ url);
    const core: FlowsModule = await create({ printErr: (text: string) => console.error(text) });
    module = core;
    const storage = await mountWorkspace(core);
    const capabilities: Partial<Capabilities> = {
      host: 'web', audioSettings: 'in-app', midiInput: typeof navigator.requestMIDIAccess === 'function' ? 'web' : 'none',
      storage, secrets: 'session', fileDialogs: 'browser', clipboard: navigator.clipboard ? 'browser' : 'none',
      externalLinks: 'browser', liveBridge: false, ai: true,
    };
    const started = withStrings(core, [workspacePath, settingsPath, JSON.stringify(capabilities)], (w, s, c) => core._flows_start(w, s, c));
    if (!started) throw new Error('The instrument engine could not start. Details are in the browser console.');
    host = new WebHost(core);
    host.start();
    host.audio.armAutoStart();
    setTransport(host);
    // The same request API the editor uses, for the browser console and tests.
    const webHost = host;
    Object.defineProperty(window, '__flows', { configurable: true, value: {
      request: (method: string, params: Record<string, unknown> = {}) => webHost.call(method, params),
      devices: () => webHost.devices(),
    } });
    const flush = () => void flushWorkspace(core);
    window.addEventListener('pagehide', () => { flush(); claim.release(); });
    // A page restored from the back/forward cache no longer holds the
    // workspace lock; start again so tab ownership is checked.
    window.addEventListener('pageshow', event => { if (event.persisted) location.reload(); });
    document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') flush(); });
    show(<StrictMode><App/></StrictMode>);
  } catch (error) {
    show(<BootScreen kind="failed" detail={error instanceof Error ? error.message : String(error)}/>);
  }
}
