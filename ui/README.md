# Editor and host bridge

The React/CodeMirror UI edits `.flows` source. Native compiler metadata supplies
signal spans, graph nodes, parameters, choices and diagnostics. DSP remains in
the C++ engine; the browser renders captured measurements rather than a second
simulation of the instrument.

```sh
npm ci
npm run build        # the editor embedded in the macOS app
npm run build:web    # the browser site (needs the WebAssembly core; see docs/WEB.md)
npm run dev:web      # browser editor with hot reload against build-web/web
npm run test:web     # Playwright: Chromium, Firefox and WebKit
```

`npm run dev` previews layout without a host. Audio, files, credentials and AI
need a host: the macOS app or the browser build.

## Interaction

- Each editor group owns tabs, selection and scroll; focus chooses the playing
  instrument and Signal Flow. AI is workspace-wide with optional references.
- Cmd+K switches Edit/Perform. Perform locks source typing while keeping
  parameter drags and option choices available. Computer keys play notes.
- Completion resolves incomplete source, key aliases, built-in/class ports,
  enums, imports, units and sample paths. Types/defaults accompany suggestions.
- Numeric controls share source ownership, musical response, Shift adjustment,
  exact entry and one undo per gesture. Stale source spans cannot edit a draft.
- Invalid source keeps diagnostics and mutes the instrument. Valid unsaved
  source drives audio; Save persists it.
- Probes, capsules and Signal Flow use measured capture. Audio defaults to a
  spectrum; expanded views stack waveform above spectrum. Sustain/release stay
  live, quiet previews clear, and Hold explicitly freezes observation.
- Sequencer lanes select their own instrument/sample, level and mute. Running
  transport enables continuous beat-aligned observation even when lanes are mute.

## Host contract

[`protocol/flows-protocol.json`](../protocol/flows-protocol.json) defines every
request (owner, params, result) and event; `src/protocol.gen.ts` is generated
from it with `npm run protocol` and the wire types in `types.ts` re-export it.
Components call `request()` and `subscribe()` from `client.ts`, which carries
them over a transport: `transports/juce.ts` (the WebView bridge:
`flowsRequest` receives `{id, method, params}` and returns
`{id, ok, result, error}`) or `web/host.ts` (the WebAssembly core in the page).
`flowsState` events contain only changed top-level snapshot slices; the client
assembles full snapshots. `flowsTelemetry` carries observations and
`flowsAgent` assistant progress. UI features branch on
`snapshot.capabilities` (for example `audioSettings`, `fileDialogs`,
`secrets`), never on the transport. The native WebView serves only the
embedded build and does not expose privileged integration to arbitrary websites.

`DocumentSession` serializes source edits and activation. Mutations carry
document ID and expected revision; a conflict preserves the user's draft.
Compiler UTF-8 source spans are converted to UTF-16 positions against that
same revision. Numeric gesture IDs/commit/cancel phases preserve undo ownership.

See [the editor implementation guide](../docs/learn/06-editor-workspace-and-ai.md)
and [the architecture](../docs/ARCHITECTURE.md) for implementation paths.
