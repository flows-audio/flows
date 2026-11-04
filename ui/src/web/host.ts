// The browser host: a Transport that runs the WebAssembly core in this page.
// Core methods go straight to C++; host methods (dialogs, clipboard, links,
// audio and MIDI devices) are implemented here with browser APIs.
import type { Transport } from '../client';
import type { AudioDevices } from '../protocol.gen';
import { WebAudio } from './audio';
import { installFetch } from './fetch';
import { audioAccept, browserAudio, bytesToBase64, download, importableAudio, pickFile, readImportableAudio } from './files';
import { WebMidi } from './midi';
import { takeString, withStrings, type FlowsModule } from './module';

type Listener = (payload: any) => void;

export class WebHost implements Transport {
  readonly kind = 'web' as const;
  readonly audio: WebAudio;
  readonly midi: WebMidi;
  private readonly listeners = new Map<string, Set<Listener>>();
  private readonly timers: ReturnType<typeof setInterval>[] = [];
  private stopped = false;

  constructor(private readonly module: FlowsModule) {
    this.audio = new WebAudio(module, () => this.emit('flowsDevices', this.devices()));
    this.midi = new WebMidi(module, () => this.emit('flowsDevices', this.devices()));
    installFetch(module);
  }

  start() {
    const poll = (milliseconds: number, work: () => void) => this.timers.push(setInterval(() => { if (!this.stopped) work(); }, milliseconds));
    poll(200, () => {
      const delta = takeString(this.module, this.module._flows_state());
      if (delta) this.emit('flowsState', JSON.parse(delta));
    });
    poll(33, () => {
      if (document.visibilityState !== 'visible' || !this.listeners.get('flowsTelemetry')?.size) return;
      const telemetry = takeString(this.module, this.module._flows_telemetry());
      const frame = telemetry ? JSON.parse(telemetry) : null;
      if (frame) this.emit('flowsTelemetry', frame);
    });
    poll(100, () => {
      const events = takeString(this.module, this.module._flows_agent_events());
      if (events && events !== '[]') for (const event of JSON.parse(events)) this.emit('flowsAgent', event);
    });
  }
  /** Stops events and audio (another tab took over, or the page is leaving). */
  stop() {
    this.stopped = true;
    this.timers.splice(0).forEach(clearInterval);
    this.audio.stop();
    this.midi.stop();
  }

  listen(event: string, callback: Listener) {
    let set = this.listeners.get(event);
    if (!set) this.listeners.set(event, set = new Set());
    set.add(callback);
    return () => { set!.delete(callback); };
  }
  private emit(event: string, payload: unknown) {
    this.listeners.get(event)?.forEach(listener => listener(payload));
  }

  devices(): AudioDevices { return { audio: this.audio.describe(), midi: this.midi.describe() }; }

  /** Calls the WebAssembly core synchronously. */
  core(method: string, params: Record<string, unknown> = {}): any {
    if (this.stopped) throw new Error('Flows is open in another tab.');
    const text = withStrings(this.module, [method, JSON.stringify(params)], (m, p) => takeString(this.module, this.module._flows_request(m, p)));
    const reply = JSON.parse(text ?? '{"ok":false,"error":"The core returned no reply."}');
    if (!reply.ok) throw new Error(reply.error);
    return reply.result;
  }

  async call(method: string, params: Record<string, unknown>): Promise<unknown> {
    if (this.stopped) throw new Error('Flows is open in another tab.');
    switch (method) {
      case 'audio.devices': return this.devices();
      case 'audio.start': this.audio.start(); return this.devices();
      case 'audio.suspend': await this.audio.suspend(); return this.devices();
      case 'midi.request': await this.midi.request(); return this.devices();
      case 'midi.enable': this.midi.setEnabled(String(params.id ?? ''), params.enabled === true); return this.devices();
      case 'audio.settings':
        window.dispatchEvent(new CustomEvent('flows:audio-settings'));
        return this.core('session.get');
      case 'clipboard.write':
        if (!navigator.clipboard?.writeText) throw new Error('This browser does not allow copying here.');
        await navigator.clipboard.writeText(String(params.text ?? ''));
        return { ok: true };
      case 'browser.open': {
        const url = new URL(String(params.url ?? ''));
        if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) throw new Error('Open a valid HTTP or HTTPS link without credentials.');
        window.open(url.href, '_blank', 'noopener,noreferrer');
        return { ok: true };
      }
      // Dialog methods work when the call still carries the click's user
      // activation. The UI prefers to pick the file itself (fileDialogs: browser).
      case 'project.import': {
        const file = await pickFile('.flowsinst,application/json');
        return file ? this.core('project.importText', { text: await file.text() }) : this.core('session.get');
      }
      case 'project.export': {
        const snapshot = this.core('session.get');
        const name = String(snapshot.document?.path ?? 'Instrument').split('/').pop()!.replace(/\.flows$/i, '') || 'Instrument';
        download(`${name}.flowsinst`, this.core('project.exportText'), 'application/json');
        return snapshot;
      }
      case 'sample.import': {
        const file = await pickFile(audioAccept);
        if (!file) return this.core('session.get');
        const audio = await readImportableAudio(file);
        const result = this.core('workspace.import', { folder: 'Samples', name: audio.name, data: bytesToBase64(audio.bytes) });
        return { ...result, sampleImportedPath: result.importedPath };
      }
      case 'workspace.import': {
        const name = String(params.name ?? '');
        if (!browserAudio.test(name)) break;
        // Compressed audio is decoded by the browser before the core sees it.
        const bytes = Uint8Array.from(atob(String(params.data ?? '')), character => character.charCodeAt(0));
        const audio = await importableAudio(name, bytes);
        return this.core('workspace.import', { ...params, name: audio.name, data: bytesToBase64(audio.bytes) });
      }
    }
    return this.core(method, params);
  }
}
