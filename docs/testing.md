# Testing

Every module has tests under `tests/unit/<module>/`. Beyond unit tests there are conformance suites (web-platform-tests
reftests and testharness tests, test262), golden-image tests, framework app scenarios, fuzzers and benchmarks.

Run everything that is part of the normal test run with CTest:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -j2 --output-on-failure
```

Tests that need a GPU device exit with code 77, which CTest reports as skipped, when no device is available. On a Linux
machine without a GPU, Mesa llvmpipe runs them: `SDL_VIDEO_DRIVER=offscreen ctest ...` (this is what CI does; `LP_NUM_THREADS=4`
limits llvmpipe's threads). With `-DEMBER_SANITIZE=ON` CI uses `ASAN_OPTIONS=detect_stack_use_after_return=0` (the JavaScript
engine scans the C stack conservatively, which ASan's fake stacks confuse) and `LSAN_OPTIONS=suppressions=tests/lsan.supp`.

## Unit tests

- `tests/unit/<module>/test_<name>.c` becomes the executable `test_<module>_<name>` (a file already named
  `test_<module>_...` keeps its name). `tests/CMakeLists.txt` globs the files, links the module and its dependencies
  (`src/<module>/deps.txt`, plus an optional `tests/unit/<module>/deps.txt` for test-only modules), and registers each with
  CTest.
- The harness is the header `tests/emb_test.h`:

```c
#include "emb_test.h"

EMB_TEST(addition_works)
{
    EMB_CHECK_EQ_INT(2 + 2, 4);
}

EMB_TEST_MAIN()
```

  It prints one line per test (PASS, FAIL or SKIP) and a summary. The exit status is 0 on success, 1 on a failure or when no
  test is registered, and 77 when every test skipped.
- Run one test directly: `build/tests/test_css_calc_math`, or through CTest: `ctest --test-dir build -R css_calc`.
- Tests find fixtures (fonts, data, `third_party/*-tests`) under `EMBER_SOURCE_DIR`, which CMake defines for every test.
- Options that remove a feature remove its tests too (canvas, SVG, default font, network, compressed assets); the tests of the
  "off" behaviour are built only in builds without the feature.
- Golden images: `tests/emb_gold.h` compares RGBA pixels with a BMP in `tests/gold/`, tolerating small antialiasing differences
  and failing on more than 0.5% mismatching pixels. On failure it writes `<name>.actual.bmp` and `<name>.diff.bmp` to
  `gold-out/`. `EMBER_UPDATE_GOLDENS=1` rewrites the goldens; review the images before committing them.
- Render tests use `tests/emb_render_test.h` and need a GPU device (or llvmpipe).

## Reftests (web-platform-tests)

The reftest runner `test_paint_reftest` (`tests/unit/paint/test_reftest.c`) renders each test and its references through the
public embedding API at 800x600 and compares the pixels, honouring `<meta name=fuzzy>`. Tests come from a vendored subset
of web-platform-tests in `tests/wpt/` (at the revision in `tests/wpt/COMMIT`) and a few of our own in `tests/reftest/ours/`.
The lists are `tests/wpt/reftests.txt` and `tests/reftest/ours/reftests.txt`.

```sh
build/tests/test_paint_reftest                    # the smoke subset: every 40th test, plus our own (what ctest runs)
build/tests/test_paint_reftest --all              # every test
build/tests/test_paint_reftest --only css/CSS2/abspos/x.xht --verbose
build/tests/test_paint_reftest --filter css-grid  # tests whose id contains the text
build/tests/test_paint_reftest --shot <id>        # render one page to <out>/shot.bmp
```

`--threads N` (or `EMBER_REFTEST_THREADS`) sets the worker processes. A crash or hang costs one test and is recorded as
`CRASH` or `TIMEOUT`. Failure images (`.test`, `.ref`, `.diff` BMPs) go to `build/tests/reftest-out/` or the directory in
`EMBER_REFTEST_OUT`; `failures.txt` there lists every failing test.

### Expectations

`tests/reftest/expectations/<wpt-dir>.txt` has one line per known failure:

```
<test id> <reason tag> [note]
```

A failing test must be listed, a listed test must fail (a fixed bug has to be deleted from its file), ids are unique, and the
`bug` entries of a directory are at most 3% of its tests (checked by `--all`). Reason tags include `not-yet` (feature not
implemented, the note names it), `multicol`, `writing-mode`, `r3-effects` (shadows, filters, masks, clip-path, blend modes),
`font-metrics` (the default font's metrics differ from the font the reference was drawn with), `out-of-scope`, `needs-http`,
`platform` (passes on some GPU drivers, fails on others; either result is accepted), `layout`, `paint`, and `bug` for an
unclassified defect. `tests/reftest/expectations/README.md` explains the tags.

To regenerate after a full run: `test_paint_reftest --all`, then
`python tools/wpt/classify_reftests.py build/tests/reftest-out/failures.txt`. A test that already has a line keeps its tag
and note.

`tools/wpt/select_reftests.py` picks the tests from a WPT checkout (skipping tests that need scripts, `reftest-wait`, vertical
writing modes, print media, iframes and the like), and `tools/wpt/sync.sh` is the helper for refreshing the vendored subset.

## WPT testharness.js tests

`test_bindings_wpt_th` (`tests/unit/bindings/test_wpt_th.c`) runs the testharness tests listed in `tests/wpt/testharness.txt`
in an offscreen view and collects the subtest results through the console callback. The results page and the harness report
script are in `tests/wpt-th/`.

```sh
build/tests/test_bindings_wpt_th               # smoke subset: every 20th test (ctest)
build/tests/test_bindings_wpt_th --all         # everything, and checks the expectations
build/tests/test_bindings_wpt_th --only dom/nodes/Node-appendChild.html
build/tests/test_bindings_wpt_th --filter dom/events --threads 2
```

Expectations are in `tests/wpt-th/expectations/<dir>.txt`, one line per failing test (`<test path> <reason> [note]`) or per
failing subtest (`<test path> :: <subtest name> :: <reason> [note]`). As with reftests, a listed test that passes is an error.
`tests/wpt-th/README.md` has the verdict rules, the reasons and the regeneration script
(`tools/wpt-th/gen_expectations.py`).

## test262

The ECMAScript conformance suite is a submodule at `third_party/test262`. The runner is `tools/test262`:

```sh
cmake --build build --target test262
build/tools/test262/test262 --smoke                  # the fixed sample of tests/test262/smoke-list.txt (what ctest runs)
build/tools/test262/test262                          # a full run of test/language and test/built-ins
build/tools/test262/test262 --filter test/built-ins/Array
build/tools/test262/test262 --list FILE --verbose
build/tools/test262/test262 --update                 # rewrite the expectations for what this run covered
```

Each test runs in sloppy and strict mode. `tests/test262/expectations.txt` lists the variants known to fail (`<path> sloppy|strict
[flaky]`), and `tests/test262/unsupported-features.txt` lists features whose tests are skipped. The runner exits 1 for a new
failure, an unexpected pass, or an expectation for a variant that no longer runs, and 77 when the submodule is missing. A
watchdog reports a variant that makes no progress as a timeout. Options include `--threads`, `--gc-stress N` (collect before
every N-th allocation), `--generational` and `--rope-min N`.

## Other test data

- html5lib: tree-construction `.dat` files (in `tests/wpt/html/syntax/parsing/`) and tokenizer tests
  (`third_party/html5lib-tests`) run as unit tests under `tests/unit/html/`.
- Framework apps: `tests/apps/` holds unmodified Vite builds of Preact, React, Vue, Svelte and Solid TodoMVC apps, a React
  Router app and a vanilla app. `test_embed_apps` loads each `dist/` through `emb_runtime_mount_dir` and plays a scripted
  scenario (clicks, typing, filters, a golden-image check). See `tests/apps/README.md`. The sources and lock files are there so
  the builds can be reproduced with `tools/apps/build.sh`.
- Layout checks: `tests/layout/` holds layout expectations for the check-layout tests from WPT.

## Fuzzers

`tests/fuzz/` has libFuzzer harnesses for the HTML, CSS and JavaScript parsers, the regular expression engine, the URL parser,
MIME, encoding, structured clone, import maps, DEFLATE, Brotli, WOFF, fonts and image decoders. They need clang on Linux and are
off by default:

```sh
CC=clang cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMBER_FUZZ=ON \
   -DEMBER_TESTS=OFF -DEMBER_EXAMPLES=OFF -DEMBER_SLANGC=/path/to/slangc
cmake --build build/fuzz --target fuzz_url
ASAN_OPTIONS=detect_stack_use_after_return=0 \
  build/fuzz/tests/fuzz/fuzz_url -max_len=2048 -dict=tests/fuzz/dict/url.dict corpus-dir tests/fuzz/corpus/url
```

`tests/fuzz/README.md` lists the targets and their input formats. `tools/fuzz/build.sh` and `tools/fuzz/run.sh` wrap the
build and a timed run. A crash that is fixed gets a regression test in the module's unit tests
(for example `tests/unit/html/test_fuzz_regress.c`).

## Benchmarks

Every `tools/bench/bench_<name>.c` is built as the executable `bench_<name>` in `build/tools/bench/` (not in a shared-library
build). CTest does not run them. Use a Release build for numbers.

| Benchmark | Measures |
|---|---|
| `bench_html` | Tokenizer and tree-builder throughput and memory per node. |
| `bench_style` | Style resolution time and computed-style size. |
| `bench_layout` | Layout. |
| `bench_incr` | Incremental layout against full layout. |
| `bench_vblock` | The value-block allocator used by animations. |
| `bench_render` | Renderer frame cost. |
| `bench_anim` | Frame cost of an animating page against the 60 fps budget. |
| `bench_view` | The embedding budgets: first paint, scrolling, memory per view, library size. `--app tests/apps/react-todomvc/dist --todomvc todos-react` measures a real app, and `--baseline tools/bench/budget_baseline.txt --key <key>` compares against recorded numbers. |
| `bench_js_compile`, `bench_js_es6`, `bench_js_suite` | The JavaScript compiler and interpreter. |

`tools/bench/js/compare.sh` compares the engine with QuickJS-ng on shared workloads and `tools/bench/js/micro.sh` runs
micro-benchmarks. `tools/libsize.sh` reports the size of `libember` by module on Linux.

## Adding a test

1. Unit test: create `tests/unit/<module>/test_<name>.c` with `emb_test.h`. No CMake edit is needed; re-run CMake (the glob
   uses `CONFIGURE_DEPENDS`). Test the module's behaviour including bad input. A new module also needs `src/<module>/`,
   `src/<module>/deps.txt` and its own `tests/unit/<module>/` directory.
2. A rendering regression: use `emb_gold_check` with a new golden (`EMBER_UPDATE_GOLDENS=1`), or add a reftest pair under
   `tests/reftest/ours/` (a test page and a `-ref.html`) and list it in `tests/reftest/ours/reftests.txt`.
3. A bug that WPT covers: remove its line from the expectation file once it passes. The runners fail on a listed test that
   passes.
4. A parser or decoder bug found by fuzzing: add the reproducer as a unit test.
5. Network tests bind and connect to `127.0.0.1` only (see `CONTRIBUTING.md`).
6. Run the module's tests, then `ctest -j2`, before sending the change.
