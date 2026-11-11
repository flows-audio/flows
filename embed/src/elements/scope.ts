// <flows-scope mode="wave|spectrum">: the instrument's output on a canvas.
// Colours come from --flows-scope-line / --flows-accent; it draws only while
// visible and sounding, and at a low frame rate with reduced motion.
import type { Flows, FlowsAnalyser } from '../core';
import { Base, InstrumentLink, TOKENS, adoptStyles, define } from './shared';

const STYLE = String.raw`
:host { display: block; }
.scope {
  position: relative; height: var(--flows-scope-height, 72px);
  background: var(--flows-scope-background, transparent);
  border-radius: var(--flows-scope-radius, calc(var(--flows-radius, 10px) * .6));
  overflow: hidden;
}
canvas { display: block; width: 100%; height: 100%; }
.probe-line { color: var(--flows-scope-line, var(--flows-accent, currentColor)); }
.probe-fill { color: var(--flows-scope-fill, transparent); }
.probe-glow { color: var(--flows-glow, transparent); }
.probe-grid { color: var(--flows-scope-grid, color-mix(in srgb, currentColor 10%, transparent)); }
`;

export class FlowsScopeElement extends Base {
  static observedAttributes = ['mode', 'instrument'];
  private link = new InstrumentLink(this, (flows) => this.bind(flows));
  private root: ShadowRoot | null = null;
  private analyser: FlowsAnalyser | null = null;
  private frame = 0;
  private visible = true;
  private observer: IntersectionObserver | null = null;
  private resize: ResizeObserver | null = null;
  private colors = { line: '', fill: '', glow: '', grid: '' };
  private colorAge = 0;
  private data: Float32Array | null = null;
  private lastDraw = 0;
  private offState: (() => void) | null = null;
  private motion: MediaQueryList | null = null;

  connectedCallback() {
    if (!this.root) {
      this.root = this.attachShadow({ mode: 'open' });
      adoptStyles(this.root, TOKENS, STYLE);
      this.root.innerHTML = `<div class="scope" part="scope"><canvas part="scope-canvas" role="img"></canvas>
        <span class="probe-line" hidden></span><span class="probe-fill" hidden></span><span class="probe-glow" hidden></span><span class="probe-grid" hidden></span></div>`;
    }
    this.canvas.setAttribute('aria-label', this.mode === 'spectrum' ? 'Spectrum of the instrument output' : 'Waveform of the instrument output');
    this.motion = typeof matchMedia !== 'undefined' ? matchMedia('(prefers-reduced-motion: reduce)') : null;
    if (typeof IntersectionObserver !== 'undefined') {
      this.observer = new IntersectionObserver((entries) => { this.visible = entries.some((e) => e.isIntersecting); this.schedule(); });
      this.observer.observe(this);
    }
    if (typeof ResizeObserver !== 'undefined') {
      this.resize = new ResizeObserver(() => { this.colorAge = 0; this.draw(true); });
      this.resize.observe(this);
    }
    this.link.connect();
  }
  disconnectedCallback() {
    this.observer?.disconnect();
    this.resize?.disconnect();
    cancelAnimationFrame(this.frame);
    this.frame = 0;
    this.link.disconnect();
  }
  attributeChangedCallback(name: string) {
    if (name === 'instrument' && this.root) this.link.connect();
    else { this.data = null; this.draw(true); }
  }

  get mode(): 'wave' | 'spectrum' { return this.getAttribute('mode') === 'spectrum' ? 'spectrum' : 'wave'; }
  private get canvas() { return this.root!.querySelector('canvas') as HTMLCanvasElement; }

  private bind(flows: Flows | null) {
    this.offState?.();
    this.offState = null;
    this.analyser = flows ? flows.analyser() : null;
    if (flows) this.offState = flows.on('meter', () => this.schedule());
    this.draw(true);
  }
  private schedule() {
    if (!this.frame && this.visible && this.isConnected) this.frame = requestAnimationFrame((time) => { this.frame = 0; this.tick(time); });
  }
  private tick(time: number) {
    const flows = this.link.flows;
    const reduced = !!this.motion?.matches;
    if (!reduced || time - this.lastDraw > 100) { this.draw(false); this.lastDraw = time; }
    // Keep animating while sound may still be decaying.
    if (flows && flows.state === 'running' && (flows.peak > 1e-4 || flows.voices > 0)) this.schedule();
    else this.draw(true);
  }

  private readColors() {
    const read = (selector: string) => getComputedStyle(this.root!.querySelector(selector)!).color;
    this.colors = { line: read('.probe-line'), fill: read('.probe-fill'), glow: read('.probe-glow'), grid: read('.probe-grid') };
  }
  private draw(silent: boolean) {
    if (!this.root) return;
    const canvas = this.canvas;
    const ratio = Math.min(2, globalThis.devicePixelRatio || 1);
    const width = Math.max(1, Math.round(canvas.clientWidth * ratio)), height = Math.max(1, Math.round(canvas.clientHeight * ratio));
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    const context = canvas.getContext('2d');
    if (!context) return;
    if (this.colorAge-- <= 0) { this.readColors(); this.colorAge = 30; }
    const lineWidth = Number.parseFloat(getComputedStyle(this).getPropertyValue('--flows-scope-line-width')) || 2;
    context.clearRect(0, 0, width, height);
    context.strokeStyle = this.colors.grid;
    context.lineWidth = ratio;
    context.beginPath();
    context.moveTo(0, height / 2);
    context.lineTo(width, height / 2);
    context.stroke();
    const analyser = this.analyser;
    const points: number[] = [];
    if (this.mode === 'wave') {
      const size = analyser?.size ?? 2048;
      if (!this.data || this.data.length !== size) this.data = new Float32Array(size);
      if (!silent && analyser) analyser.waveform(this.data); else this.data.fill(0);
      // Start at a rising zero crossing so periodic sounds stand still.
      const span = Math.floor(size / 2);
      let start = 0;
      for (let i = 1; i < size - span; i++) if (this.data[i - 1] < 0 && this.data[i] >= 0) { start = i; break; }
      for (let x = 0; x < width; x++) {
        const sample = this.data[start + Math.floor((x / width) * span)] ?? 0;
        points.push(height / 2 - Math.max(-1, Math.min(1, sample)) * (height / 2 - lineWidth * ratio));
      }
    } else {
      const size = (analyser?.size ?? 2048) / 2;
      if (!this.data || this.data.length !== size) this.data = new Float32Array(size);
      if (!silent && analyser) analyser.spectrum(this.data); else this.data.fill(-120);
      const rate = this.link.flows?.context?.sampleRate ?? 48000;
      for (let x = 0; x < width; x++) {
        // Logarithmic frequency axis, 30 Hz to 16 kHz; -100..-10 dB.
        const frequency = 30 * Math.pow(16000 / 30, x / width);
        const db = this.data[Math.min(size - 1, Math.round((frequency / (rate / 2)) * size))] ?? -120;
        const level = Math.max(0, Math.min(1, (db + 100) / 90));
        points.push(height - level * (height - lineWidth * ratio));
      }
    }
    context.beginPath();
    points.forEach((y, x) => (x ? context.lineTo(x, y) : context.moveTo(x, y)));
    if (this.colors.fill && !/rgba\(0, 0, 0, 0\)|transparent/.test(this.colors.fill)) {
      context.save();
      context.lineTo(width, this.mode === 'wave' ? height / 2 : height);
      context.lineTo(0, this.mode === 'wave' ? height / 2 : height);
      context.closePath();
      context.fillStyle = this.colors.fill;
      context.fill();
      context.restore();
      context.beginPath();
      points.forEach((y, x) => (x ? context.lineTo(x, y) : context.moveTo(x, y)));
    }
    context.lineWidth = lineWidth * ratio;
    context.lineJoin = 'round';
    context.strokeStyle = this.colors.line;
    if (!/rgba\(0, 0, 0, 0\)/.test(this.colors.glow) && !this.motion?.matches) { context.shadowColor = this.colors.glow; context.shadowBlur = 10 * ratio; }
    context.stroke();
    context.shadowBlur = 0;
  }
}

export function defineScope() { define('flows-scope', FlowsScopeElement); }
