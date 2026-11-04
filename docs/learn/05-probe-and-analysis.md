# 05 — Measure the signal that actually ran

The purpose of a probe is to make a signal inspectable without constructing a second version of the instrument. The engine calculates a value once; the observation path copies that value before its storage is reused.

The main files are [RealtimeSession.hpp](../../src/app/RealtimeSession.hpp#L30), [Analyzer.hpp](../../src/app/Analyzer.hpp#L12), and [Analyzer.cpp](../../src/app/Analyzer.cpp#L15).

## Two graphs, with a one-way connection

The DSP graph produces sound. The analysis graph describes operations on captured samples:

```flows
# Excerpt inside an instrument; raw and filtered are existing audio signals.
p_raw = probe(raw)
p_filtered = probe(filtered)
removed = p_raw - p_filtered
spectral_change = spec(p_raw) - spec(p_filtered)
power_change = power(p_raw) - power(p_filtered)
```

The excerpt is not a complete instrument. `probe` refers to an existing DSP value. `spec`, `power`, and arithmetic after it create analysis nodes. Analysis results cannot feed back into the DSP output: these operations run later on a non-audio worker and are not sample-synchronous modulation sources.

A named DSP signal can already have a capsule and be inspected without an explicit `probe`. The explicit call gives the analysis language a value to compose. An alias does not duplicate DSP execution.

## Subscribe to inputs, not to every view

[`makeProbeSelection`](../../src/app/Analyzer.cpp#L200) walks selected analysis views back to their DSP roots. Two views depending on the same root share a capture channel. `spec(p)`, `power(p)`, and `p` consume one root, not three.

Current budgets are eight distinct roots, 128 analysis dependencies per view, 512 nodes across the selected views, and 32 spectral-transform nodes. The UI plans a selection within these limits; the host independently validates it. Unselected analysis does not consume capture channels. The Analyzer API can default to the first four authored views, but the application passes an explicit current selection.

[`Engine::set_capture_values`](../../src/engine/engine.cpp#L898) updates a preallocated tap mask. During render, the engine copies only subscribed values and audition taps. The graph still calculates authored named branches even if they do not reach the audio output, because they may be objects of investigation.

## What arrives in a capture frame

[`CaptureFrame`](../../src/app/RealtimeSession.hpp#L43) records:

- Source revision, engine generation, and parameter/observation epochs.
- Absolute sample interval, sample rate, and sequence number.
- DSP value IDs, selected voice, voice generations, and channel projection.
- Up to 1024 samples for each selected root.

The audio callback writes into a 128-frame queue. The worker reads it. If the queue is full, audio continues and a drop counter increases. The Analyzer then sees a sequence or timeline gap; it must not join samples from either side into a supposedly continuous FFT.

## Stereo and poly need an explicit interpretation

[`capture_stereo`](../../src/app/RealtimeSession.cpp#L386) projects audio into one chosen analysis channel: Mid `(L+R)/2`, Left, Right, or Side `(L−R)/2`. In mixed-voice mode, these audio values are summed across voices. Analysis of `power(probe(audio))` therefore measures the power of that projected sum, including interference between voices. It is not the sum of separate per-voice powers.

Mono control signals such as gate and envelope keep their original values even when audio projection is Side. Adding all gates or envelopes would not describe one voice's behavior. The mixed control preview uses the last rendered voice's value and labels it `last_voice_control`. This is a rendering selection, not a guarantee that it belongs to the newest key press.

For arithmetic or transforms involving a poly control probe, choose a specific voice slot. The Analyzer rejects the ambiguous mixed-control case. A slot can later host a different note, so the slot's generation also participates in continuity checks.

## A continuous window is evidence, not decoration

[`Analyzer::Impl::consume`](../../src/app/Analyzer.cpp#L292) maintains a circular history of 4096 samples per root. At 48 kHz this is about 85.3 ms; at 44.1 kHz it is about 92.9 ms. This is the analysis-window length, not total input-to-display latency.

History resets after a missing packet, sample gap, source/engine change, parameter epoch, observation change, selected-voice replacement, or transition frame. Invalid/nonfinite captures are rejected. A mixed-control voice change resets that control channel without discarding independent mixed-audio history.

Raw waveform previews appear from the first valid continuous block. Arithmetic between matching short waveform windows can already be displayed. `power`, FFT, and mel spectrum wait for 4096 samples. The system does not silently stitch short notes from different selected voices into one long measurement.

This explains a practical demo behavior: a held note gives a stable spectral window quickly; repeatedly changing a parameter can keep restarting the analysis window. Stop dragging briefly before interpreting a settled spectrum. The exact visual refresh also depends on callback size, worker scheduling, and display delivery.

## Waveform, RMS, peak, and display reduction

[`display`](../../src/app/Analyzer.cpp#L134) calculates RMS and peak from the currently available waveform samples:

```text
mean square = Σ x[n]² / N
RMS         = sqrt(mean square)
peak        = max |x[n]|
```

It then reduces the waveform to at most 192 display points by retaining the minimum and maximum in each interval, in chronological order. That is more useful for short transients than simply keeping every twentieth sample. RMS and peak are calculated before this display reduction.

The musical display has two additional presentations. **Detail** uses a fixed span of up to 1024 samples, aligned to an interpolated rising zero crossing when possible. **1 Beat** accumulates every captured sample into 256 min/max bins using transport PPQ. Current-beat data is bright, previous-beat data is dim, and missing bins are blank. Direct probes share this beat envelope; derived analysis currently uses Detail. These presentations never become operands for FFT, differences or power.

The spectrum renderer uses a logarithmic frequency axis and a fixed dB range, with separate attack, release and peak decay. Those curves are display smoothing only. Spectral subtraction stays signed and linear, rather than losing its sign in a logarithm. See [signalPlot.ts](../../ui/src/signalPlot.ts) for timing and fallback behavior.

The standalone `power(p)` analysis operator uses the complete 4096-sample window and returns mean square, without a Hann window. For a steady sine of amplitude `A`, its long-window mean square approaches `A²/2`.

## FFT: what the spectrum numbers mean

[`fft`](../../src/app/Analyzer.cpp#L104) is an iterative radix-2 FFT: bit-reorder the 4096 values, then combine pairs in stages of size 2, 4, 8, and so on. Before transforming, it applies the symmetric Hann window:

```text
w[n] = 0.5 − 0.5 cos(2πn/(N−1)),   N = 4096
X    = FFT(x × w)
```

Windowing reduces leakage caused by the window's endpoints. It also changes magnitude, so [`snapshot`](../../src/app/Analyzer.cpp#L308) normalizes the result. `spec`/`spectrum` returns a one-sided **linear amplitude spectrum**:

```text
A[k] = |X[k]| × factor / ((N−1)/2)
factor = 1 for DC and Nyquist, otherwise 2
frequency[k] = k × Fs / N
```

At 48 kHz the bin spacing is 11.71875 Hz. This is not arbitrary-resolution frequency measurement, and it is not dB. A tone between bins spreads over nearby bins. The display keeps at most 512 representatives, choosing the largest absolute value in each bucket and its frequency; the underlying calculation uses 2049 nonnegative-frequency bins.

Direct `probe(audio)` views reuse their captured root's preview spectrum. Selected explicit spectral operations also cache transforms of a shared analysis input within one snapshot.

## Mel spectrum: a different question

[`mel_spectrum`](../../src/app/Analyzer.cpp#L313) divides frequency into 64 triangular bands. Band edges are equally spaced on the mel scale:

```text
mel(f) = 2595 log10(1 + f/700)
```

The implementation converts those edges back to hertz, weights each FFT bin by its triangle, and sums one-sided squared magnitudes. Its normalization is `N(N−1) × 3/8`, corresponding to the Hann window's energy normalization in this implementation.

The result is **linear weighted band power**. It is not a spectrogram history, not decibels, and not a psychoacoustic loudness score. `mel_spec(after) - mel_spec(before)` answers where power changed across broad bands.

The current compiler preserves the input `Unit` label on `power` and `mel_spec`; it does not model a new squared physical dimension. Treat the mathematical quantity described here as authoritative. Do not present the unit label as a complete dimensional-analysis system.

## Three differences that answer different questions

| Expression | Meaning |
| --- | --- |
| `p_a - p_b` | Signed, sample-aligned residual waveform; includes phase differences |
| `spec(p_a) - spec(p_b)` | Difference of amplitude magnitudes at corresponding frequencies |
| `power(p_a) - power(p_b)` | Difference between two mean-square values |

These are not interchangeable. In particular, `power(p_a - p_b)` also includes the correlation between the two waveforms and is generally different from `power(p_a) - power(p_b)`. Likewise, a magnitude-spectrum difference is not the spectrum of a waveform difference.

The [analysis evaluator](../../src/app/Analyzer.cpp#L330) checks matching window shapes and frequency axes. Scalars can broadcast. Divide-by-zero and nonfinite results report errors instead of displaying a plausible-looking number. Source-level unit rules still apply before evaluation.

## Listen and Hold have narrow meanings

Listen switches monitoring from the final instrument to a named audio signal, or its direct audio probe, with a 10 ms transition. The signal can be mixed across voices or selected by slot. It retains the actual left/right channels: choosing Side for a chart does **not** make Listen audition Side. Derived analysis and control values cannot be auditioned through this path. See [audition mixing](../../src/app/RealtimeSession.cpp#L330).

[`Observation`](../../ui/src/observation.ts#L1) can retain a valid display while new data is pending. The UI labels held data; it does not feed that old display back into analysis. User Hold freezes the measured display, not the sound engine. These distinctions prevent a stable-looking old chart from being mistaken for a new measurement.

## A useful experiment

Open an example with a raw oscillator and a filtered version. Observe both, hold one note, and compare waveforms. Then inspect their spectra and power difference. Change cutoff, pause briefly, and inspect the new window. Finally compare `power(p_raw - p_filtered)` with `power(p_raw) - power(p_filtered)`.

The important question is not whether a chart looks impressive. It is whether the code makes the experiment precise enough to repeat and explain.

[analyzer_tests.cpp](../../test/cpp/analyzer_tests.cpp#L39) checks known-tone FFT magnitude/frequency, power and mel differences, gaps and epochs, selection budgets, partial windows, and selected-voice restrictions. Its [short poly burst test](../../test/cpp/analyzer_tests.cpp#L111) checks that unrelated control voice churn does not erase mixed-audio history.

Next: [06 — Editor, workspace, and AI](06-editor-workspace-and-ai.md).
