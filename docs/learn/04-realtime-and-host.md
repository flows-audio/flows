# 04 — Keep editing work away from the audio callback

The engine explains how to calculate sound. The host explains how that calculation keeps running while the user types, changes instruments, opens a view, or saves a DAW project.

The main files are [AppCore.hpp](../../src/app/AppCore.hpp), [AudioRuntime.cpp](../../src/app/AudioRuntime.cpp#L82), and [RealtimeSession.cpp](../../src/app/RealtimeSession.cpp#L7). `AppCore` owns documents and background work. `AudioRuntime` is the audio callback; JUCE's [`FlowsProcessor`](../../src/host/app/FlowsProcessor.cpp) and the browser's audio worklet both call it. `RealtimeSession` owns the currently playing program and the handover to its replacement.

## Three kinds of work

| Execution context | Main responsibility |
| --- | --- |
| JUCE message thread / WebView | User actions, document requests, UI state delivery |
| `FlowsProcessor::worker` | Compile, prepare engine storage, analyze captures, save recovery, retire old engines |
| Audio callback | Consume prepared work, process events and automation, render, publish bounded captures |

AI network requests have a separate `aiWorker_`. The compilation worker and message thread may use `mutex_`; the render callback does not lock that document mutex or ask the GUI for data.

The [worker loop](../../src/app/AppCoreWorker.cpp#L20) copies the desired source/revision under a short lock, compiles and prepares it outside that lock, then verifies that source, document, revision, and sample rate still match. A result that finished after a newer edit is discarded. The worker also consumes captured samples, produces telemetry roughly every 33 ms, checks workspace recovery every 500 ms, and reaps retired programs.

## A typed edit takes this path

```text
CodeMirror draft
→ document.apply(documentId, baseRevision, text, gestureId)
→ native document revision advances
→ compiler worker prepares a candidate
→ RealtimeSession::submit
→ next audio block installs that candidate
```

Each arrow has a concrete owner. UI draft handling is covered in chapter 06. The native receiver is [`request`](../../src/app/AppCore.cpp#L1176); preparation happens in [`worker`](../../src/app/AppCoreWorker.cpp#L20); transfer happens in [`submit`](../../src/app/RealtimeSession.cpp#L24); installation happens in [`install`](../../src/app/RealtimeSession.cpp#L101).

`PreparedProgram` contains a `Program`, fully constructed `Engine`, source text, structure identity, revisions, and initial runtime parameters. The audio callback never constructs the engine's vectors.

## Ownership crosses threads, destruction does not

`pending_` is an atomic pointer holding the newest prepared candidate. Publishing uses `exchange`: the worker can replace and delete an older candidate only if audio has not consumed it. Audio takes ownership by exchanging the pointer with `nullptr` at a block boundary.

An old active engine is sent through the `retired_` queue. [`reap`](../../src/app/RealtimeSession.cpp#L28) deletes it on the worker. If the retirement queue is full, installation waits. This is preferable to deallocating a large program from the audio callback.

[`SpscQueue`](../../src/app/RealtimeSession.hpp#L13) is a fixed array with two atomic counters. *SPSC* means one producer and one consumer: the producer writes a slot then publishes it; the consumer reads it then releases it. Acquire/release memory ordering makes the slot contents visible in the proper order. This queue is not a general multi-writer container.

Queues also carry probe selections, keyboard events, and capture frames. Capacity is explicit. A callback limits how many commands it drains, because a producer could keep filling a queue while the callback is reading it.

## Two different ways to install a program

### Same DSP structure: retain the playing engine

The worker's new program can describe different source text without requiring new DSP state. `install` compares `dsp_structure_key`, sample rate, and document identity. If these match, it swaps the existing engine into the new wrapper rather than restarting it.

Changed authored parameter values become `ParameterUpdate` commands. [`make_parameters`](../../src/engine/engine.cpp#L434) produces a smooth per-sample transition using the parameter's registered duration. A linear ramp interpolates values; a requested logarithmic ramp interpolates their logarithms when both endpoints are positive. The exact target is reached after the requested number of samples.

Observation metadata does not enter DSP structure identity. Adding a probe or changing the view can therefore preserve oscillator and envelope state. Host-owned parameter values and in-progress automation ramps are also preserved when the engine is retained; editing source does not silently reset them to an older worker snapshot.

### Changed DSP structure: prepare new state and crossfade

For a changed graph, the new engine has new state. `install` applies its valid runtime parameter overlay and replays accepted notes that are still held, in original press order. A new envelope starts Attack; this is not a transfer of its old stage or filter memory. Rejected notes do not automatically become accepted during replacement.

For the same document, the old and new engines overlap for 20 ms:

```text
output = old_output × old_weight + new_output × (1 − old_weight)
old_weight decreases linearly from 1 to 0
```

The outgoing engine receives no new note events during this fade. Its old release tails disappear with the fade. Afterward, the worker retires it. See the actual [stereo crossfade loop](../../src/app/RealtimeSession.cpp#L303).

This mechanism softens a valid structural edit; it does not guarantee that a large sound change will be inaudible. Compilation or preparation failure mutes the affected source and clears held notes. The editor preserves the invalid draft and displays diagnostics. Saving cannot make it playable.

### Switching to another instrument is a separate case

Tabs retain independent source and view state. [`activateLocked`](../../src/app/AppCore.cpp#L415) switches the cursor instrument and its audio identity. That session panics the old instrument, does not replay its keys into the new one, and does not crossfade its ungated output into the new instrument. A broken draft stays muted after tab activation or recovery, even when historical successful source exists.

The Step Sequencer can follow the cursor instrument or select another
`.flows` file or sample. An explicit choice uses a second `RealtimeSession`.
Its prepared program and fixed event/output buffers keep compilation, sample
decoding and allocation outside the audio callback. Pattern events go to this
session; keyboard/MIDI events go to the cursor session. Their stereo outputs
mix at the host. Switching editor focus leaves the pinned pattern running.
Probe Listen temporarily mutes its mix without stopping the pattern.

## Four version numbers answer four questions

| Field | Question it answers |
| --- | --- |
| Document/program `revision` | Which source snapshot does this information describe? |
| Program `generation` | Has the DSP engine state been replaced? |
| `parameterEpoch` | Have runtime parameter values changed? |
| `observationEpoch` | Has the probe, voice, projection, audition, or relevant transport context changed? |

They are carried into captures. A graph with the same structure can still have a new source revision; a parameter change can preserve oscillator state while invalidating a measurement window. One counter cannot express both facts safely.

## MIDI and the audio host

[`AudioRuntime::process`](../../src/app/AudioRuntime.cpp#L82) uses the internal BPM and advancing local position when the host passes no musical position: the standalone app and the browser. JUCE's `AudioProcessorPlayer` supplies a device-clock playhead in standalone, but its default `isPlaying = false` does not describe musical transport, so `FlowsProcessor` does not pass it on and the local pattern and transport LFO can advance.

AU/VST3 instances use the host's BPM, PPQ, and play state when available, with internal tempo and position as fallbacks for missing fields. A discontinuous reported host sample timeline requests panic so held notes do not survive an unrelated seek. The [standalone transport regression](../../test/cpp/host_tests.cpp#L283) supplies the same minimal device-clock playhead and checks audible pattern playback, stopping, and transport LFO movement.

MIDI note-on messages receive unique internal note IDs. Note-off matches the oldest held note with the same MIDI channel and pitch. That FIFO matching matters when a keyboard repeats the same pitch before releasing it. MIDI CC 120/123 requests panic. The current implementation handles note events; it is not a complete MPE or arbitrary MIDI modulation system.

Computer-keyboard events arrive through their own queue, tagged with the originating instrument identity. They enter at the next callback boundary. Events from a previously active instrument are dropped, so late key-up or pointer-up actions cannot release a different instrument's note.

Host events are sorted stably by sample time. A host block larger than 1024 frames is divided into prepared-size chunks, preserving absolute event positions. The engine can therefore use bounded internal storage without requiring the host to choose that block size.

## Automation is runtime data, not repeated source rewriting

The plugin exports 32 stable host slots, `macro01 … macro32`. A user binds a live source parameter to a slot. [`updateMacrosLocked`](../../src/app/AppCore.cpp#L986) publishes a revision-specific mapping from slot to graph parameter and range.

The host's normalized 0 … 1 value maps to a linear range, or to a logarithmic range with positive endpoints. The processor preserves VST3 parameter-point timing and AU scheduled ramps rather than collapsing a block into its final value. The [automation conversion](../../src/app/AudioRuntime.cpp#L325) creates timestamped `ParameterUpdate`s; the engine applies them in sample order.

Source text remains the authored instrument. Runtime automation can temporarily differ from those literals. The portable package and DAW checkpoint preserve runtime macro values separately. If a changed graph no longer proves a macro binding's identity, it is left unbound rather than controlling an unrelated node.

## Resource limits are part of behavior

| Resource | Current native limit |
| --- | --- |
| Poly voices / held note IDs | 32 voices, including release; 1024 held IDs |
| Events / automation points | Each bounded to 256 per host block |
| Prepared native graph | 2048 DSP nodes, 256 states, 4096 completion nodes, 2048 analysis nodes |
| Internal render chunk | 1024 samples |
| Captured roots / capture queue | 8 roots; 128 frames |
| Retired programs | 2 queue slots, plus active/outgoing and newest pending candidate |

The native preparation budget is smaller than the standalone compiler's graph limit. An accepted language program can therefore still produce a clear native preparation error if it is too large. These limits bound work; they are not a promise that every maximum-sized graph meets a device deadline.

Capture overflow drops a frame and increments a counter. Event/automation loss is more serious because a lost note-off could stick a note; those overflow paths request reset and expose errors. Neither path waits for the GUI.

## What the tests establish

[session_tests.cpp](../../test/cpp/session_tests.cpp#L63) compares live parameter edits with a reference engine, rejects stale candidates, checks exact 20 ms crossfade samples, replays held identities, tests stereo audition and projection, and checks allocation/deletion on exercised callback paths. [host_tests.cpp](../../test/cpp/host_tests.cpp) exercises document changes, serialization, and actual native metadata; [plugin_tests.cpp](../../test/cpp/plugin_tests.cpp) covers plugin integration where built.

See [test/README](../../test/README.md) for the measured performance scope. Functional correctness, heap behavior, and a recorded benchmark are separate pieces of evidence. None alone certifies hard realtime behavior on every system.

Next: [05 — Probe and analysis](05-probe-and-analysis.md).
