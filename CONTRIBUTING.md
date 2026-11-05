# Contributing

Flows explores how editable source and signal observation can help people
build and understand an instrument. Musical usefulness, clear language and
reliable audio behavior guide changes.

## Report a problem

Include OS/CPU, Flows version, standalone or plugin host, sample rate/block
size when relevant, exact reproduction steps and a small `.flows` source.
For sample problems, attach only audio you have permission to share. Remove
API keys and private paths from logs. A screenshot or short recording helps
with editor and signal-display issues.

## Propose a change

For a new language feature, architecture change or large UI redesign, open an
issue first with a musical example and expected behavior. Small bug fixes,
documentation corrections and example improvements can go directly to a PR.
Keep each PR focused and explain the problem, resulting behavior and checks.

Use plain English in code comments, documentation and interface text. Keep
compiler behavior, language reference, editor completion and examples aligned.
All controls must edit the same source; avoid hidden instrument state.

## Build and verify

Follow [BUILDING.md](BUILDING.md). Run the affected tests, then the relevant
core/app suite. Add a regression for a substantive bug; do not duplicate the
implementation in tests. UI changes need visual verification. Audio changes
need checks for event timing, release, finite output and block-size invariance
where applicable, plus listening when the sound changes.

Application behaviour lives in the portable core (`src/app`) and reaches the
UI through `protocol/flows-protocol.json`. Hosts (`src/host/app` for JUCE,
`src/web` and `ui/src/web` for the browser) implement platform services only.
When you add or change a request, edit the protocol and run
`npm run protocol --prefix ui`; when a host cannot support a feature, publish a
capability and have the UI explain it rather than checking which host runs.
See [docs/WEB.md](docs/WEB.md#keeping-the-hosts-together).

Disk, compilation, network and analysis work stay outside the audio callback.
Keep callback work bounded and avoid allocations, locks and blocking calls.
Preserve voice identity, automation timing and observable DSP values.

Never commit credentials, personal workspaces, build output or test recordings.
Contribute only code and assets you have the right to share under the applicable
project license. Third-party code needs its original license and attribution.

## Pull request checks

All PRs run the [CI checks](.github/CI.md). Wait for the **CI** check and the
latest CodeRabbit review, address actionable findings, and include relevant
local validation in the PR description. Native UI and listening checks remain
local responsibilities; a green hosted build does not replace them.

## Platform contributions

The desktop app currently targets macOS and the browser build runs the same
core as WebAssembly. Windows/Linux work should keep the core portable,
implement its ports (secrets, HTTP, audio decoding, parameters) in small
adapters, and include reproducible build instructions and platform test
results. A successful core build alone does not establish desktop or DAW
support.
