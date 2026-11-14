# Fuzzing

libFuzzer harnesses for every parser in Ember. They need clang (libFuzzer is part of its runtime), so
they build and run on Linux only. The normal build is unchanged: `EMBER_FUZZ` is off by default, and
nothing in this directory is built without it.

## What is here

| Path | Contents |
|---|---|
| `fuzz_<target>.c` | One harness per target: a single `LLVMFuzzerTestOneInput` (plus `LLVMFuzzerInitialize` in `fuzz_clone.c` for the seed dump). The header comment of each file documents its input format. |
| `fuzz_common.h` | Shared helpers: the per-input leak check (`FUZZ_GUARD_BEGIN/END`) and an input reader. |
| `corpus/<target>/` | Small committed seed corpora (about 1 MB in all), made by `tools/fuzz/mkseeds.py` from the repository's own fixtures. |
| `dict/<target>.dict` | libFuzzer dictionaries (HTML, CSS, JS, URL, MIME, import map and encoding tokens), made by `tools/fuzz/mkdicts.py`. |
| `CMakeLists.txt` | Builds every `fuzz_*.c`. A harness names its modules in a `// fuzz-modules: a b c` line on top. |

Scripts are in `tools/fuzz/`: `build.sh`, `run.sh`, `run_all.sh`, `mkseeds.py`, `mkdicts.py`.

## Targets

| Target | Covers |
|---|---|
| `html_doc` | HTML tokenizer and tree builder: whole documents, chunked writes, script pauses, fragment parsing against 32 context elements, byte input through the encoding sniffer, serialize and parse again. |
| `css_sheet` | CSS Syntax entry points, stylesheets with and without the property hooks and an `@import` loader, byte decoding (BOM, `@charset`, UTF-16), the rule tree walked into selectors and values. |
| `css_selector` | Selector, relative selector, An+B and nested selector parsing; matching against a fixed document with and without state hooks; the rule index. |
| `css_value` | The value parser of every property (the property comes from the first input byte plus one bit), colors, `calc()` and friends, registered custom property syntax, `var()` substitution, the typed-value parser, logical/physical mapping. |
| `js_parse` | JS lexer (with regex and template rescans), parser and bytecode compiler. Nothing runs. |
| `js_exec` | Execution under a step budget, a wall-clock budget and a heap cap, scripts and ES modules, job queue drained, optional GC stress. |
| `regexp` | RegExp flags, compile, search and match on a short subject, with the backtracking memory cap. |
| `json_num` | `JSON.parse`; decimal parsing cross-checked against glibc `strtod` and the lexer's converter; shortest `Number -> string -> Number` round trip; radix and fixed/exponential/precision formatting. |
| `url` | URL parser (with and without base and query encoding), all getters and setters, IDNA, Punycode, percent decoding, urlencoded forms, `data:` URLs. Checks that parsing is idempotent. |
| `multipart` | The FormData body parser of the bindings (`ldr_formdata_parse`, the path of `Response.formData()`): multipart/form-data (boundary and content type taken from the input, file parts, percent-escaped names) and urlencoded bodies, in a real binding context. |
| `mime_headers` | MIME type parser (serialization must be stable), header names and values, `Headers` under every guard, methods, status text. |
| `encoding` | Every WHATWG decoder (fatal and replacement, BOM, split reads), the encoder, sniffing, the HTML input decoder. Checks that decoders write well-formed UTF-8. |
| `clone` | Structured-clone deserializer on raw bytes, and a re-serialize round trip. |
| `importmap` | Import map parsing and module specifier resolution. |
| `inflate`, `brotli` | DEFLATE/zlib (one-shot and streaming with odd chunk sizes) and Brotli. |
| `woff` | WOFF 1 and WOFF 2, and the font module on whatever they produce. |
| `font` | sfnt/TTC open, then cmap, advances, glyph outlines (glyf, CFF, CFF2, variations), COLR/CPAL, names. |
| `image` | PNG, JPEG and GIF through `embi_image_decode`, and through the image cache. |

Not covered: bitmap font tables (the font module has none), and the style engine (media query
and `@font-face` parsing are only reached through `css_sheet`, not through the style module).

## Build

On a Linux machine with clang 19 or later, CMake 3.28+, Ninja and slangc:

```sh
tools/fuzz/build.sh                     # all harnesses, into ../fuzz-build next to the checkout
tools/fuzz/build.sh fuzz_url fuzz_html_doc    # only these
EMBER_FUZZ_BUILD=/tmp/fz EMBER_SLANGC=/path/to/slangc JOBS=8 tools/fuzz/build.sh
```

By hand, that is:

```sh
CC=clang cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMBER_FUZZ=ON \
   -DEMBER_TESTS=OFF -DEMBER_EXAMPLES=OFF -DEMBER_SLANGC=/path/to/slangc
cmake --build build/fuzz --target fuzz_url
```

`EMBER_FUZZ=ON` implies the sanitizers: every engine module is compiled with
`-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined`, so a UBSan report aborts
and libFuzzer files it as a crash. Each harness links with `-fsanitize=fuzzer`.

## Run

```sh
ASAN_OPTIONS=detect_stack_use_after_return=0 \
  build/fuzz/tests/fuzz/fuzz_url -max_len=2048 -dict=tests/fuzz/dict/url.dict corpus-dir tests/fuzz/corpus/url
```

`detect_stack_use_after_return=0` is required: the JS engine scans the native stack conservatively, and
ASan's fake stacks confuse the scan. The corpus directory is where libFuzzer saves the inputs that add
coverage; the committed seeds are the read-only second directory.

`tools/fuzz/run.sh <target> <seconds>` does that with `-jobs`/`-workers`, a `-max_len` suited to the
target, the dictionary, crash artifacts in `$FUZZ_ROOT/<target>/crashes` (default `FUZZ_ROOT=/opt/ember/fuzz`)
and a summary line appended to `$FUZZ_ROOT/<target>/summary.txt` with the numbers libFuzzer printed
(executions per second, total runs, coverage, new artifacts). `tools/fuzz/run_all.sh <seconds> [target...]`
runs several targets at once within a worker budget (`MAX_WORKERS`, default 16).

```sh
FUZZ_BUILD=$PWD/build/fuzz FUZZ_ROOT=/tmp/fuzz WORKERS=4 tools/fuzz/run.sh html_doc 600
```

The binary formats (`inflate`, `brotli`, `woff`, `font`, `image`) take a file as it is and read their
options from the last byte, so `run.sh` also feeds them the fixtures in `tests/fonts`, `tests/image`,
`tests/data/brotli` and `tests/data/deflate`. The text formats start with one or two mode bytes (see the
comment on top of each harness); `mkseeds.py` writes them.

## Leak checks

Each input must leave `embi_mem_live()` where it found it. The harness drops the global atom table (and
the thread caches that hold atoms) before taking the baseline and again before comparing, so atoms
interned by the input do not count. A difference aborts with `LEAK: n bytes still live`. LeakSanitizer
stays on as well and checks at exit.

## A crash, hang or out-of-memory

libFuzzer writes `crash-*`, `timeout-*`, `oom-*` or `leak-*` into the artifact directory.

```sh
build/fuzz/tests/fuzz/fuzz_url crash-abc                       # reproduce
build/fuzz/tests/fuzz/fuzz_url -minimize_crash=1 -runs=100000 crash-abc   # minimize (writes minimized-from-*)
```

1. Minimize it.
2. Add it as a regression unit test in the module's `tests/unit/<mod>/` as a byte array (no new
   fixture file over 4 KB). Test the behavior (no crash, balanced allocator), not the byte values.
3. Fix it. In a module with another active writer, report it with the minimized reproducer instead.
4. Run the target again for 10 minutes.

## Regenerating seeds and dictionaries

```sh
python tools/fuzz/mkseeds.py --test262 /path/to/test262     # corpus/<target>/ (all but clone)
python tools/fuzz/mkdicts.py                                 # dict/ only
```

The `clone` seeds are streams written by the serializer. Rebuild them on Linux with
`mkdir d && EMBER_FUZZ_DUMP_SEEDS=d build/fuzz/tests/fuzz/fuzz_clone` and copy `d/*` to `corpus/clone/`.
