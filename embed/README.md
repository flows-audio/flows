# Flows Embed

A playable [Flows](https://github.com/flows-audio/flows) instrument, with its
editable code, for any web page. The Flows compiler and DSP engine run as WebAssembly in an AudioWorklet,
so an instrument sounds exactly as it does in the Flows app. It needs no
special server headers or service worker, and every colour, corner and font
can come from your site.

```html
<script type="module" src="https://flows-audio.github.io/flows/embed/flows-embed.js"></script>
<flows-instrument preset="soft-keys" code></flows-instrument>
```

Visitors play it and change it: edit the code, drag a number in it while a
note sounds, or turn a knob and watch its number change in the code.

- **Engine**: `import { Flows } from '@flows-audio/embed/core'`; draw your own interface.
- **Elements**: `<flows-instrument>`, `<flows-code>`, `<flows-keys>`, `<flows-knob>`, `<flows-controls>`, `<flows-scope>`, `<flows-play-gate>`.
- **Appearance**: `--flows-*` custom properties, `::part()` and slots; `theme="none" | "glass" | "studio"`.

The [integration guide](skill/SKILL.md) covers all of it, with framework
snippets and browser audio rules. It is also a Claude Code skill: copy
`skill/` into `.claude/skills/flows-embed/` of a project and coding agents will
use it to embed and style instruments. Live examples:
<https://flows-audio.github.io/flows/embed/>.

## Develop

The engine is the `flows_embed` CMake target (see [docs/WEB.md](../docs/WEB.md#embedding-flows-in-other-sites)):

```sh
emcmake cmake --preset web && cmake --build --preset web --target flows_embed
npm ci && npm run build     # dist/: modules, classic script, .wasm, presets, types
npm test                    # server-side import and unit tests
npm run parity -- --native ../build/flows   # sample-exact against the native engine
```

## License

Apache-2.0, like the Flows compiler and engine it contains. The WebAssembly
includes Emscripten runtime libraries; see [THIRD_PARTY.md](THIRD_PARTY.md).
