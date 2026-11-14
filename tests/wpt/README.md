# Vendored web-platform-tests subsets

Everything here comes from https://github.com/web-platform-tests/wpt at the commit in `COMMIT`
(3-Clause BSD, see `LICENSE.md`). Files keep their WPT paths, so root-relative references such as
`/fonts/ahem.css` or `/css/support/grid.css` resolve against this directory.

- `html/syntax/parsing/` — tree-construction `.dat` files.
- Layout: `layout-tests.txt` lists check-layout tests from `css/css-flexbox`,
  `css/css-grid`, `css/css-tables`, `css/css-position` and `css/CSS2`, with every local resource they
  reference (stylesheets, images, `fonts/Ahem.ttf`, which is public domain/CC0).
  - A test is in the list only if its script does nothing except call `checkLayout(selector)`
    (directly, from `onload`, or after `document.fonts.ready`), optionally after
    `setup({explicit_done: true})`. Tests that mutate the DOM from script need JS and are not
    vendored.
  - Our runner does not execute scripts. It reads the selector from the `checkLayout` call and
    checks the `data-expected-*` / `data-offset-*` / `data-total-*` attributes on the matching
    elements and their descendants, following `resources/check-layout-th.js` (kept for reference).
- Paint: `reftests.txt` lists the reftests (`rel=match`/`mismatch`) from 21 `css/` directories,
  selected by `tools/wpt/select_reftests.py` with every reference in their chains and every local resource
  they use (stylesheets, PNG images, fonts). A test is left out if any file in its chain has a `<script>`,
  `reftest-wait`, a vertical or sideways writing mode, SVG, a non-PNG image, an iframe/object/embed/video/
  audio/canvas, a 3D transform, `@page` or print media, or a `paged`/`animated`/`interact`/`may`/`should`/
  `dom`/`font`/`speech`/`scroll` flag, or if a local resource is missing. Rerun it against a sparse WPT
  checkout at `COMMIT` to reproduce the list.
- SVG paint: the `svg/...` lines of `reftests.txt` (the reftests from `svg/painting`, `svg/shapes`, `svg/pservers`, `svg/struct`,
  `svg/coordinate-systems` and `svg/render`) come from `tools/wpt/select_reftests.py WPT_ROOT LIST FILES svg`, run against a sparse
  checkout at `COMMIT` with those directories added (and `svg/resources`, `svg/support`). Inline svg, `.svg` tests and SVG images
  are allowed there; scripts, SMIL, filter elements and SVG fonts still exclude a test. Their expectations are the `svg-*.txt` files.
- testharness.js: `testharness.txt` lists the testharness tests from `dom/nodes`, `dom/events`, `dom/collections`,
  `dom/lists`, `dom/traversal`, `dom/abort`, `uievents`, `html/dom` (the `reflection-*.html` files), `html/semantics/forms`,
  `html/webappapis/timers`, `html/webappapis/microtask-queuing` and `console`, selected by `tools/wpt/select_testharness.py`
  (run it against a sparse checkout at `COMMIT`; it prints the counts, with `--why` the number of tests each rule removed, and
  writes the list and the files to vendor).
  - A `.html` test must load `/resources/testharness.js`; a `.any.js` test whose META global includes `window` runs as the
    `foo.any.html` document WPT serves for it (the runner generates that document from the META scripts).
  - A test is left out if it (or a local script it loads) uses an iframe or frame, worker, shadow DOM, custom element,
    `window.open`, testdriver, a server handler, templating, a network request or a fetched absolute URL, `postMessage`, or a
    feature Ember does not have and the spec leaves out (Range, StaticRange, Selection, XPath, cross-document navigation); if
    it is manual, `.tentative`, a `.sub.`/`.https.`/`.h2.`/`.worker.`/`.window.`/`.serviceworker.`/`.www.` variant, or the
    declarative-partial-updates proposal; under `html/dom` only the `reflection-*.html` files are taken (and none with a
    video, audio, canvas, picture, svg or math element).
  - The `dom/events/scrolling` tests and the `dom/nodes/moveBefore` tests that need rendering, scrolling, animation, focus
    movement or style invalidation, and the `dom/events` tests of the legacy webkit-prefixed animation and transition events,
    are left out (they need a running animation).
  - `resources/testharness.js` is the only harness file; `testharnessreport.js` is replaced by
    `tests/wpt-th/testharnessreport.js`. The scripts that the tests load are scanned with the same rules as the tests.
- testharness.js: the same selector (`tools/wpt/select_testharness.py`) also takes `encoding/` (the Encoding tests
  of the shipped encodings; the CJK-only files `legacy-mb-*`, `big5-encoder`, `iso-2022-jp-*`, `eof-shift_jis*` are left out,
  as CJK decoders are out of scope), `fetch/api/` (the static files of `fetch/api/resources/` are vendored with
  them, and `xhr/resources/` holds the `.asis` raw responses and `utf16-bom.json` that the header and json tests fetch; `.py`, `.h2.`, `.sub.` and `.headers` server files are not; the MediaSource and bad-port tests are out of scope), `html/webappapis/structured-clone/` (the
  `structured-clone.any.js` battery; its transferables helper is kept, its MessageChannel subtests are expectations) and
  `webstorage/`. The `webstorage/*.window.js` and `encoding/*.window.js` tests are listed as `*.window.html`, the document WPT
  serves for them: `tools/wpt/gen_window_wrappers.py tests/wpt tests/wpt/testharness.txt` writes those documents next to the
  scripts (they are committed). Their `/common/` helpers come from `common/` (vendored in the same way).  Three `/common`-dependent tests of the DOM and forms directories (two form tests and `html/dom/reflection-sections.html`) need a
  rendering pass or every element's reflection and stay out (`RENDER_LATER` in the selector). 
- testharness.js: `html/canvas/element` (1058 tests of the Canvas 2D API; `tools/wpt/select_testharness.py` takes the
  directory). Besides the usual rules, a canvas test is left out when it needs a second origin, a server handler, a
  worker, OffscreenCanvas, WebGL, video or audio (`CANVAS_BAD`); `offscreen/` directories are skipped. The images and fonts a
  test names from script (`fetch('/images/yellow.png')`, `url(/fonts/CanvasTest.ttf)`) are vendored with it, and so is
  `html/canvas/resources/canvas-tests.js` (its cross-origin helpers are only used by the tests left out). The expected-output
  `.png` of each generated test stays because the page shows it in an `img`.
- testharness.js and reftests: `web-animations/` (112 tests), `css/css-animations/` (78), `css/css-transitions/` (83),
  `resize-observer/` (25) and `intersection-observer/` (82) are in `testharness.txt`, selected by
  `tools/wpt/select_testharness.py` with the usual rules; the files that need a scroll timeline, a worker, an iframe or
  testdriver stay out, and so do `web-animations/` tests of `ScrollTimeline`/`ViewTimeline`, of pseudo-element
  animations (see `W6_PREFIXES` and `W6_OUT` in the selector). `/web-animations/testcommon.js` joins the allowed helpers. 9
  reftests of `css/css-animations` and `css/css-transitions` joined `reftests.txt` (`select_reftests.py` takes the two
  directories; a test with declarative shadow DOM (`shadowroot`) is left out, as one with a script is). Two of the 9 fail
  (`tests/reftest/expectations/css-animations.txt`). `css/css-animations` and `css/css-transitions` files are keyed
  `css-animations` and `css-transitions` in `tests/wpt-th/expectations/`.
