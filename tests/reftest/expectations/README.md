# Reftest expectations

The reftest runner (`test_paint_reftest`, source `tests/unit/paint/test_reftest.c`) renders each web-platform-tests reftest
and its references and compares the pixels. These files record which tests are known not to pass.

## Files and format

One file per WPT directory (`css-grid.txt` for `css/css-grid`, `CSS2.txt` for `css/CSS2`, `svg-painting.txt` for
`svg/painting`, ...). Each line that is not blank and does not start with `#` is

    <test id> <reason tag> [note]

`<test id>` is the path as it appears in `tests/wpt/reftests.txt` (or `tests/reftest/ours/reftests.txt`), for example
`css/CSS2/syntax/at-charset-007.xht`. The note is free text; for `not-yet` it names the missing feature.

Rules the runner enforces:

- A failing test must be listed. An unlisted failure is reported as `FAIL` and the run exits non-zero.
- A listed test must fail. A listed test that passes is reported as an error, so delete the line when you fix the bug.
  Tests tagged `platform` accept either result.
- Ids are unique, every listed id must exist in the test lists, and the tag must be one of the tags below.
- A crash or a timeout is never an expected failure, whatever the line says.
- With `--all`, the `bug` entries of a directory may not exceed 3% of its tests (`BUG_CAP_PERCENT` in the runner). Give a
  failure a more specific tag, or fix it, instead of adding `bug` entries past the cap.

## Tags

| Tag | Meaning |
|---|---|
| `not-yet` | A feature the engine does not implement. The note names it (for example `subgrid`, `ruby`, `grid-lanes`). Not a bug of existing code. |
| `writing-mode` | Needs vertical or sideways `writing-mode`, which is not implemented. |
| `multicol` | A multi-column feature that is not implemented yet. No test carries it at the moment. |
| `r3-effects` | A paint effect that differs: shadows, filters, masks, clip-path, blend modes or border-image. |
| `svg` | An SVG feature that is not implemented. |
| `jpeg` | The test needs a codec or image feature the engine lacks. No test carries it at the moment. |
| `xhtml` | Needs real XML parsing; `.xht` and `.xhtml` files go through the HTML parser. |
| `font-metrics` | The default font's metrics differ from the font the reference was drawn with: a few antialiasing pixels, or a line box a pixel or two taller or shorter. Not a layout bug. |
| `platform` | Passes on some GPU drivers and fails on others (antialiasing above the noise floor). Either result is accepted. |
| `needs-js` | The test needs script behaviour the runner does not provide. |
| `needs-http` | The test needs a response header or status the runner does not serve. |
| `out-of-scope` | Tests behaviour the project does not cover (for example `display: run-in`, removed from the spec), or a fixture the vendored subset lacks. |
| `not-applicable` | The test expects behaviour that a later spec superseded. The note says `superseded by <spec>`; passing it would mean implementing the older rule. |
| `layout` | A layout feature that is known not to be done; the note names it. |
| `paint` | A paint feature that is known not to be done; the note names it. |
| `blur-pyramid` | A blur with a sigma above the direct limit runs on a downsampled pyramid and differs from the reference. |
| `bug` | An unclassified defect of code that exists. |

## Running

Build the tests, then run the runner (`build/tests/test_paint_reftest`):

    test_paint_reftest                    the smoke subset: every 40th test of each directory plus all of tests/reftest/ours (what ctest runs)
    test_paint_reftest --all              every test, plus the checks above including the `bug` cap
    test_paint_reftest --only <id>        one test, verbose
    test_paint_reftest --filter <substr>  the tests whose id contains the text
    test_paint_reftest --shot <id>        render one page to <out>/shot.bmp
    test_paint_reftest --threads N        worker processes (or EMBER_REFTEST_THREADS)

A run writes `failures.txt` (id, a tab, the detail) and the `.test`, `.ref` and `.diff` images of each unexpected failure to
`build/tests/reftest-out/` (override with `EMBER_REFTEST_OUT`). `EMBER_REFTEST_TIMEOUT` sets the seconds a worker may take for
one test. The header comment of the runner lists every option.

## Adding and removing entries

- **A test you fixed now passes:** delete its line. The next run reports a listed test that passes as an error, which is how you
  find them.
- **A new failure:** fix it, or add a line with the most specific tag and a note. Do not use `bug` for something you understand.
- **Regenerating after a full run:**

      test_paint_reftest --all
      python tools/wpt/classify_reftests.py build/tests/reftest-out/failures.txt

  A test that already has a line keeps its tag and note. New failures are classified by path and content rules in
  `tools/wpt/classify_reftests.py` and fall back to `bug`; review the result.
- **Our own tests:** add a page and its `-ref.html` to `tests/reftest/ours/` and list the page in
  `tests/reftest/ours/reftests.txt`.
