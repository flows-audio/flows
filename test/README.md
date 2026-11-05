# Verification

Tests describe the behavior they check; they are not a guarantee for every
instrument, machine, operating system or DAW.

```sh
ctest --preset dev
ctest --preset app
ctest --preset web          # after emcmake cmake --preset web
npm run test:web --prefix ui
```

Build the corresponding preset first. The app suite also requires frontend
dependencies, JUCE and a graphical macOS session for native window checks.
The web preset runs the portable suites compiled to WebAssembly under Node;
`npm run test:web` drives the built site in Chromium, Firefox and WebKit.

| Area | Coverage |
| --- | --- |
| Compiler | Units, defaults, classes, policies, signal/analysis typing, diagnostics and budgets |
| Developer oscillators | Phasor, sin/cos, typed formula composition and observation |
| Engine/session | Voice identity, ADSR, mono/poly, stereo, live replacement, automation, replay and realtime heap guards |
| Audio boundaries | Onset/release discontinuities, retrigger, sample EOF and block-size invariance |
| Samples | Compiler, decoder, regions/reverse/loops, polyphonic playheads, assets and portable recall |
| Analyzer | FFT/mel/power/differences, continuity, projection, capture budgets, sustain/release and quiet drain |
| Sequencer | Timing, lane sources, levels/mutes, metronome, patterns, host transport and source failures |
| Documents/host | Unsaved live source, invalid-source silence, save/undo/recovery, multi-instrument focus and portable state |
| Completion | Incomplete source, aliases, ports/enums, custom classes, units, sample paths and UTF-16 ranges; offered calls compile with the real compiler |
| Native editor | WebKit layout, cursor/text/click geometry, completion, gestures, panels, splits, probes and assistant views |
| AU/VST3 | Real wrappers loaded in separate processes; MIDI offsets, stereo, tempo, automation and state |
| Assistant | Mock-provider compile/render/compare, proposal scope/staleness, cancellation, history and credential-store injection |
| Protocol | Every UI method is defined in the schema; a scripted session through the core validates params and results, and every core method is dispatched |
| Native/WebAssembly parity | The same scripted session returns identical responses from both cores; every audio case renders within 2 PCM16 steps (in practice identical) |
| Browser | Startup, compile, audio worklet output, browser file import, storage across reloads, single-tab handoff, device settings and the assistant's network bridge |

Seven instruments in `presets/` are reference examples. Internal regressions
live in `fixtures/`; nine event cases live in `cases/`. The hierarchical
[tutorial library](../examples/ADCxGather/README.md) adds playable feature studies.

## Reproducible audio cases

```sh
python3 test/tools/run_v1_suite.py --binary build/flows --output test/output/suite
python3 test/tools/validate_gather_examples.py --help
```

Output directories are generated and ignored. Case reports bind the source,
binary and event input hashes to sample rate, block size and observations.
Block-size comparisons and probe-on/off invariance help distinguish DSP
behavior from display or capture behavior. They do not replace listening.

## Performance scope

```sh
python3 test/tools/run_engine_benchmarks.py --binary build-app/engine_benchmark --output test/output/benchmark --build-flags='CMake Release'
```

The benchmark matrix is in `performance/profiles.json`. Read each budget result
and deadline-miss count, not just the command exit code. Offline engine timing
does not measure the full device callback, UI, host scheduling or hot-swap.
No full device/host performance acceptance is claimed for this preview.

The minimum macOS version is a build deployment target. Running on older macOS,
Windows/Linux desktop and additional DAWs requires its own verification.
Provider mocks do not establish behavior of a real paid model service.
