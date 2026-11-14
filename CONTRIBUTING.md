# Contributing to Ember

Thanks for your interest. Ember is a preview and the API is not frozen, so it helps to open an issue before starting
anything large. By contributing you agree that your work is released under the project's [MIT License](LICENSE).

## Getting set up

Build and test as described in [docs/building.md](docs/building.md) and [docs/testing.md](docs/testing.md). The short form:

```sh
git clone --recursive https://github.com/Tebarem/ember
cd ember
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -j2 --output-on-failure
```

You need GCC 15+ or Clang 19+ (C23 with `#embed`), CMake 3.28+, Ninja and `slangc`. [docs/architecture.md](docs/architecture.md)
is a map of `src/`.

## Code rules

- **C23 only.** The compiler is GCC 15+ or Clang 19+ (`#embed` is required). No C++ anywhere, including tools and tests. The
  public headers in `include/ember/` are C23 too.
- **Platform access goes through SDL3 only** (SDL_gpu, SDL_storage, SDL_iostream, SDL_thread, and so on). No direct OS APIs
  (no Win32, POSIX or Cocoa calls) in the library.
- **Shaders are Slang**, in `shaders/`. They are compiled by `slangc` at build time and embedded with `#embed`.
- **Naming:** `emb_` for the public API (declared with `EMB_API` in `include/ember/`), `embi_` for internal symbols shared across
  modules, `static` for everything else. A new `emb_*` function must be declared in a public header: CI checks that the
  exported symbols of the shared library are exactly the `EMB_API` declarations.
- **Warnings are errors.** Ember targets build with `-std=c23 -Wall -Wextra -Werror` plus `-Wshadow -Wstrict-prototypes
  -Wmissing-prototypes -Wvla -Wimplicit-fallthrough` (`cmake/EmberWarnings.cmake`). Keep sanitizer builds clean too
  (`-DEMBER_SANITIZE=ON`, ASan and UBSan).
- **Every module has tests** under `tests/unit/<module>/`. A bug fix comes with a test that fails without it. A change that
  makes a conformance test pass must delete its line from the expectation file (the runners fail on a listed test that
  passes). See [docs/testing.md](docs/testing.md#adding-a-test).
- **Modules and dependencies:** a module under `src/<module>/` lists the modules it uses in `deps.txt`. Do not add a dependency
  to get around a layering problem without discussing it.
- **Generated files** (`src/bindings/gen/`, `src/css/props.inc`, Unicode and entity tables) are checked in. Change the source
  (WebIDL files in `src/bindings/idl/`, `src/css/props.txt`, the data under `tools/*/data`) and re-run the generator target
  of the matching tool in `tools/`. Do not edit generated files by hand.
- **Budgets.** Ember aims for a library under 3.2 MiB in the size configuration, under 15 MB per idle view, a first paint under
  20 ms and a steady 60 fps. A change that adds a lot of code, memory or per-frame work should say what it costs.
  `tools/bench` has benchmarks (see [docs/architecture.md](docs/architecture.md#budgets)).
- **Comments** explain why, not what. Keep them accurate when code changes. Do not cite documents that are not in the repository.

## Third-party code

The library may use only: SDL3, SDL_image (JPEG and GIF only), kb_text_shape, SheenBidi, and for networking SDL3_net, mbedTLS
and Mozilla's root certificate list (`third_party/cacert`). Test-only data (WPT, test262, html5lib-tests) lives under `tests/`
and `third_party/`. **Any new dependency, or vendored code, needs approval from a maintainer first**; open an issue. Licenses of
included material are listed in `third_party/NOTICE`.

## Network code and tests

Tests, fixture servers and probes that use the network bind and connect to `127.0.0.1` only, never `0.0.0.0` and never an
outside host. CI runs on machines you do not control.

## Submitting changes

1. Fork the repository and make a branch from `main`.
2. Keep changes focused: one logical change per pull request, in commits that each build and pass tests.
3. Build with `-DEMBER_SANITIZE=ON` and run `ctest` before you push. If you touched layout, style or paint, also run the
   reftests (`build/tests/test_paint_reftest --all`) and compare the number of failures with and without your change. If you
   touched JavaScript, run the test262 runner (`build/tools/test262/test262`). Both are described in
   [docs/testing.md](docs/testing.md).
4. Open a pull request against `main` with a description of what changed and why, and what you ran. CI builds on Windows
   (MSYS2 GCC), Linux (GCC 15, Clang 19, Clang 19 with ASan and UBSan) and macOS (Homebrew LLVM) and must pass.
5. Be ready for review comments. Maintainers may ask for tests, smaller commits or a different approach.

## Reporting bugs

Open an issue with the platform and compiler, the GPU or driver (`SDL_GetGPUDeviceDriver`), a minimal page that shows the
problem, what you expected and what happened, and the log output (pass a `log` callback in `emb_runtime_desc`, or watch
stderr in the viewer). For a security problem, do not open a public issue: use GitHub private vulnerability reporting (the repository's Security tab, then "Report a vulnerability").
