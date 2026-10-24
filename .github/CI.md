# Continuous integration and review

Every PR and every push to `main` runs the same checks. New commits cancel
obsolete runs on that PR or branch. Jobs have bounded timeouts; test failures
retain CTest logs and JUnit reports for 14 days. CI uses temporary runners,
read-only repository tokens and no user credentials or audio hardware.

| Check | What it verifies |
| --- | --- |
| Workflow validation | GitHub workflow structure and embedded shell, using a checksum-pinned actionlint |
| Compiler and DSP (Linux/macOS) | Debug builds, language, DSP, session and offline-render regressions |
| Memory and undefined behavior | Linux Clang AddressSanitizer, leak detection and UndefinedBehaviorSanitizer over the portable suite |
| Editor and publication checks | Locked frontend install, high-severity dependency audit, TypeScript/build, UI regressions and publication hygiene |
| macOS app and plugins | arm64 release build with exact JUCE 8.0.7 revision; standalone, AU/VST3 host tests and compiler/editor integration |
| CI | Stable aggregate check; fails if any dependency fails, is skipped or is cancelled |

There are no path filters: documentation-only PRs still produce the required
check. Linux coverage applies to the portable core, not the desktop app.

## Native UI and listening

`window_startup` and `editor_layout` need an interactive macOS desktop and
remain local checks. Run them for editor, layout, keyboard and window changes:

```sh
ctest --preset app -R '^(window_startup|editor_layout)$'
```

CI does not establish audible quality, real MIDI-device behavior, DAW/device
compatibility or performance on every supported OS. Audio changes still need
relevant local listening and timing checks. CI intentionally has no fragile
wall-clock performance threshold on shared runners.

## CodeRabbit

The repository's `.coderabbit.yaml` is the review configuration. Ready PRs
receive an English review; pushes trigger incremental reviews. Drafts are
excluded. Guidance targets DSP/realtime safety, language consistency, editor
interactions, file safety and CI/release provenance. Generated lockfile noise,
third-party material and binary assets are excluded from review, but remain
covered by installation, audit and publication checks where applicable.

Automatic approval and generated follow-up code are disabled. A maintainer
must assess findings, fix confirmed defects, explain any disagreement, and
resolve discussions only after checking the result. A completed bot check is
not evidence that the code is correct. Do not merge while a review is still
running; verify it covers the latest commit. If the service is unavailable,
record an explicit manual review rather than reporting bot approval.

Recommended `main` rules: require a PR, require the **CI** check from GitHub
Actions with an up-to-date base, require resolved discussions, and prohibit
force pushes and branch deletion. The workflow supplies the check; repository
rules are configured separately by an administrator. Do not make an AI approval
a substitute for local verification or require another human approval when
there is no second maintainer available.

## Dependencies and releases

Dependabot proposes weekly Actions and frontend updates. Compatible npm
updates are grouped; major updates remain separate. The scheduled dependency
audit catches newly disclosed high-severity advisories even without new commits.
JUCE and actionlint are pinned explicitly and reviewed when upgraded.

**Publish macOS preview** is manual. Select an existing `v<VERSION>` tag;
the workflow verifies the exact tag, builds, tests, checks notices and packages
binaries with corresponding source. By default it only retains the package as
an Actions artifact for seven days. Enable `publish` to publish a release.
Only the separate publishing job has repository write access; it downloads
that run's tested artifacts and verifies their checksums before uploading.
Publishing rechecks the remote tag against the tested commit before uploading
and before publishing. Administrators should also protect release tags against
updates and deletion; workflow checks alone cannot prevent concurrent tag edits.
No release is triggered by a PR, push or dependency update. Packages remain
ad-hoc signed previews, not Apple-notarized distribution builds.
