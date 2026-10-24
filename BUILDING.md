# Building and packaging

The [README](README.md#build-from-source) contains the shortest build commands.
`VERSION` is the release version; CMake and the packaging tool read it. The UI
package version must match it.

## Dependencies

| Component | Requirement |
| --- | --- |
| Compiler/DSP/CLI | C++20, CMake 3.25+, Ninja, Python 3.9+ for tests |
| macOS app/AU/VST3 | Above plus Xcode 26+ command-line tools, Node.js 22+, JUCE 8.0.7 |
| Frontend | `npm ci --prefix ui`, using the checked-in lockfile |

JUCE is fetched separately under its own license. The build verifies the
wrapper sources before applying the adapters. Do not substitute a newer JUCE
release without reviewing those adapters and their host tests.

On macOS, install Xcode 26 or newer command-line tools. Older SDKs omit the
standard C++20 `std::jthread`/`std::stop_token` used by background workers;
the app's CMake configuration checks this before compiling.
Install CMake, Ninja and Node through your normal package manager. No API key,
personal workspace or network model provider is needed to build or run tests.

## Test selection

```sh
ctest --test-dir build-app --output-on-failure
ctest --test-dir build-app --output-on-failure -R 'compiler|developer|completion'
```

Native window tests need a graphical macOS session. The core and UI unit checks
can run without audio hardware. AU and VST3 tests load the built bundles in
separate processes and do not install plugins. Test workspaces and the local
mock AI endpoint are isolated from the user's documents and credentials.

## Try a clean workspace

```sh
FLOWS_WORKSPACE="$PWD/build-app/preview-workspace" \
  build-app/FlowsStandalone_artefacts/Release/Flows.app/Contents/MacOS/Flows
```

The app seeds seven reference instruments and two generated sample fixtures.
For the larger tutorial library:

```sh
python3 tools/install_gather_examples.py
```

That installer preserves existing edits. Set `--target` to a different folder
when testing it. Normal workspace location: `~/Library/Flows/Workspace`.

## Plugin bundles

- AU: `build-app/FlowsPlugin_artefacts/Release/AU/Flows.component`
- VST3: `build-app/FlowsPlugin_artefacts/Release/VST3/Flows.vst3`

To test in a DAW, copy the chosen bundle into the normal user plugin directory
and rescan. Host/device compatibility is separate from an automated build.

## Release package

A release must have a clean checkout and a `v<VERSION>` tag pointing at HEAD.
Run all relevant tests before packaging:

```sh
python3 tools/package_macos.py --build-dir build-app --juce-dir ../JUCE
```

The script stages the app and plugins, includes dependency licenses and
instructions, stamps the public source commit/tag, ad-hoc signs the copies,
verifies them and produces a ZIP with normalized file timestamps. The ZIP
includes Flows and pinned JUCE source archives alongside the binaries. It does not
modify installed apps, upload anything, sign with an Apple Developer identity
or notarize. Source should be available at the matching public tag.

Maintainers can run **Publish macOS preview** in GitHub Actions for an existing
release tag. It builds and tests on a fresh macOS runner, checks dependency
notices, packages the binaries and source, then stores the package and checksums as an Actions artifact. Enable the
`publish` input to publish the release. Only the separate publishing job receives repository write access;
ordinary CI checks remain read-only.

The deployment target is macOS 11, not a claim of runtime testing on every
macOS version. Current package output is arm64. Windows/Linux desktop builds
need platform adapters before they can be advertised as supported.
