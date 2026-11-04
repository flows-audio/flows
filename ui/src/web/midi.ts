// Web MIDI input. Behaves like the macOS app: new keyboards are enabled
// automatically, a keyboard the user turned off stays off, and unplugging or
// disabling any input releases every note held from MIDI.
import type { AudioDevices, MidiInput } from '../protocol.gen';
import type { FlowsModule } from './module';

const disabledKey = 'flows.midi.disabled';

export class WebMidi {
  state: AudioDevices['midi']['state'];
  error = '';
  private access?: MIDIAccess;
  private disabled = new Set<string>();
  private attached = new Map<string, MIDIInput>();

  constructor(private readonly module: FlowsModule, private readonly changed: () => void) {
    this.state = typeof navigator.requestMIDIAccess === 'function' ? 'prompt' : 'unsupported';
    try { this.disabled = new Set(JSON.parse(localStorage.getItem(disabledKey) ?? '[]')); } catch { /* defaults */ }
    // Reconnect silently when the browser already granted access.
    if (this.state === 'prompt') void navigator.permissions?.query({ name: 'midi' as PermissionName })
      .then(status => { if (status.state === 'granted') void this.request(); else if (status.state === 'denied') { this.state = 'denied'; this.changed(); } })
      .catch(() => { /* Permission query unsupported: wait for the user. */ });
  }

  /** Asks for MIDI access. Call from a user gesture the first time. */
  async request() {
    if (this.state === 'unsupported') return;
    try {
      this.access = await navigator.requestMIDIAccess({ sysex: false });
      this.state = 'granted'; this.error = '';
      this.access.onstatechange = () => this.refresh();
      this.refresh();
    } catch (error) {
      this.state = 'denied';
      this.error = error instanceof Error ? error.message : String(error);
      this.changed();
    }
  }
  setEnabled(id: string, enabled: boolean) {
    if (enabled) this.disabled.delete(id); else this.disabled.add(id);
    try { localStorage.setItem(disabledKey, JSON.stringify([...this.disabled])); } catch { /* session only */ }
    this.refresh();
  }
  describe(): AudioDevices['midi'] {
    const inputs: MidiInput[] = [];
    this.access?.inputs.forEach(input => inputs.push({
      id: input.id, name: input.name || 'MIDI input', ...(input.manufacturer ? { manufacturer: input.manufacturer } : {}),
      enabled: !this.disabled.has(input.id), connected: input.state === 'connected',
    }));
    return { state: this.state, inputs, ...(this.error ? { error: this.error } : {}) };
  }
  private refresh() {
    let lost = false;
    const present = new Set<string>();
    this.access?.inputs.forEach(input => {
      present.add(input.id);
      const wanted = input.state === 'connected' && !this.disabled.has(input.id);
      if (wanted && !this.attached.has(input.id)) {
        input.onmidimessage = event => {
          const data = event.data;
          if (!data?.length) return;
          this.module._flows_midi(data[0], data[1] ?? 0, data[2] ?? 0, Math.min(3, data.length));
        };
        this.attached.set(input.id, input);
      } else if (!wanted && this.attached.has(input.id)) {
        input.onmidimessage = null;
        this.attached.delete(input.id);
        lost = true;
      }
    });
    for (const [id, input] of this.attached) if (!present.has(id)) { input.onmidimessage = null; this.attached.delete(id); lost = true; }
    if (lost) this.module._flows_release_midi();
    this.changed();
  }
}
