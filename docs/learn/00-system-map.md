# 00. The system in one pass

Flows is a synthesizer whose instrument definition is text. The desktop window edits that text, a compiler checks its meaning, and an audio engine executes the resulting signal graph. A separate observation path shows what the engine actually computed.

Keep three objects separate. **Source** is the `.flows` text you save. A **Program** is the compiler's description of operations and their connections. **Runtime state** is the changing memory that makes an oscillator keep its phase or an envelope remember its current level. Editing text does not mean that every edit must discard that memory.

## The smallest audible program

This is the bundled [sine-reference.flows](../../test/presets/sine-reference.flows):

```flows
import standard as std
from lib.osc import ClassicSine

Inst.sigstream(std.event.keys.poly()) as key:
    mOsc1 = ClassicSine.make()

    raw = mOsc1(pitch: key.pitch, unison: 1, level: -6db)
    return raw * key.gate * key.velocity
```

The imports name built-in functionality. They do not load Python libraries from disk. `poly()` chooses how key events allocate voices. A voice is one running copy of the instrument's state and its current key controls. Two overlapping notes can therefore have independent oscillator phase.

`make()` declares the oscillator instance. Calling `mOsc1` connects its inputs and produces `raw`. In the sample loop, `raw` represents the oscillator's current output. Multiplication by `key.gate` silences it when the key is released. Multiplication by velocity scales its amplitude. This deliberately small example has a hard gate, so it is useful for reading but not a model of click-free articulation. The envelope examples provide a shaped attack and release.

The source contains no buffer size. The host can ask for one sample or a block without changing what the instrument means. That is the hardware-independent part of this language. The implementation still has to manage buffers, event offsets and sample rate correctly.

## Where the implementation lives

| Files | Job | Read first |
| --- | --- | --- |
| [program.hpp](../../src/compiler/program.hpp), [program.cpp](../../src/compiler/program.cpp) | Turn text into a checked `Program` | `Program`, `compile`, `Parser` |
| [engine.hpp](../../src/engine/engine.hpp), [engine.cpp](../../src/engine/engine.cpp) | Execute nodes and maintain voice state | `Engine`, its preparation and rendering methods |
| [RealtimeSession.hpp](../../src/app/RealtimeSession.hpp), [RealtimeSession.cpp](../../src/app/RealtimeSession.cpp) | Transfer events, parameters, prepared programs and capture data across the audio boundary | `RealtimeSession` |
| [AppCore.cpp](../../src/app/AppCore.cpp), [AppCoreWorker.cpp](../../src/app/AppCoreWorker.cpp) | Own documents and background work, publish UI state | `request`, `snapshot`, `worker` |
| [AudioRuntime.cpp](../../src/app/AudioRuntime.cpp), [FlowsProcessor.cpp](../../src/host/app/FlowsProcessor.cpp) | Run the audio callback; adapt JUCE audio, MIDI and automation | `AudioRuntime::process`, `processBlock` |
| [Analyzer.cpp](../../src/app/Analyzer.cpp) | Analyze captured samples away from the audio callback | `Analyzer` |
| [flows-protocol.json](../../protocol/flows-protocol.json), [client.ts](../../ui/src/client.ts) | Define and carry every request, event and host capability | `methods`, `request`, `subscribe` |
| [FlowsEditor.cpp](../../src/host/app/FlowsEditor.cpp#L33), [WebHost.cpp](../../src/web/WebHost.cpp) | Connect the editor to the core: the native WebView bridge, or WebAssembly in the browser | `flowsRequest`, `flows_request`, `flowsState` |
| [App.tsx](../../ui/src/App.tsx), [Editor.tsx](../../ui/src/Editor.tsx) | Arrange the workspace and edit instrument text | document activation, editor transactions |
| [Dataflow.tsx](../../ui/src/Dataflow.tsx), [Signals.tsx](../../ui/src/Signals.tsx) | Display graph nodes and measured signals | node selection, Inspector, canvas drawing |
| [AIService.cpp](../../src/app/AIService.cpp) | Run bounded model requests and instrument tools | tool dispatch and source proposals |
| [offline_main.cpp](../../src/host/offline_main.cpp) | Run the same compiler and engine without the GUI | `main`, `render` |

JUCE supplies native audio devices, MIDI and plugin wrappers. It does not define the Flows language. React arranges the interface. CodeMirror manages the text editor. Neither produces audio samples.

## What happens when you play a note

1. The host receives a note event with an offset in the current audio block. Computer keys and on-screen keys enter through the application's event path.
2. The session passes accepted events to the active engine. The engine applies the mono or poly policy and associates the press with a note identity.
3. For every sample, each active voice evaluates the prepared nodes in dependency order. An oscillator reads pitch, advances phase and produces a sample. Arithmetic connects that result to the output.
4. The engine mixes voices into left and right audio. A mono signal has the same value in both output channels.
5. Selected observation roots copy their existing samples into bounded capture storage. The analysis worker reads those samples later. Opening an Inspector does not create another oscillator or voice.

On release, the engine does not simply wait for a sample equal to zero. A sine wave crosses zero while it is playing. Instead, the compiler supplies conditions that describe when the output can finish, such as a closed gate or an idle envelope, including relevant effect tails.

## What happens when you change a number

The editor sends the document ID, its expected revision and the new text. A revision is an increasing version number. The native document service checks that the edit belongs to the current version and records undo history. The compiler checks the new source on a background worker.

For a compatible parameter change, the session updates parameter values while keeping the running state. Registered smoothing can soften the transition. A structural change prepares a new engine and installs it at an audio block boundary. When replacing an instrument within the same document, the session can replay held notes and crossfade the old and new sound. An envelope can therefore restart for structural edits. The source-level appearance of an edit does not determine its runtime cost.

If the source is invalid, the editor keeps the draft, shows the diagnostic and mutes that instrument. Valid unsaved edits drive audio; Save only stores the source. The interface disables controls whose source positions no longer match the draft.

## What happens when you open a probe

The UI asks to observe a named signal or an analysis expression. The host identifies the required DSP roots, checks the capture and analysis budgets, and updates the subscription. The engine copies selected samples without rerunning the graph. The analyzer checks their identity and continuity before forming a window.

Raw waveforms can appear before a full analysis window is available. FFT and power need 4096 continuous samples. At 48 kHz that is about 85 ms of signal, plus scheduling and display delay. Edits, gaps or a changed voice can reset that window. A held display is useful, but must not be mistaken for a new measurement.

## A useful reading habit

When a file feels too large, follow one value rather than reading every function in order. Start with `key.pitch`, then the oscillator's node, then its runtime state, then a captured `raw` sample. For a UI question, start with the visible gesture, find the native request, and follow the returned revision. The later chapters use these paths to explain the details.
