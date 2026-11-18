# Web platform support

Ember implements a subset of the web platform for application and game UIs. This page says what is in, what is out, and how
that is measured. The sources of truth are the code under `src/`, the WebIDL files in `src/bindings/idl/`, and the
expectation files under `tests/` (see [Testing](testing.md)): each lists the tests that are known to fail, with a reason.

Numbers here come from files in the repository and were checked at commit `7e80abf3` (2026-11-14).

## At a glance

| Area | Status |
|---|---|
| HTML parsing | WHATWG tokenizer and tree builder, fragment parsing, serializer. Passes the vendored html5lib tree-construction and tokenizer tests (the unit test fails below a 99% pass rate and lists its known failures). |
| DOM | Core DOM, events, `MutationObserver`, `ResizeObserver`, `IntersectionObserver`, `AbortController`, traversal (`NodeIterator`, `TreeWalker`), `DOMParser`, forms with constraint validation. No shadow DOM, no custom elements. |
| CSS | Large part of the modern cascade, selectors, values and units, transitions and animations, flex, grid, table, float and positioned layout, transforms including 3D, filters, masks, blend modes, multi-column layout. No vertical writing modes, no subgrid, no ruby. |
| Text | OpenType (TrueType, CFF, CFF2), variable fonts, colour fonts (COLR/CPAL, CBDT, sbix), WOFF and WOFF2, shaping, bidirectional text, system font fallback. |
| Images | PNG, JPEG, GIF (including animated GIF), SVG (when built with `EMBER_SVG`). No WebP. |
| Graphics | Canvas 2D (when built with `EMBER_CANVAS`), SVG. No WebGL, no video or audio. |
| JavaScript | Own engine: ES2015 to ES2025 plus parts of ES2026, ES modules with import maps. `Intl` is a small en-US subset. |
| Web APIs | `fetch`, XMLHttpRequest, WebSocket, EventSource, Streams, `Blob`/`File`/`FormData`, URL, encoding, `localStorage`, Web Animations, messaging in one document, `structuredClone`, `crypto.getRandomValues` and `crypto.randomUUID` only. |
| Not available | Workers, WebAssembly, IndexedDB, iframes and other nested browsing contexts, WebGL, media playback. |

## HTML and the DOM

- Parser: the WHATWG HTML tokenizer and tree builder, with the scripting flag (so `<noscript>` content is built only when scripts are off), `innerHTML` and fragment parsing, and the serializer. Input bytes are decoded with the
  WHATWG Encoding Standard decoders for UTF-8, UTF-16 and the single-byte encodings; the CJK legacy encodings are listed but
  not decoded (`src/loader/encoding.c`).
- DOM: nodes, elements, attributes, `DOMTokenList`, `NamedNodeMap`, `HTMLCollection`, `NodeList`, `Document`,
  `DocumentFragment`, `DOMImplementation`, `DOMParser`, `XMLDocument`, `CDATASection`, `ProcessingInstruction`, `Text`,
  `Comment`, `DocumentType`, `NodeIterator`, `TreeWalker`, `NodeFilter`. HTML element interfaces are in
  `src/bindings/gen/html.h` (58 `HTML*Element` interfaces).
- Events: `Event`, `CustomEvent`, `UIEvent`, `MouseEvent`, `PointerEvent`, `WheelEvent`, `KeyboardEvent`, `InputEvent`,
  `CompositionEvent`, `FocusEvent`, `TouchEvent`, `ClipboardEvent`, `DragEvent`, `SubmitEvent`, `HashChangeEvent`,
  `PopStateEvent`, `PromiseRejectionEvent`, `ErrorEvent`, `MessageEvent`, `CloseEvent`, `StorageEvent` and more.
- Forms: text controls with editing, selection and IME composition; checkboxes, radios, selects, buttons, the
  constraint validation API, `FormData`, and submission through the host's navigation hook. `pattern` validation and some
  other details are incomplete (see the `forms` entries in `tests/wpt-th/expectations/`).
- Window and environment: `window`, `location`, `history`, `navigator`, `screen`, `matchMedia`, `getComputedStyle`,
  timers (`setTimeout`, `setInterval`, `requestAnimationFrame`, `queueMicrotask`), `performance.now`, `console`,
  `structuredClone`, `crypto.getRandomValues` and `crypto.randomUUID`, `navigator.clipboard.readText` and `writeText`,
  `document.fonts` (`FontFaceSet`), constructable stylesheets (`CSSStyleSheet`, `document.adoptedStyleSheets`), the CSS
  namespace, `DOMRect`, `DOMPoint` and `DOMMatrix`.
- Not implemented: `Range` and `Selection`, shadow DOM (`attachShadow`), custom elements (`customElements`), iframes and
  other nested browsing contexts (the elements exist, but nothing loads in them), `requestIdleCallback`, `ElementInternals`,
  drag and drop sessions.

## CSS

The property database is `src/css/props.txt` (419 properties and shorthands as of this revision). Being in the database means
the property parses and computes; layout and paint support is listed below.

Supported:

- Selectors level 4: combinators, attribute selectors, structural pseudo-classes, `:is()`, `:where()`, `:not()`, `:has()`,
  `:nth-child(... of S)`, dynamic and form pseudo-classes (`:hover`, `:focus-visible`, `:checked`, `:valid` and so on), CSS
  Nesting.
- Cascade: origins, specificity, `!important`, cascade layers (`@layer`), `@scope`, `@supports`, `@media`, `@container`,
  `@import`, custom properties with `var()`, registered custom properties (`@property`), `all`.
- Values: `calc()` and the math functions, relative units, viewport units, `color-mix()`, `oklch()` and other color spaces,
  gradients (linear, radial, conic), counters, `content`.
- Layout: block flow with margin collapsing, inline formatting with bidi, floats and clearance, positioned layout including
  sticky, flexbox, grid (tracks, areas, auto placement, `minmax`, `fit-content`, `repeat`, aspect ratios, absolutely
  positioned grid children), tables (both border models, fixed and auto layout), `display: contents` and `flow-root`,
  intrinsic sizing keywords, `aspect-ratio`, scroll containers with scrollbars and `overflow`, `scroll-behavior`,
  `position: fixed`.
- Painting: backgrounds and borders (including `border-radius`, `border-image`), `box-shadow`, `text-shadow`, `opacity`,
  `filter` and `backdrop-filter`, `mix-blend-mode` and `background-blend-mode`, masks, `clip-path`, 2D and 3D transforms with
  `perspective`, `object-fit`, text decoration, outlines, list markers.
- Motion: CSS transitions, CSS animations with `@keyframes`, and the Web Animations API (`Element.animate`, `getAnimations`).
  Animations and transitions of pseudo-elements are not supported.
- Fonts: `@font-face` (with `local()`), variable font axes, `font-feature-settings`.
- Environment: `prefers-color-scheme` (set per view with `emb_view_set_color_scheme`), `color-scheme`, `accent-color`.

Known gaps (each has failing, tagged tests in `tests/reftest/expectations/`):

- Multi-column layout covers block containers. Not yet: `box-decoration-break: clone`, the `orphans` and `widows`
  properties (both are fixed at 2), and floats and `clear` that respect the column they fall in.
- Vertical and sideways `writing-mode` values (39 tagged `writing-mode`).
- Grid: `subgrid` and grid lanes (masonry). Grid lanes account for 657 of the `not-yet` reftests.
- `ruby`, `text-wrap` modes (`balance`, `pretty`), `hyphens: auto` (it acts as `manual`), `text-autospace`, `text-justify`,
  `hanging-punctuation`, `line-clamp`, `margin-trim`, `corner-shape`, `contain-intrinsic-size`, `::marker` details,
  `text-decoration-skip-ink` (acts as `none`), scroll-driven animations, view transitions, `@starting-style`, anchor
  positioning, `font-variation-settings` interpolation, and a number of newer features. The `not-yet` tags in the expectation
  files name each one.
- A first-line style that changes line metrics is not laid out with those metrics (`::first-line`).
- Stylesheet bytes in legacy encodings other than UTF-8 and UTF-16.
- `@page` and print media.

## Text and fonts

- Font formats: TrueType and OpenType outlines (glyf, CFF, CFF2), TrueType collections, variable fonts, colour glyphs (COLR/CPAL,
  and bitmap strikes CBDT/CBLC and sbix), WOFF and WOFF2. Inputs are bounds-checked and fuzzed (`tests/fuzz`).
- Shaping is done by kb_text_shape, bidirectional reordering by SheenBidi.
- Glyphs are drawn from their outlines on the GPU (the Slug algorithm), not from a glyph atlas, so text stays sharp under
  scaling and transforms. Windowed views use LCD subpixel antialiasing where it is safe (`emb_text_aa`).
- Text hinting is a view setting (`emb_text_hinting`: NATURAL, SHARP, NONE; see `docs/embedding.md`). NATURAL is vertical
  hinting under 36 px, baseline snap and stem darkening. SHARP adds x fitting and whole-px pen positions. NONE is no fitting
  at all, with the contrast curve kept.
- `text-rendering` (inherited; the SVG presentation attribute maps to it): `auto`, `optimizeSpeed` and `optimizeLegibility`
  use the view's hinting; `geometricPrecision` draws that text as NONE.
- `-webkit-text-stroke` strokes the glyph outlines (miter join with limit 4, butt ends) over the fill. Synthetic bold
  emboldens the outline by about a 24th of the em up to 9 px, a 32nd from 36 px, so the stems grow to the right.
- Font selection: registered fonts, `@font-face`, system fonts (indexed on a background thread), generic family mapping set
  by the host, then per-character fallback and the bundled default font (Noto-based, 216 KB).

## Images and graphics

- Image formats: PNG (own decoder), JPEG and GIF (through SDL_image with stb_image), SVG. Animated GIFs play on the frame clock.
  EXIF orientation is applied. WebP, AVIF and other formats are not decoded (SDL_image is built with JPEG and GIF only).
- Decoded images are kept within a budget per view (`emb_runtime_desc.image_cache_bytes`, default 64 MiB).
- Canvas 2D (`EMBER_CANVAS`): `CanvasRenderingContext2D`, `Path2D`, gradients, patterns, `ImageData`, `ImageBitmap` data,
  `TextMetrics`. `OffscreenCanvas` is not implemented, and `getContext` returns `null` for WebGL and other context types.
- SVG (`EMBER_SVG`): inline `<svg>`, SVG images, shapes, paint servers (gradients and patterns), structure, markers, text, the SVG DOM interfaces for lengths, angles, transforms and matrices. SMIL animation (`<animate>`, `<set>`)
  and SVG fonts are not supported.
- `<video>` and `<audio>` elements exist in the DOM but nothing plays.

## JavaScript

Ember has its own engine (`src/js`): a bytecode VM with shapes and inline caches, a non-moving mark-sweep collector with
lazy sweeping, and its own RegExp implementation.

Language coverage, from the test262 configuration (`tests/test262/unsupported-features.txt`): everything in `test/language`
and `test/built-ins` runs, except tests tagged with these features, which are skipped: `await-dictionary`, `decorators`,
`explicit-resource-management`, `import-bytes`, `import-defer`, `import-text`, `source-phase-imports`,
`tail-call-optimization`, `cross-realm` (one realm per runtime), `iterator-includes`, `iterator-chunking`, `joint-iteration`,
`Iterator.prototype.join`, `Atomics.waitAsync`, `Temporal` and `ShadowRealm`.

Beyond that the engine provides classes (including private members and static blocks), generators and async functions,
async generators, `for await`, modules (static and dynamic `import`, `import.meta`, import maps, JSON modules), BigInt,
`Proxy` and `Reflect`, `Symbol`, `WeakRef` and `FinalizationRegistry`, typed arrays, `DataView`, resizable `ArrayBuffer`,
`SharedArrayBuffer` and `Atomics`, iterator helpers, `Intl` (see below), and the newer `Array`, `String`, `Object`,
`Promise` and `Uint8Array` base64 and hex methods.

`Intl` is minimal: `Intl.NumberFormat`, `DateTimeFormat`, `Collator` and others exist, but every locale resolves to en-US and
no CLDR data is bundled. The test262 `intl402` directory therefore has 1,668 known-failing variants in
`tests/test262/expectations.txt`.

Modules load from files, from resource handlers (`app://`) and, with networking, from allowed hosts; the module loader follows
the HTML rules for media types and import maps.

## Web APIs

From the WebIDL files in `src/bindings/idl/` and the script preludes in `src/bindings/js/`:

| Group | Interfaces |
|---|---|
| Network | `fetch`, `Headers`, `Request`, `Response`, XMLHttpRequest (with upload and progress events), `WebSocket`, `EventSource`. Subject to the host's allowlist; no CORS. |
| Streams and data | `ReadableStream` (with readers, controllers and async iteration), `WritableStream`, `TransformStream`, `TextDecoderStream`, `TextEncoderStream`, `Blob`, `File`, `FormData`, `URL`, `URLSearchParams`, `TextEncoder`, `TextDecoder`, `atob`, `btoa`. |
| Storage | `localStorage` and `sessionStorage` (`Storage`, `StorageEvent`); persistence needs `emb_runtime_set_storage`. `document.cookie` exists. No IndexedDB. |
| Animation | `requestAnimationFrame`, `Animation`, `KeyframeEffect`, `DocumentTimeline`, `getAnimations`, CSS animation and transition events. |
| Messaging | `MessageChannel`, `MessagePort`, `postMessage` within one document, `structuredClone`. |
| Geometry | `DOMRect`, `DOMPoint`, `DOMMatrix` and their read-only forms. |
| Other | `console`, `crypto.getRandomValues`, `crypto.randomUUID`, `performance.now`, `Clipboard` (read and write text), `DOMException`, `AbortController`, `AbortSignal`, `FontFaceSet`. |

## Conformance numbers

All counts are derived from files in the repository at commit `7e80abf3`. They are not a statement about any browser
compatibility beyond these suites.

**Layout and render reftests** (vendored subsets of web-platform-tests, run by `test_paint_reftest` at 800x600 on a software
or hardware device):

- `tests/wpt/reftests.txt` plus `tests/reftest/ours/reftests.txt` list 12,098 tests (12,080 from WPT, 18 of our own).
- The expectation files list 1,739 of them as known failures (or, for 5 tagged `platform`, as passing on some drivers only).
  The last full run passed 10,355 of 12,098 (85.6%).
- The tags of the 1,739: `not-yet` 1,195 (657 of them grid lanes), `bug` 197, `font-metrics` 135, `layout` 49, `out-of-scope` 40,
  `writing-mode` 37, `r3-effects` 29, `paint` 21, `needs-http` 21, `not-applicable` 6, `platform` 5, `xhtml` 4.
- The CSS2 suite is the largest part (5,577 tests, 299 listed).
- The selection excludes tests that need scripts, `reftest-wait`, vertical writing modes, print media, iframes and other
  features the engine does not host; so these numbers say little about those areas.

**WPT testharness.js tests** (run by `test_bindings_wpt_th`): `tests/wpt/testharness.txt` lists 2,272 tests (canvas 1,058,
`html/semantics` 274, `dom/nodes` 223, SVG types 84, CSS transitions 83, CSS animations 78, `dom/events` 65, fetch 46, Web
Animations, intersection and resize observers, encoding, storage and more). The expectation files hold 1,557 lines: 132 for
whole tests and the rest for single subtests.

**test262**: every variant of `test/language` and `test/built-ins` that is not skipped for an unsupported feature passes, except
4 `createRealm` variants. The `intl402` expectations hold 1,668 failing variants (Intl is a minimal en-US subset). The runner
exits non-zero for any difference from the expectations, and `ctest` runs a fixed 549-test sample of them.

**html5lib**: tree-construction `.dat` files run through the parser with a 99% overall pass-rate floor and a list of known
failing cases; tokenizer `.test` files run too.

**Framework apps**: unmodified Vite builds of Preact, React 19, Vue 3, Svelte 5 and Solid TodoMVC apps, a React Router app and
a vanilla TypeScript app run scripted scenarios in `test_embed_apps` (see `tests/apps/README.md`).
