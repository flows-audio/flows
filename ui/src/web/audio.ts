// Browser audio output. An AudioContext may only start inside a user gesture,
// so the first pointer or key press anywhere starts it; until then the
// instrument compiles and edits normally but stays silent.
import type { AudioDevices } from '../protocol.gen';
import type { FlowsModule } from './module';

type State = AudioDevices['audio']['state'];

export class WebAudio {
  state: State;
  error = '';
  context?: AudioContext;
  private attached = false;
  private disarm?: () => void;

  constructor(private readonly module: FlowsModule, private readonly changed: () => void) {
    const supported = typeof AudioContext !== 'undefined' && 'audioWorklet' in AudioContext.prototype;
    this.state = supported ? 'off' : 'unsupported';
    module.onFlowsAudio = state => {
      if (state === 'running') this.attached = true;
      else { this.state = 'failed'; this.error = 'The audio engine could not start in this browser.'; }
      this.update();
    };
  }

  /** Must run inside a user gesture (click, key or pointer press). */
  start() {
    if (this.state === 'unsupported') return;
    if (!this.context) {
      try { this.context = new AudioContext({ latencyHint: 'interactive' }); }
      catch (error) { this.state = 'failed'; this.error = error instanceof Error ? error.message : String(error); this.changed(); return; }
      this.context.onstatechange = () => this.update();
      this.state = 'starting';
      this.module._flows_audio_attach(this.module.emscriptenRegisterAudioObject(this.context));
    }
    void this.context.resume().catch(error => { this.error = String(error); this.update(); });
    this.update();
  }
  async suspend() {
    await this.context?.suspend();
    this.update();
  }
  /** Starts audio on the first pointer or key press anywhere on the page. */
  armAutoStart() {
    if (this.disarm || this.state === 'unsupported') return;
    const handler = () => { this.disarm?.(); this.start(); };
    const options = { capture: true };
    for (const type of ['pointerdown', 'keydown'] as const) window.addEventListener(type, handler, options);
    this.disarm = () => {
      for (const type of ['pointerdown', 'keydown'] as const) window.removeEventListener(type, handler, options);
      this.disarm = undefined;
    };
  }
  describe(): AudioDevices['audio'] {
    const context = this.context;
    return {
      state: this.state, sampleRate: context?.sampleRate ?? 0,
      ...(context?.baseLatency !== undefined ? { baseLatency: context.baseLatency } : {}),
      ...(context?.outputLatency !== undefined ? { outputLatency: context.outputLatency } : {}),
      ...(this.error ? { error: this.error } : {}),
    };
  }
  private update() {
    const context = this.context;
    if (context && this.state !== 'failed') {
      const live = context.state === 'running' && this.attached;
      this.state = live ? 'running' : !this.attached ? 'starting' : 'suspended';
      this.module._flows_audio_running(live ? 1 : 0, context.sampleRate);
    }
    this.changed();
  }
}
