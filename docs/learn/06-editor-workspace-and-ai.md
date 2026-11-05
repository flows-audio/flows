# 06 — One source, several ways to work with it

Flows has a text editor, numeric gestures, dropdown choices, and Signal Flow controls. They all edit the same instrument source. There is no separate editable patch hidden behind the text.

The browser engine renders the interface; the core owns documents and sound, whether it runs in the macOS app or compiled to WebAssembly in a browser. Start with [FlowsEditor.cpp](../../src/host/app/FlowsEditor.cpp#L33), [App.tsx](../../ui/src/App.tsx#L38), and [documents.ts](../../ui/src/documents.ts#L9).

## Native shell, local browser interface

`FlowsEditor` is a JUCE `AudioProcessorEditor` containing a `WebBrowserComponent`. It serves every file of the embedded editor build through a local resource provider. On macOS the native browser backend is WKWebView. Navigation is limited to the local resource root and `about:blank`. Explicit Markdown links ask the native editor to open the default browser.

React owns the layout, CodeMirror owns each text view, and canvas code draws measured signals. [`client.ts`](../../ui/src/client.ts) sends every request through a transport. [`transports/juce.ts`](../../ui/src/transports/juce.ts) sends a `flowsRequest` through JUCE's native bridge: a request carries an ID, method, and parameters; a promise resolves with the result or rejects with its error. In the browser build, [`web/host.ts`](../../ui/src/web/host.ts) calls the WebAssembly core in the page instead. Methods, parameters and results are defined in [`protocol/flows-protocol.json`](../../protocol/flows-protocol.json). Opening the UI in an ordinary development server without a host does not create an audio engine: requests report that no host is connected.

[`FlowsEditor::timerCallback`](../../src/host/app/FlowsEditor.cpp#L53) sends telemetry and agent events at up to 30 Hz while visible, and every sixth tick sends the snapshot slices that changed since its previous state event. Audio runs independently of this timer. Hiding the editor does not stop the instrument.

## Documents have identity and revision

The native [`Document`](../../src/app/AppCore.hpp#L77) stores path, text, last saved text, revision, undo/redo history, and current gesture. `OpenDocument` additionally keeps compile metadata, macro mappings, last playable source, and observation selection.

The UI's [`DocumentSession`](../../ui/src/documents.ts#L9) keeps local drafts so typing is immediate. Its serialized queue handles both source changes and instrument activation. Before switching instruments, it flushes the outgoing draft and waits for acknowledgment. Typing that arrives during an activation remains attached to its document ID.

Native `document.apply` requires the matching base revision. If another operation has changed the source, the request fails and the local draft remains available. The user can reload native source or explicitly apply the draft. A delayed network or compile result does not silently win over newer typing.

`App` batches ordinary typing for roughly 80 ms and gesture updates for 25 ms before flushing. These are UI scheduling choices, not audio timing guarantees. See [`changeDocument`](../../ui/src/App.tsx#L167).

## Source locations are versioned data

The compiler records UTF-8 byte spans. JavaScript strings and CodeMirror use UTF-16 offsets. The host converts spans against the exact source snapshot when it publishes diagnostics, parameters, choices, and signals. This matters when comments contain non-ASCII text or emoji.

The editor only enables span-based edits when displayed text and metadata describe the same revision. A pending draft removes stale inline capsules and numeric/choice decorations; moving old widgets through edited text can put them inside newly typed expressions. The last valid flow can remain visible with controls disabled. See [decoration building](../../ui/src/Editor.tsx#L110) and the [metadata guard](../../ui/src/App.tsx#L383).

Completion works while source is incomplete. [languageCatalog.ts](../../ui/src/languageCatalog.ts) describes the supported built-ins, ports and enum values; [completionModel.ts](../../ui/src/completionModel.ts) scans current source to resolve aliases, local instances, key fields and typed class ports. [flowsCompletion.ts](../../ui/src/flowsCompletion.ts) adapts these results to CodeMirror. `key.` opens member choices; calls offer unused parameter names and parameter descriptions; sample file arguments offer imported audio paths. Tab/Enter accept, Escape closes, and Ctrl+Space explicitly opens choices. Perform mode closes completion and locks text input.

The editor explicitly remeasures after font preferences, font loading, inline widget rebuilding and size changes. Source ligatures are disabled. The real WebKit window test compares DOM text positions with CodeMirror cursor/click coordinates after changing fonts and sizes, then accepts a completion and verifies the exact resulting source.

Syntax highlighting is a lightweight CodeMirror token stream, not a second compiler. The compiler supplies semantic facts: which assignment is audio/control/analysis, which literal is an adjustable parameter, and which symbol is an enum option. A word receiving a color does not prove that a program is valid.

## Drag a number, then follow the edit

[`Editor`'s pointer handler](../../ui/src/Editor.tsx#L192) finds the compiler-provided parameter span. It remembers the original source and a unique gesture ID, then replaces only the numeric literal as the pointer moves. Unit suffixes remain in the source. A popup shows range, current value, and live/rebuild status. Shift reduces movement for fine adjustment.

Registered range, step, and scale determine the gesture. Without a range, movement is relative to the current magnitude. A fixed range is read-only. Double-click can restore a registered default.

Each move travels through the normal draft/compile path. Native [`applyLocked`](../../src/app/AppCore.cpp#L630) saves one starting text for the gesture, so Undo restores the whole drag rather than each intermediate value. Escape cancels the gesture and restores that starting text. Undo is routed to native history; CodeMirror does not maintain a competing source history.

## Choices are compiler-owned too

`ChoiceDescriptor` contains the selected value, allowed options, disabled reasons, source span, owner, and insertion text for an omitted default. The host exposes this with each compile snapshot. The interface does not guess choices from a word such as `mode`.

[`applyChoice`](../../ui/src/choice.ts#L5) checks that the option is enabled, the span is valid, and the source still contains the expected explicit value. An implicit choice uses an empty insertion span plus compiler-provided prefix/suffix text. Selecting the current value does nothing.

The editor decorates explicit values with a small underline. Click or Alt+Enter opens [`ChoiceMenu`](../../ui/src/ChoiceMenu.tsx); arrows, typeahead, Enter, and Escape operate it. Signal Flow also displays implicit defaults, making an omitted parameter discoverable without adding text until needed. Changing a choice creates one undoable source edit.

Some choices depend on existing inputs. For example, retrigger LFO mode needs a trigger; a menu can explain why that option is unavailable. A dropdown is not permission to invent missing signal connections. Numeric values such as unison count continue to use numeric gestures.

## Signal Flow is a view of the compiler graph

[`DataflowGraph.hpp`](../../src/app/DataflowGraph.hpp) builds the display graph from compiled values and authored names. [`flowLayout.ts`](../../ui/src/flowLayout.ts) positions nodes and routes their display connections. [`Dataflow.tsx`](../../ui/src/Dataflow.tsx#L240) provides pan, zoom, Fit, selection, and source reveal.

The topology is read-only. Selecting a node highlights its signal path and can reveal its source. Turning a knob or choosing an option edits source, just like doing it in CodeMirror. There is no second graph to reconcile after a manual code edit. When compilation is pending or failed, the last valid graph can remain as a reference with controls disabled.

A capsule can be dragged into the observation area to prioritize that signal. It is not a module instance dragged into DSP. Cards, capsules, Inspector, and Signal Flow share the observation plan and its eight-root budget. [probeSelection.ts](../../ui/src/probeSelection.ts) plans that selection; the native host remains the final validator.

## Splits show several instruments; focus chooses one

`App` maintains two editor groups with their own tabs. The same instrument may appear in both. Focusing another instrument activates it for keyboard performance and Signal Flow. AI can work across instruments independently of focus. Inactive tabs keep source and metadata but do not run extra DSP engines.

Closing one of two views of the same document does not discard the document. Closing the final view checks whether it is dirty; Save, Discard, and Cancel have explicit meanings. Switching documents clears old audio observation data so a delayed frame cannot appear under a new source.

Resizable dividers, visibility controls, keyboard shortcuts, theme colors, font size, and transparency live in the UI rather than the language. [ResizeHandle.tsx](../../ui/src/ResizeHandle.tsx), [panelVisibility.ts](../../ui/src/panelVisibility.ts), and [style.css](../../ui/src/style.css) are the relevant entry points. The default code font is 16 px, adjustable from 10 to 32 px. macOS standalone supports background transparency; plugin windows remain opaque.

## The workspace is ordinary files plus recovery state

[`configureWorkspace`](../../src/app/AppCore.cpp#L279) uses the folder its host passes in. The macOS app passes `FLOWS_WORKSPACE` if set, otherwise JUCE's user application-data directory plus `Flows/Workspace` (normally `~/Library/Flows/Workspace`). The browser build passes a folder in Emscripten's file system that is saved to the browser's IndexedDB for this site, or kept in memory for the tab when the browser blocks storage; there is no folder on disk to open. The application recursively discovers `.flows` files. The demonstration library is a hierarchy of these ordinary source files, not a separate preset format or runtime module database.

| File/state | Purpose |
| --- | --- |
| `.flows` | Saved instrument source |
| `.flows-settings.json` | Appearance/tempo preferences, AI endpoint and model |
| `.assistant-history.json` | Local assistant conversations, messages, archive and Trash state |
| `.sequencers/<instrument>.json` | Saved pattern, sound selection and captured playable source |
| `.session.flowsinst` | Standalone recovery for open documents, drafts, saved text, last playable source, macros, active document |
| Exported `.flowsinst` | Portable package for one instrument |

Sequencer source selection does not open another editor. Its owner retains the
pattern while another instrument has keyboard focus. Instrument packages and
workspace recovery include captured sound source and required sample bytes, so
recall does not require the original selected file.

[`packageLocked`](../../src/app/AppCore.cpp#L650) serializes source, last playable source, preferences, and the 32-slot macro registry/runtime values. It does not save instantaneous phase, filter memory, or envelope stage. Import uses a local filename rather than depending on the original absolute path. There are no arbitrary filesystem module imports or asset dependency locks yet.

[`recoveryLocked`](../../src/app/AppCore.cpp#L692) covers up to 32 open documents. Recovery distinguishes a saved file from an unsaved draft. Clean recovered documents can reload newer disk text; dirty drafts are preserved. Undo history is process-local and is not reconstructed after restart. Opening an already open document uses its in-memory state; the recursive file inventory is not a general external-edit merge service.

DAW save reads an immutable, prebuilt source/package snapshot and adds the current bounded macro values. Restore is handed to the worker for validation and compilation. This lets a plugin keep sounding without an open editor. It does not mean that arbitrary serialization is part of the render loop.

## AI searches the workspace and uses the same engine

[`AssistantHistory`](../../src/app/AssistantHistory.cpp) saves conversations separately from instruments. Each message records its request ID and original document/revision so a reply goes back to the correct conversation even after the interface switches views. Rename, archive, Trash and restore all use native requests. A temporary-file replacement and inter-process lock prevent partial or overlapping writes. Interrupted requests recover explicitly.

A follow-up uses up to 16 recent successful turns within a 64 KiB context budget. An instrument can be attached as an optional reference; otherwise the assistant starts from the prompt and searches for relevant files. Stored conversations hold readable responses and proposed source, while the runtime validates whether a proposal can still apply. A restart does not restore old proposal permissions. [`Markdown.tsx`](../../ui/src/Markdown.tsx) renders replies as Markdown, including copyable code, without executing returned HTML.


[`AIService.cpp`](../../src/app/AIService.cpp#L266) exposes seven tools:

| Tool | What it actually does |
| --- | --- |
| `search_instruments` | Searches visible workspace instruments by path or source, with bounded pages |
| `read_source` | Reads a named instrument, preferring its open draft; returns a source digest |
| `explain_signal_path` | Compiles and traverses a bounded typed dependency graph |
| `compile_snapshot` | Returns compiler results; no audio render |
| `render_case` | Runs the real engine offline and returns measured metrics |
| `compare_observations` | Renders matching event timelines and compares signals or source variants |
| `propose_source_patch` | Compiles a complete replacement source and returns a proposal |

Sending a prompt does not require an open instrument. The model can search names or source text, then read the identified files. Hidden files and paths outside the workspace are excluded. Source and tool results are treated as data. [`AIModels`](../../src/app/AIModels.cpp) saves multiple global profiles in the host's settings folder (`~/Library/Flows` in the macOS app); [`AISecrets.mm`](../../src/host/app/AISecrets.mm) stores their keys in macOS Keychain. The browser build keeps keys in memory only: closing or reloading the tab forgets them, and they must be entered again. The bottom selector chooses a saved profile across conversations. Keys never enter source packages or model context.

The [network loop](../../src/app/AIService.cpp#L765) allows at most six model/tool rounds and twelve tool calls, with request/response limits, timeout, and cancellation. Endpoints must use HTTPS or strict loopback HTTP. Networking happens on the AI worker, never the audio callback.

### What a render proves

[`render`](../../src/app/AIService.cpp#L169) compiles a private source snapshot and runs a separate engine at 48 kHz in 256-sample blocks. Its budget is at most three seconds of audio, 256 graph nodes, 32 states, eight voices, eight observations, and 64 explicit note events. It uses an explicit BPM and a PPQ timeline starting at zero, not the currently playing live transport.

Output metrics include both channels for stereo output. Named observed signals use voice slot 0; stereo observations are Mid. `compare_observations` reports sample-aligned residual RMS/peak, mean-power difference, and cosine similarity. These are numerical measurements. They are not listening, a perceptual quality score, or a guarantee that a sound is musically useful.

### A proposal does not apply itself

The service validates proposed source and records its target path plus the source read before the edit. Existing edits must supply that read’s digest. `ai.apply` checks the target’s current source and the exact proposal text before entering the same `applyLocked` source/undo path used by a user edit. Opening the target for review does not invalidate it; changing its source does. New instruments can be saved in AI-gen even when no instrument is open. A stale proposal cannot overwrite a newer draft. Compilation success validates language semantics, not every possible runtime event sequence or the user's musical intent.

This gives AI a useful role in the instrument's central workflow: formulate a change, inspect the signal path, run a bounded comparison, and present readable code. The creator can inspect and revise the same code and observations.

## Where to verify behavior

- [documents.test.mjs](../../ui/test/documents.test.mjs) exercises draft acknowledgment, activation, and conflict handling.
- [choice.test.mjs](../../ui/test/choice.test.mjs) checks exact source replacements, stale ranges, and disabled choices.
- [window_tests.mm](../../test/cpp/window_tests.mm) drives the real native browser/editor, including gestures, splits, visibility, and choices.
- [host_tests.cpp](../../test/cpp/host_tests.cpp) checks native requests, recovery, metadata, AI tools, and the local fake-endpoint workflow.

The local fake endpoint verifies protocol and tool execution without relying on an external model. It is not evidence that a particular live provider, model, or network connection will behave identically. For recording, treat a live AI request as optional unless that exact setup has been rehearsed.

You can now trace a complete action: a source edit becomes a versioned document, a prepared graph, audio output, a bounded capture, and an inspectable result. The core design keeps those stages connected without making the source depend on the interface.
