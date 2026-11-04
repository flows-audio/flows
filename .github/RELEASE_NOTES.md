Code is the instrument: build sounds in source, then listen, inspect and
compare each stage through Signal Flow and probes.

## Download

- **Flows-1.1.4-macOS-arm64.zip** includes the standalone app, AU/VST3 plugins,
  licenses, Flows source and the pinned JUCE source. Apple Silicon only.
- Extract the ZIP, move **Flows.app** to Applications, open an example and
  press **Cmd+K** to play. Z/X changes octave.
- The app is ad-hoc signed, **not notarized**. macOS may require approval in
  Privacy & Security. AU/VST3 host compatibility remains experimental.
- AI is optional: configure your own endpoint, model and key to use it.
  No keys or personal workspace are included.
- **Flows-1.1.4-web.zip** is the browser build as a static site, with licenses
  and a source link. Host it where you can send the two cross-origin isolation
  headers in its `_headers` file; see `docs/WEB.md`.

## Included

Poly/mono synthesis, ADSR, LFO, stereo unison, filters, distortion, formula
oscillators and samples; a per-lane step sequencer; waveform/spectrum probes
and differences; source-linked controls, completion, split editors and an
optional global assistant.

Valid unsaved source drives the instrument; compile errors mute the affected
instrument. Save only stores the source.

## Verification

43 native/editor/plugin checks and 18 independent core checks passed locally.
Nine audio scenarios and 21 study instruments passed compile/render checks.
The release workflow builds and tests the downloadable bundles in a fresh
macOS environment before packaging and uploading them. Signatures and
checksums are verified. Windows/Linux desktop support and broad DAW/device
compatibility are not yet established.

## Source and license

[Build instructions](https://github.com/flows-audio/flows/blob/v1.1.4/BUILDING.md)
· [Language guide](https://github.com/flows-audio/flows/blob/v1.1.4/docs/LANGUAGE.md)

The complete application is **AGPL-3.0-only**; the independent compiler and
DSP directories are **Apache-2.0**. Third-party licenses are retained.
Source archives and SHA256SUMS are attached below.
