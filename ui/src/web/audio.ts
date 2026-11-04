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
  private stopped = false;
  private disarm?: () => void;

  constructor(private readonly module: FlowsModule, private readonly changed: () => void) {
    const supported = typeof AudioContext !== 'undefined' && 'audioWorklet' in AudioContext.prototype;
    this.state = supported ? 'off' : 'unsupported';
    module.onFlowsAudio = state => {
      if (state === 'running') this.attached = true;
      else { this.state = 'failed'; this.error = 'The audio engine could not start in this browser.'; this.disarm?.(); }
      this.update();
    };
  }

  /** Must run inside a user gesture (click, key or pointer press). */
  start() {
    if (this.stopped || this.state === 'unsupported') return;
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
  /** Another tab owns the workspace: release the output for good. */
  stop() {
    this.stopped = true;
    this.disarm?.();
    void this.context?.close().catch(() => {}).finally(() => this.update());
  }
  /** Starts audio on a pointer or key press anywhere on the page. Stays armed
   * until sound runs: a touch counts as a user gesture only when it ends, and
   * Escape never does. */
  armAutoStart() {
    if (this.stopped || this.disarm || this.state === 'unsupported') return;
    const handler = () => this.start();
    const options = { capture: true };
    const types = ['pointerdown', 'pointerup', 'keydown'] as const;
    for (const type of types) window.addEventListener(type, handler, options);
    this.disarm = () => {
      for (const type of types) window.removeEventListener(type, handler, options);
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
      if (live) this.disarm?.();
      this.state = live ? 'running' : !this.attached ? 'starting' : 'suspended';
      this.module._flows_audio_running(live ? 1 : 0, context.sampleRate);
    }
    this.changed();
  }
}
