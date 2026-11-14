# WPT testharness.js expectations

The runner (`tests/unit/bindings/test_wpt_th.c`) runs the testharness.js tests listed in `tests/wpt/testharness.txt`
(vendored by `tools/wpt/select_testharness.py`, see `tests/wpt/README.md`). `testharnessreport.js` in this directory replaces
the WPT file of the same name: it forwards each subtest result and the completion to the runner through the console callback.

## Running

    build/<dir>/tests/test_bindings_wpt_th              the smoke subset (every 20th test; ctest runs it)
    build/<dir>/tests/test_bindings_wpt_th --all        the full run (also checks the expectations)
    build/<dir>/tests/test_bindings_wpt_th --only dom/nodes/Node-appendChild.html   one test, every subtest
    build/<dir>/tests/test_bindings_wpt_th --filter dom/events --threads 2

`EMBER_WPT_THREADS` (default 2) sets the worker processes, `EMBER_WPT_TH_OUT` (or `--out`) the output directory, which receives
`results.txt` (one line per test: id, outcome, passing subtests, state) and `failures.txt` (one line per non-passing subtest, and
one per test that did not complete). Without a GPU the runner exits 77 (skipped).

## Verdicts

A test passes when it completes (the harness reports completion), the harness status is OK, it has at least one subtest and every
subtest passes. A test that does not complete within the timeout (75 s, 150 s under ASan; the report script gives the harness
six times its default 10 s, and turns off its HTML results table, which costs more to lay out than the thousands of subtests of
the largest tests) is a `timeout`; a worker that dies or
hangs inside a test records that test as `crash` or `hang`. Each such test fails.

A failure is expected when the expectations list it. A listed test or subtest that passes is an error, so a fixed bug must be
deleted from its file. `--all` also reports listed ids that are not in the list, and expectations that were not used.

## Format

One file per directory: `dom-nodes.txt`, `dom-events.txt`, `html-dom.txt`, `html-semantics.txt`, `html-webappapis.txt`, `uievents.txt`,
`console.txt`, `dom-collections.txt`, and so on (the first two path components, joined with `-` for `dom/` and `html/`). Each line is one of:

    <test path> <reason> [note]
    <test path> :: <subtest name> :: <reason> [note]

The first form expects the whole test not to pass. The second expects one subtest not to pass (the name is the subtest's name, exactly,
and may contain spaces, but not " :: "). Blank lines and lines starting with `#` are ignored. The note is free text.

## Reasons

- `bug`: an unclassified defect of the code that exists. `--all` caps the `bug` entries of each directory at 3% of its tests
  (an entry per line, so a directory of fewer than 34 tests has none).
- `not-yet`: a feature the engine does not implement yet; the note names it.
- `out-of-scope`: a test of behaviour the spec no longer requires, or a fixture the vendored subset does not have.
- `platform`: passes on some platforms, fails on others (either result is accepted).
- `layout`: a layout defect (src/layout, src/style, src/paint).
- `input`: a platform or input defect (src/platform, src/embed/input.c).
- `forms`: a control or editing defect (impl_forms.c, src/paint/control.c, src/platform/edit.c): form association,
  constraint validation, activation behavior of controls, reflection of control attributes.
- `scripts`: dynamic scripts and the event loop (src/embed/script.c, src/embed/loop.c): the insertion steps of a
  `<script>` element made by script, parser-driven mutation records, the timer nesting level.
- `needs-http`: needs a response header or status the runner does not serve.
- `harness`: a failure of the harness integration (report, completion) rather than of the test's subject.

## Regenerating

`python tools/wpt-th/gen_expectations.py OUT_DIR SHARD_DIR...` writes one file per directory from the worker records
(`shard-*.out`) of full `--all` runs, run with the expectation directory empty (a later directory overrides an earlier one, so a
directory re-run alone can replace its part). It lists every test that does not pass: one line per failing subtest when the test
completed, one whole-test line when it did not complete or has more than 25 failing subtests. The reason follows the path and the
message (RULES at the top of the script, first match wins); anything no rule claims is `bug`. Review the diff before committing:
a generated file replaces the old one, so an entry that became passing must disappear, and a new `bug` entry is a defect to fix.
