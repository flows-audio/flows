---
name: flows-embed
description: Embed a playable Flows synthesizer, with its editable Flows code, in any website and make it look native to that site. Use when a page should let visitors play an instrument, a synth, piano keys or sound demos, or read and change the code that makes a sound (blogs, portfolios, docs, landing pages, course material), or when someone asks to add Flows, an embedded synth, live-coded sound or interactive audio to a site. Covers choosing between the engine API, the <flows-instrument> web components and full custom styling; reading the host site's design and mapping it to --flows-* variables and ::part() rules; snippets for plain HTML, React/Next.js, Vue/Nuxt, Svelte/SvelteKit, Astro and Jekyll; and browser audio rules.
---

# Flows Embed

Flows Embed puts a real Flows instrument on a web page: the Flows compiler and
DSP engine compiled to WebAssembly, running in an AudioWorklet. It sounds
exactly like the Flows app (CI checks the output sample for sample against the
native engine). It needs **no special headers, no service worker and no
cross-origin isolation**, so it works on GitHub Pages, Netlify, WordPress,
Jekyll and any static host.

Flows is code you can play: `<flows-code>` shows the instrument's source
beside its keys. Visitors edit it and hear the change; numbers that set a
parameter can be dragged while a note sounds, and turning a knob rewrites its
number in the code.

Everything visual is the host page's decision. The default look is transparent
and takes the page's font and colours; CSS custom properties, `::part()` and
slots restyle every piece.

Live examples: <https://flows-audio.github.io/flows/embed/>.

## When to use it

Use it when visitors should **play** something: a synth in a blog post about
sound, an instrument on a musician's portfolio, a demo in course material, a
playful element on a landing page. Do not use it for background music that
starts by itself (browsers block audio before a user gesture), or for audio file
playback (use `<audio>`). `<flows-code>` edits one instrument; for projects,
probes, sequencing and the AI assistant, link to the
[Flows web app](https://flows-audio.github.io/flows/).

## Quick start

```html
<script type="module" src="https://flows-audio.github.io/flows/embed/flows-embed.js"></script>

<flows-instrument preset="soft-keys" code></flows-instrument>
```

That is a complete instrument: title, *Tap to play* button, its editable
source (the `code` attribute; leave it out for an instrument without code),
scope, knobs and keys. Visitors click once to start sound, then play with the mouse, touch, or
the keyboard.

## Choose a layer

| Layer | Use it when | Load |
| --- | --- | --- |
| **Elements, default layout**: `<flows-instrument>` | You want a working instrument fast and will restyle it with variables and parts. | `flows-embed.js` or `flows-embed.iife.js` |
| **Elements, custom layout**: `<flows-instrument>` containing `<flows-code>`, `<flows-keys>`, `<flows-knob>`, `<flows-scope>`, `<flows-play-gate>` and your own markup | The site needs its own arrangement (knobs beside keys, a big scope, only two knobs). | same |
| **Engine only**: `Flows.create()` | The page draws everything itself: glass buttons, drum pads, a canvas visualiser, a game. | `flows-embed-core.js` |

Prefer the highest layer that can reach the design. Most sites need only the
default layout plus 5–15 variables. Use a custom layout when the arrangement
differs. Use the engine only when no piano keyboard or knobs should appear.

## Loading

| Where | URL / import | Notes |
| --- | --- | --- |
| ES module | `https://flows-audio.github.io/flows/embed/flows-embed.js` | Defines the elements, exports `Flows`. |
| Engine only | `https://flows-audio.github.io/flows/embed/flows-embed-core.js` | No elements, no element CSS. |
| Classic script | `https://flows-audio.github.io/flows/embed/flows-embed.iife.js` | For `<script src>` without `type="module"`; sets `window.Flows`. |
| npm (when published) | `npm i @flows-audio/embed`; `import '@flows-audio/embed'` or `import { Flows } from '@flows-audio/embed/core'` | See *Bundlers*. |

The script finds `flows-embed.wasm`, its worklet and `presets/` next to
itself. The GitHub Pages URLs always serve the latest release and send
`Access-Control-Allow-Origin: *`. To self-host, copy the whole `embed/` folder
(or `node_modules/@flows-audio/embed/dist`) and serve it with CORS if it is on
another origin.

Download size: about 24 KB gzip of JavaScript plus 142 KB gzip of WebAssembly
(139 KB total with brotli for the classic script). Nothing loads before the
script runs; the engine downloads once per page and is shared by every
instrument.

## The instrument source

Pick one, in this order of precedence:

1. `source` property or `source="..."` attribute: Flows source text.
2. A child `<script type="text/flows">…</script>`: indentation is removed.
3. `src="/sounds/bass.flows"`: a URL. Samples (`file: "Samples/kick.wav"`) resolve relative to it.
4. `preset="name"`: a bundled instrument (default `soft-keys`).

| Preset | Character | Useful keys |
| --- | --- | --- |
| `soft-keys` | warm poly keys | `filtered.cutoff`, `filtered.resonance`, `amp.A`, `amp.R` |
| `glass-pad` | slow, wide, bright pad | `shimmer.cutoff`, `shimmer.resonance`, `amp.A`, `amp.R` |
| `pluck-bass` | mono bass with glide | `bite.resonance`, `grit.drive`, `grit.mix`, `amp.R` |
| `sine-reference`, `mono-legato-glide`, `trance-rolling-bass`, `stereo-motion`, `probe-lab`, `sample-one-shot`, `sample-loop` | the Flows app's examples | see `flows.params()` |

A minimal instrument with nicely named knobs:

```html
<flows-instrument label="Bell">
  <script type="text/flows">
    import standard as std
    from lib.osc import ClassicSine
    from lib.env import ClassicADSR

    Inst.sigstream(std.event.keys.poly()) as key:
        osc = ClassicSine.make()
        env = ClassicADSR.make()
        tone = osc(pitch: key.pitch, level: -12db)
        amp = env(gate: key.gate, trigger: key.trigger, A: 2ms, D: 600ms, S: 0, R: 800ms)
        return tone * amp * key.velocity
  </script>
</flows-instrument>
```

Language reference: <https://github.com/flows-audio/flows/blob/main/docs/LANGUAGE.md>.
Compile errors appear inside the instrument (`::part(error)`) with line
numbers, and as a `flows-error` event whose `detail.error.diagnostics` lists
`{ line, column, message }`.

### Parameter keys

Every number in the source can become a knob. A parameter's **key** is
`<assignment>.<port>`: in `amp = env(..., R: 800ms)` the release is `amp.R`;
in `filtered = tone(body, cutoff: 2.4khz)` the cutoff is `filtered.cutoff`.
A bare port name (`cutoff`) works when only one parameter has it. List them
with `(await el.ready).params()` or in the console: `document.querySelector('flows-instrument').flows.params()`.

## Elements

### `<flows-instrument>`

| Attribute | Default | Meaning |
| --- | --- | --- |
| `preset`, `src`, `source` | `soft-keys` | The instrument (see above). Changing one reloads it. |
| `label` | preset name | Title text (or use `slot="title"`). |
| `theme` | `none` | `none`, `glass` or `studio`. Themes only set default variables. |
| `controls` | `auto` | `auto` (up to `max-controls` useful knobs), `none`, `all`, or a comma list of keys. |
| `max-controls` | `6` | Limit for `auto`. |
| `scope` | `wave` | `wave`, `spectrum` or `none`. |
| `start`, `octaves`, `labels` | `C3`, `2`, `c` | Passed to the built-in keys. |
| `volume` | `0.8` | Output gain 0..1. |
| `bpm` | `138` | Tempo for tempo-synced LFOs. |
| `code` | absent | Show the editable source above the scope (default layout). |
| `code-readonly` | absent | Show it read-only: numbers stay draggable controls. |

Properties and methods: `flows` (the engine, `null` while loading), `ready`
(Promise of the engine), `state`, `source`, `start()`, `noteOn(note, velocity)`,
`noteOff(note)`, `setParam(key, value)`, `params()`.

Events (bubble, composed): `flows-ready`, `flows-state` (`detail.state`),
`flows-error` (`detail.error`), `flows-param` (`detail.key`, `detail.value`),
`flows-note` (`detail.pitch`, `detail.on`), `flows-load`.

**Custom layout:** any child element other than `<script>`/`<template>`
replaces the built-in code, scope, knobs and keys (the host gets a `custom-layout`
attribute). The header with title and play button stays; hide it with
`::part(header) { display: none }` and place your own `<flows-play-gate>`.

### Child elements

They find their instrument as an ancestor, or anywhere on the page with
`instrument="instrument-id"`.

| Element | Attributes | Notes |
| --- | --- | --- |
| `<flows-keys>` | `start` (`C3` or MIDI number), `octaves` (1–8), `labels` (`c`, `all`, `none`), `velocity`, `computer-keys="off"` | Pointer glissando; arrows move focus; Space/Enter play; A W S E D F T G Y H U J K play once a key is clicked (or the keyboard focused), and stay on while the same instrument's knobs or code numbers are dragged; their letters show on the keys, and the host gets an `armed` attribute. Escape, a click outside the instrument, or typing in a text field turns them off. Keys it plays never reach the page's own shortcuts. |
| `<flows-knob>` | `param` (key), `label` | ARIA slider. Drag up/right; Shift for fine; arrows, PageUp/PageDown, Home/End; double-click or Delete restores the source value. Content becomes the label. |
| `<flows-controls>` | `params` (comma list), `max` | A row of knobs. |
| `<flows-scope>` | `mode` (`wave`, `spectrum`) | Canvas; draws only while visible and sounding. |
| `<flows-play-gate>` | | Button that starts sound; hidden while running. Its content is the label. |
| `<flows-code>` | `readonly`, `delay` (ms before recompiling, default 400) | The instrument's Flows source, editable (see below). |

### `<flows-code>`: the code beside the sound

- **Typing** recompiles after a short pause (or at once with Ctrl/Cmd+Enter).
  If the edit only changes live values, they apply in place: held notes keep
  sounding. Other edits rebuild the instrument.
- **Live numbers** (every literal that sets a parameter) are drag handles:
  drag sideways to change the value while you listen, Shift for fine. A click
  places the caret, a double-click selects the number. Alt+Up/Down nudges the
  number at the caret.
- **Knobs and the page write back**: turning a `<flows-knob>` or calling
  `setParam()` rewrites that number in the code, in its own unit (`3.2khz`,
  `520ms`).
- **Mistakes** are underlined, their line numbers marked, and listed below the
  code (click one to jump there). The last version that compiled keeps playing.
- Tab indents (Escape, then Tab, leaves the editor); Enter keeps the indentation.
- Typing stays in the editor: the page's own single-key shortcuts do not see it.
  Escape and Command/Control combinations still reach the page.
- **Reset** appears once the code differs from what was loaded and brings that
  back (the host has a `modified` attribute meanwhile; hide the button with
  `::part(code-reset) { display: none }` to place your own and call `reset()`).
- With `--flows-code-max-height` the code scrolls in its box, and a number
  changed by a knob scrolls into view and lights up.
- Properties `value` (get/set the text) and `original` (as loaded); methods
  `apply()` (compile now, resolves to `true` when it compiled) and `reset()`;
  event `flows-apply` (`detail.source`, `detail.ok`, `detail.diagnostics`).

Give it the host site's code look: `--flows-code-font` and `--flows-code-size`
from the site's code blocks, `--flows-code-background`, and the eight
`--flows-syntax-*` colours from the site's syntax theme (the defaults mix a
hue with the text colour, so they read on light and dark pages alike).

## Engine API (`flows-embed-core.js`)

```js
import { Flows } from 'https://flows-audio.github.io/flows/embed/flows-embed-core.js';

const flows = await Flows.create({ preset: 'pluck-bass' });  // or { source } / { src }
button.addEventListener('pointerdown', () => {
  flows.start();              // must run inside the gesture; resolves when running
  flows.noteOn('E1', 0.9);    // MIDI number or name (C4 = 60)
});
button.addEventListener('pointerup', () => flows.noteOff('E1'));
```

| Member | Description |
| --- | --- |
| `Flows.create(options)` | Fetches, compiles, returns an instrument in state `ready`. Options: `source`, `preset`, `src`, `baseUrl`, `context` (your own AudioContext), `destination` (AudioNode or `null`), `volume`, `bpm`, `boundarySeconds`. Rejects with `FlowsCompileError` (`.diagnostics`). |
| `start()` | Starts sound. Call from a click, tap or key handler. |
| `noteOn(note, velocity \| { velocity, time })`, `noteOff(note, { time })` | `time` is AudioContext time for sample-accurate scheduling. |
| `allNotesOff()`, `panic()`, `heldNotes()` | |
| `params()`, `param(key)`, `getParam(key)`, `setParam(key, value)`, `resetParams()` | Each parameter has `key`, `label`, `unit`, `min`, `max`, `step`, `initial`, `value`, `live`, and `from`/`to`: where its literal is in `source` (-1 when it has none). `setParam` clamps and returns the applied value. |
| `analyser({ size })` | `waveform(into?)`, `spectrum(into?)` (dB), `level()`, `node`. |
| `on(type, fn)` → unsubscribe | `state`, `error`, `param`, `note`, `load`, `meter` (`peak`, `voices`). |
| `state` | `loading`, `ready`, `starting`, `running`, `suspended`, `error`, `disposed`. |
| `output`, `context`, `volume`, `bpm`, `peak`, `voices` | `output` is a GainNode you can route anywhere. |
| `load({ source \| preset \| src })`, `dispose()` | A new source that changes only live values applies in place (the `load` event has `hot: true`, notes keep sounding); anything else replaces the engine. |
| `Flows.configure({ assets, wasm, worklet })` | Where the files live, when not next to the script. |
| `formatValue(param, value)`, `formatLiteral(param, value, original)`, `toPosition`, `fromPosition`, `notePitch`, `noteName` | Helpers for building your own controls with the same knob response. |

All instruments share one AudioContext (`Flows.context`); one gesture starts
all of them. Each has its own audio node and engine instance, so notes and
parameters never leak between them.

## Make it look native to the host site

Work through these steps; do not invent a new style.

1. **Read the host's design.** Open the site's main stylesheet(s), layout and a
   representative page. Note: root custom properties (`--color-*`, `--radius`,
   `--font-*`), body font family and size, text and muted text colours, link or
   brand colour, background(s), card/panel styles (background, border,
   radius, shadow), `backdrop-filter` use, button styles, dark mode handling
   (`prefers-color-scheme`, a `.dark`/`[data-theme]` class), and the spacing
   scale. In an unfamiliar codebase, search for `--`, `font-family`,
   `border-radius`, `box-shadow`, `backdrop-filter` and the primary colour.
2. **Start from `theme="none"`** unless the site is already glassy (`glass`)
   or a solid hardware look fits (`studio`).
3. **Map tokens to variables** on the element (or a wrapper class). Reference
   the site's own variables instead of copying values, so its theme switch
   keeps working:

   | Host design | Flows variable |
   | --- | --- |
   | body/heading font | `--flows-font` (default: inherited) |
   | text colour | `--flows-text` (default: inherited) |
   | secondary text | `--flows-muted` |
   | brand / link / primary | `--flows-accent` |
   | card background | `--flows-surface` |
   | button / raised control background | `--flows-surface-raised` |
   | card border colour / width | `--flows-border-color`, `--flows-border-width` |
   | card radius | `--flows-radius` (keys and scope derive from it) |
   | card padding / spacing | `--flows-padding`, `--flows-gap` |
   | card shadow | `--flows-shadow` |
   | glass blur / saturation | `--flows-blur`, `--flows-saturate` |
   | hover / focus glow | `--flows-glow`, `--flows-focus` |

4. **Refine with parts** where variables are not enough: gradients,
   multiple shadows, borders on single keys, typography of labels.
   `::part()` accepts any property, including `backdrop-filter`.
5. **Replace text with slots**: `slot="title"` for the heading,
   `slot="gate"` for the start prompt, in the site's voice and language.
6. **Check** in light and dark mode, at phone width (320–390 px), with
   keyboard focus visible, and that text on `--flows-surface` keeps at least
   4.5:1 contrast.

### Recipe: liquid glass

```css
flows-instrument {
  --flows-text: var(--ink);             /* the site's tokens */
  --flows-accent: var(--brand);
  --flows-glow: color-mix(in srgb, var(--brand) 45%, transparent);
  --flows-radius: 32px;
  --flows-padding: 24px;
  --flows-key-radius: 14px;
}
flows-instrument::part(panel) {
  background: linear-gradient(145deg, rgb(255 255 255 / .5), rgb(255 255 255 / .14) 55%, rgb(255 255 255 / .26));
  border: 1px solid rgb(255 255 255 / .62);
  -webkit-backdrop-filter: blur(22px) saturate(170%);
  backdrop-filter: blur(22px) saturate(170%);
  box-shadow: inset 0 1.5px 0 rgb(255 255 255 / .9), 0 30px 60px -28px rgb(20 33 61 / .45);
}
flows-instrument::part(key-white) {
  background: linear-gradient(180deg, rgb(255 255 255 / .78), rgb(255 255 255 / .34));
  border-color: rgb(255 255 255 / .85);
}
flows-instrument::part(key-pressed) { box-shadow: 0 0 26px var(--flows-glow); }
```

Glass only reads as glass over something colourful: the page needs colour
behind the instrument (gradient, image, blurred shapes).

### Recipe: dark site

```css
flows-instrument {
  --flows-accent: var(--accent);
  --flows-surface: var(--card-bg);
  --flows-border-color: var(--card-border);
  --flows-border-width: 1px;
  --flows-padding: 20px;
  --flows-key-white: color-mix(in srgb, var(--text) 85%, transparent);
  --flows-key-black: var(--bg);
}
```

With `theme="none"` on a dark page, keys follow the text colour, so white
keys are dark and black keys are light. Set `--flows-key-white` and
`--flows-key-black` as above for a conventional keyboard.

### All variables

| Variable | Default (`theme="none"`) | Used for |
| --- | --- | --- |
| `--flows-font` | inherit | All text |
| `--flows-text` | inherit | Text, knob indicator |
| `--flows-muted` | 60% text | Status, values, key labels |
| `--flows-accent` | currentColor | Value arcs, pressed keys, scope line, play icon, focus |
| `--flows-surface` | transparent | Instrument panel |
| `--flows-surface-raised` | transparent | Play button, knob face |
| `--flows-border-color` | 25% text (buttons), none (panel) | Panel and button borders |
| `--flows-border-width` | 0 panel, 1px controls | Borders |
| `--flows-radius` | 10px | Panel; keys and scope derive from it |
| `--flows-padding` | 0 | Panel padding |
| `--flows-gap` | 14px | Space between parts |
| `--flows-shadow` | none | Panel shadow |
| `--flows-blur`, `--flows-saturate` | 0px, 100% | Panel backdrop filter |
| `--flows-glow` | transparent | Glow on pressed keys, value arcs, play hover, scope |
| `--flows-focus`, `--flows-focus-width` | accent, 2px | Keyboard focus outline |
| `--flows-transition` | 90ms | Transition duration (0 with reduced motion) |
| `--flows-error` | #c2410c | Error box |
| `--flows-title-size`, `--flows-title-weight`, `--flows-status-size` | 1em, 600, .8em | Header text |
| `--flows-key-height` | 112px | Keyboard height |
| `--flows-key-white`, `--flows-key-black` | 4% text, text colour | Key fills (any background, gradients too) |
| `--flows-key-active` | accent | Pressed key fill |
| `--flows-key-border` | 22% text | Key borders |
| `--flows-key-radius` | radius / 2 | Key corners |
| `--flows-key-label-size` | 11px | Key labels |
| `--flows-key-hint`, `--flows-key-hint-black`, `--flows-key-hint-size` | muted, `Canvas`, label size | Computer-key letters on white and black keys, shown while the keyboard has focus |
| `--flows-knob-size` | 48px | Knob diameter |
| `--flows-knob-face` | surface-raised | Knob background |
| `--flows-knob-track`, `--flows-knob-value` | 16% text, accent | Arc colours |
| `--flows-knob-indicator`, `--flows-knob-stroke`, `--flows-knob-shadow` | text, 8, none | Knob details |
| `--flows-label-size`, `--flows-value-size` | 12px, 11px | Knob label and value |
| `--flows-gate-background`, `--flows-gate-padding`, `--flows-gate-radius` | surface-raised, .45em .9em, 999px | Play button |
| `--flows-scope-height` | 72px | Scope height |
| `--flows-scope-line`, `--flows-scope-line-width`, `--flows-scope-fill`, `--flows-scope-grid` | accent, 2, transparent, 10% text | Scope drawing |
| `--flows-scope-background`, `--flows-scope-radius` | transparent, radius × .6 | Scope box |
| `--flows-code-font`, `--flows-code-size`, `--flows-code-line-height` | system monospace, 13px, 1.6 | Code text |
| `--flows-code-background`, `--flows-code-border`, `--flows-code-radius`, `--flows-code-padding` | surface-raised or 5% text, 14% text, radius × .6, 12px | Code box |
| `--flows-code-max-height` | none | Scroll the code past this height |
| `--flows-code-text`, `--flows-code-gutter`, `--flows-code-caret`, `--flows-code-selection` | text, 38% text, accent, 26% accent | Code details |
| `--flows-code-param`, `--flows-code-param-line`, `--flows-code-param-active` | 7% text, 35% text, 22% accent | Live numbers (drag handles) |
| `--flows-syntax-keyword`, `-type`, `-function`, `-property`, `-number`, `-string`, `-comment`, `-operator` | hues mixed with the text colour | Syntax colours |

Variables set on the page always win over themes. Set them on
`flows-instrument` (they inherit into all child elements) or on a standalone
child element.

### All parts

| Part | Element |
| --- | --- |
| `panel`, `header`, `title`, `status`, `error`, `body` | `<flows-instrument>` frame |
| `gate`, `gate-button`, `gate-icon`, `gate-label` | play button |
| `scope`, `scope-canvas` | scope |
| `controls` | knob row |
| `knob`, `knob-dial`, `knob-track`, `knob-arc`, `knob-indicator`, `knob-label`, `knob-value` | each knob (`knob-track`/`knob-arc` are SVG paths: use `stroke`) |
| `keys`, `key`, `key-white`, `key-black`, `key-pressed`, `key-label`, `key-hint` | keyboard; `key-pressed` is present only while a key is held |
| `code`, `code-gutter`, `code-line-number`, `code-line-bad`, `code-view`, `code-input`, `code-param`, `code-problems`, `code-problem`, `code-reset` | code editor; `code-line-bad` marks lines with a problem, `code-view` is the highlighted text and `code-input` the textarea over it; `code-reset` is the Reset button, shown once the code is edited |

Parts of nested elements are re-exported, so `flows-instrument::part(key-white)`
works for the built-in layout. In a custom layout, style the child element
itself: `flows-keys::part(key-white)`. State is exposed as part names
(`key-pressed`) because `::part()` cannot be combined with attribute
selectors; pseudo-classes work (`::part(knob-dial):focus-visible`,
`::part(key):hover`).

### Slots

| Slot | On | Default |
| --- | --- | --- |
| `title` | `<flows-instrument>` | `label` attribute or preset name |
| `gate` | `<flows-instrument>` | "Tap to play" |
| default | `<flows-instrument>` | custom layout (replaces scope, knobs and keys) |
| default | `<flows-play-gate>` | "Tap to play" |
| default | `<flows-knob>` | the parameter label |

## Frameworks

Custom elements work everywhere once the module is imported **in the browser**.
The import itself is safe during server rendering (it defines nothing there),
but instruments only start in the browser.

**Plain HTML / Jekyll / Hugo / WordPress**: put the script tag in the layout or
include (Jekyll: `_includes/flows.html`, then `{% include flows.html %}`):

```html
<script src="https://flows-audio.github.io/flows/embed/flows-embed.iife.js" defer></script>
<flows-instrument preset="soft-keys" theme="glass"></flows-instrument>
```

Liquid and Markdown leave the element untouched; in Markdown, keep it on its
own lines with blank lines around it.

**React 19 / Next.js (App Router)**:

```tsx
'use client';
import { useEffect, useRef } from 'react';

export function Synth() {
  const ref = useRef<HTMLElement>(null);
  useEffect(() => {
    void import('@flows-audio/embed'); // or load the CDN script in your layout
    const el = ref.current;
    const onParam = (e: Event) => console.log((e as CustomEvent).detail);
    el?.addEventListener('flows-param', onParam);
    return () => el?.removeEventListener('flows-param', onParam);
  }, []);
  return <flows-instrument ref={ref} preset="soft-keys" theme="glass" />;
}
```

TypeScript JSX typing (once, e.g. `flows.d.ts`):

```ts
import type { DetailedHTMLProps, HTMLAttributes } from 'react';
type FlowsProps = DetailedHTMLProps<HTMLAttributes<HTMLElement>, HTMLElement> & Record<string, unknown>;
declare module 'react' {
  namespace JSX {
    interface IntrinsicElements {
      'flows-instrument': FlowsProps; 'flows-keys': FlowsProps; 'flows-knob': FlowsProps;
      'flows-scope': FlowsProps; 'flows-controls': FlowsProps; 'flows-play-gate': FlowsProps;
    }
  }
}
```

React 18 passes attributes as strings, which is all these elements need;
listen to events with a ref as above.

**Vue 3 / Nuxt**: tell the compiler these are custom elements.

```js
// vite.config.js
vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('flows-') } } })
// nuxt.config.ts
export default defineNuxtConfig({ vue: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('flows-') } } })
```

```vue
<script setup>
import { onMounted } from 'vue';
onMounted(() => import('@flows-audio/embed'));
</script>
<template>
  <flows-instrument preset="glass-pad" theme="glass" @flows-param="(e) => console.log(e.detail)" />
</template>
```

**Svelte / SvelteKit**:

```svelte
<script>
  import { onMount } from 'svelte';
  onMount(() => import('@flows-audio/embed'));
</script>

<flows-instrument preset="pluck-bass" theme="studio"></flows-instrument>
```

**Astro**: a `<script>` is bundled and runs in the browser only.

```astro
<flows-instrument preset="soft-keys"></flows-instrument>
<script>
  import '@flows-audio/embed';
</script>
```

Or without npm: `<script is:inline type="module" src="https://flows-audio.github.io/flows/embed/flows-embed.js"></script>`.

**Bundlers (Vite, webpack, Rollup)**: the package loads its engine with
`new URL('./flows-embed.wasm', import.meta.url)`, which bundlers copy
automatically. In Vite, exclude it from dependency pre-bundling, which breaks
that URL in development:

```js
export default { optimizeDeps: { exclude: ['@flows-audio/embed'] } };
```

The `presets/` folder is not copied. Either pass your own `source`/`src`, or
call once: `Flows.configure({ assets: 'https://flows-audio.github.io/flows/embed/' })`.

## Browser audio rules and common problems

| Symptom | Cause and fix |
| --- | --- |
| No sound until a click | Browsers require a user gesture. Keep the play button visible, or call `start()` inside a click/keydown handler. Pressing a key on `<flows-keys>` also counts. Never call `start()` from a timer or on load. |
| No sound on iPhone | The ring/silent switch mutes Web Audio in Safari. Tell users to turn it off, or set `navigator.audioSession.type = 'playback'` (Safari 17+) in the start gesture. |
| Stops when the tab is hidden | Browsers may suspend audio; the state becomes `suspended` and the play button returns. |
| `Could not load the Flows engine … (network or CORS)` | Self-hosted files on another origin need `Access-Control-Allow-Origin`. The .wasm should be served as `application/wasm`. |
| CSP errors | Add `'wasm-unsafe-eval'` to `script-src`, allow the asset origin in `script-src` and `connect-src`. If `blob:` scripts are blocked, the worklet loads from the hosted `flows-embed-worklet.js` automatically. |
| `Unknown parameter` / knob shows nothing | Check keys with `flows.params().map(p => p.key)`; keys follow assignment names in the source. |
| Preset not found after bundling | See *Bundlers*. |
| Crackles with many instruments | Each sounding instrument costs CPU on the shared audio thread. Keep a few per page; `dispose()` ones that scroll away for good. |
| Element shows unstyled text before loading | Normal before the script runs. Add `flows-instrument:not(:defined) { display: block; min-height: 220px; }` to reserve space. |

Do not register service workers or set COOP/COEP headers for the embed; it
needs neither. Cross-origin isolated pages work too.

Accessibility comes built in: keys are buttons with note names and
`aria-pressed`, knobs are `role="slider"` with readable values, the status is
a live region. Keep these when restyling: do not hide focus outlines without
replacing them, and keep labels.

## Verify before you finish

1. The page loads without console errors; the instrument shows its title and
   play button (state `ready`).
2. Click play, then a key: `document.querySelector('flows-instrument').flows.peak`
   rises above 0 and the key highlights.
3. Compare a screenshot with the site's other cards and buttons in light and
   dark mode and at phone width; colours, radius, font and shadow should
   match.
4. Tab to the keys and knobs: focus is visible, Space plays, arrows turn knobs.
5. With code shown: change a number in it and hear it; drag a live number
   while holding a note; break a line and see it marked while the sound keeps
   playing. The code should look like the site's own code blocks.

The examples at <https://flows-audio.github.io/flows/embed/> (source in
`embed/examples/` of the Flows repository) are tested in Chromium, Firefox
and WebKit: a minimal blog (no styling, with its code), a liquid-glass
personal site (custom layout and parts) and a dark portfolio (engine only).
