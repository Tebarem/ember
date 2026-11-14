# Building Ember

## Requirements

| Need | Details |
|---|---|
| C compiler | C23 with `#embed`: GCC 15 or newer, or Clang 19 or newer. MSVC is not supported as the compiler (the build has a few MSVC branches for clang-cl, but CI does not cover them). |
| CMake | 3.28 or newer (`cmake_minimum_required(VERSION 3.28)`). |
| Build tool | Ninja is what CI and the examples below use. |
| `slangc` | The [Slang](https://github.com/shader-slang/slang/releases) shader compiler. The shaders in `shaders/` are compiled at build time and embedded with `#embed`. CI uses v2026.10.2. Put it on `PATH` or pass `-DEMBER_SLANGC=<path>`; configuring stops with an error if it is missing. |
| DXIL compiler (Windows) | `dxcompiler.dll` and `dxil.dll` from the Windows SDK, which `slangc` uses to produce Direct3D 12 shaders. Found automatically under `Windows Kits/10`, or pass `-DEMBER_DXC_DIR=<dir>`. |
| Git submodules | All submodules of `third_party/` are required: SDL3, SDL_image, SDL_net, mbedTLS, kb_text_shape, SheenBidi, and the test data (test262, html5lib-tests). Clone with `--recursive`. |
| Python 3 | Only for the maintenance scripts under `tools/wpt`, `tools/wpt-th` and `tools/fuzz`, not for the build. |

Running Ember needs a GPU driver that SDL_gpu supports: Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS. On Linux
without a GPU, Mesa llvmpipe works (CI runs the GPU tests on it with `SDL_VIDEO_DRIVER=offscreen`).

## Getting the source

```sh
git clone --recursive https://github.com/Tebarem/ember
cd ember
```

If you cloned without `--recursive`, run `git submodule update --init`. All submodules are required.

Vendored code: `third_party/SDL` is a submodule of a fork of libsdl-org/SDL at github.com/Tebarem/SDL (the other
submodules point at their upstream repositories; see `.gitmodules` and `third_party/NOTICE`). `third_party/cacert` is curl's
extract of Mozilla's CA root list (`cacert.pem`, see `third_party/cacert/README`). To build against another SDL checkout
pass `-DEMBER_SDL_DIR=<path>`.

## Configure and build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build type defaults to Release. The library (`libember.a`, or `libember.so` / `ember.dll` with `-DEMBER_SHARED=ON`) goes
to `build/`, the examples to `build/examples/`, the tests to `build/tests/` and the tools to `build/tools/`.

### Windows

CI builds with the MSYS2 UCRT64 toolchain (`mingw-w64-ucrt-x86_64-gcc`, `-cmake`, `-ninja`) in an MSYS2 shell. Any GCC 15+
or Clang 19+ that has C23 and `#embed` and works with CMake and Ninja should do. Install the Windows SDK (for the DXIL
compiler) and put `slangc` on `PATH`, or pass its path:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEMBER_SLANGC=/c/tools/slang/bin/slangc.exe
cmake --build build
```

The renderer uses the Direct3D 12 backend of SDL_gpu. The build produces DXIL and SPIR-V shaders (DXIL only in a size
build, which leaves Vulkan out of SDL).

### Linux

CI installs these packages on Ubuntu 24.04 (`gcc-15` comes from the `ubuntu-toolchain-r/test` PPA):

```sh
sudo apt-get install -y cmake ninja-build pkg-config binutils gcc-15 clang-19 libclang-rt-19-dev llvm-19 \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libdbus-1-dev \
  libudev-dev libvulkan-dev mesa-vulkan-drivers
CC=gcc-15 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Shaders are built as SPIR-V, the format of the Vulkan backend.

### macOS

Apple's clang is too old for `#embed`. CI installs Homebrew LLVM and puts it first on `PATH`:

```sh
brew install llvm ninja
export PATH="$(brew --prefix llvm)/bin:$PATH"
export SDKROOT="$(xcrun --show-sdk-path)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Shaders are built as Metal Shading Language. macOS is in the CI matrix (macos-15, Apple silicon) but is less exercised than
Windows and Linux.

## CMake options

Pass options as `-DNAME=value`. "Size build" means `EMBER_MIN_SIZE=ON`.

### General

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | Set to Release when empty. |
| `EMBER_SHARED` | OFF | Build `libember` as a shared library. It exports only the `emb_*` API. The `hello_window` example and the benchmarks use internal symbols and are not built in a shared build. |
| `EMBER_TESTS` | ON | Build the unit tests (`tests/`), the benchmarks (`tools/bench`) and the test262 runner. |
| `EMBER_EXAMPLES` | ON | Build the examples. |
| `EMBER_SANITIZE` | OFF | AddressSanitizer and UBSan on every Ember target (not on SDL). |
| `EMBER_FUZZ` | OFF | Build the libFuzzer harnesses of `tests/fuzz`. Clang only; implies `EMBER_SANITIZE`. |
| `EMBER_NET` | ON | The network stack: HTTP/1.1 and TLS over SDL_net and mbedTLS with Mozilla's root certificates, for `fetch`, XMLHttpRequest, WebSocket, EventSource and remote pages. OFF leaves all of it out, and the `emb_runtime_net_*` functions return false. |
| `EMBER_SDL_DIR` | `third_party/SDL` | SDL3 source directory. |
| `EMBER_NET_SERVER` | same as `EMBER_TESTS` | Build the server half of mbedTLS, which only the network tests' TLS server needs. `-DEMBER_NET_SERVER=OFF` still builds the library and examples; the network tests then do not link. Only has an effect with `EMBER_NET`. |
| `EMBER_SDL_EXTERNAL` | OFF | Shared library only: leave SDL3 and SDL_image out of `libember` and resolve them from the host's SDL3 at load time. Avoids two SDL copies in one process. |

### Optional features

Each is ON by default and OFF by default in a size build. An explicit `-DEMBER_X=ON` keeps the feature in a size build.

| Option | Off means |
|---|---|
| `EMBER_CANVAS` | No canvas 2D context: `canvas.getContext()` returns `null`, the canvas interfaces do not exist, and a `<canvas>` is an empty box (300x150 by default). About 130 KB. |
| `EMBER_SVG` | No SVG: `<svg>` renders nothing and takes no space, the SVG interfaces do not exist, and SVG images in `<img>` and CSS `url()` do not decode. |
| `EMBER_DEFAULT_FONT` | No bundled default font (`assets/fonts/ember-default.ttf`, 216 KB). Text uses system fonts and the fonts the host registers with `emb_runtime_add_font`. With no font at all nothing is drawn and the runtime logs one warning. |
| `EMBER_NO_DEFAULT_FONT` | Deprecated; the same as `EMBER_DEFAULT_FONT=OFF`. |

### Shaders

| Option | Default | Meaning |
|---|---|---|
| `EMBER_SLANGC` | empty (search `PATH`) | Path to `slangc`. |
| `EMBER_DXC_DIR` | empty (auto-detect) | Directory with `dxcompiler.dll` and `dxil.dll` (Windows). |
| `EMBER_SHADER_FORMATS` | `spirv` (Linux and others), `msl` (Apple), `spirv;dxil` (Windows; `dxil` in a size build) | Shader formats to build, any of `spirv;dxil;msl`. |
| `EMBER_SPIRV_OPT` | `-O0` in a size build, `-O2` otherwise | `slangc` optimisation level of the SPIR-V blobs. |

### Size build

| Option | Default | Meaning |
|---|---|---|
| `EMBER_MIN_SIZE` | OFF | Optimise Ember and SDL for size: `-Oz` (`/O1` on MSVC), unused-section removal, LTO, and the parts of SDL that Ember does not use left out (on Windows only Direct3D 12 stays). Also changes the defaults of the optional features and of `EMBER_COMPRESS_ASSETS`, as their rows say. |
| `EMBER_SIZE_OPT` | `-Oz` on Clang and GCC 12+, else `-Os` | The size flag. |
| `EMBER_LTO` | ON | With `EMBER_MIN_SIZE`: link with LTO. `tools/libsize.sh` turns it off to get a per-module linker map. |
| `EMBER_LTO_FULL` | OFF | With clang LTO: full LTO instead of ThinLTO (about 1% smaller, slower JavaScript compile, single-threaded link). |
| `EMBER_OUTLINE` | same as `EMBER_MIN_SIZE` | With clang: the machine outliner (about 0.9% smaller). Not used in Debug, RelWithDebInfo or sanitizer builds. |
| `EMBER_SPEED_SOURCES` | the JavaScript front end: `js/scope.c`, `js/compiler.c`, `jslex/jslex.c`, `js/js_atom.c` (plus `js/parser.c` with GCC) | Sources compiled with `EMBER_SPEED_OPT` instead of the size flag, because JavaScript compilation is on the first-paint path. Paths relative to `src/`. |
| `EMBER_SPEED_OPT` | `-O2` | The flag for those sources. |
| `EMBER_COMPRESS_ASSETS` | same as `EMBER_MIN_SIZE` | Store shader blobs, prelude scripts, the UA style sheet, the Brotli dictionary and RegExp property data (and, with `EMBER_NET`, the root certificates as DER) compressed, and expand them where used. |
| `EMBER_RELR` | ON in a size build on Linux, else OFF | Pack relative relocations in the shared library (`-z pack-relative-relocs`). Needs glibc 2.36 or newer (musl 1.2.4) at run time. |
| `EMBER_UNWIND_TABLES` | OFF | In a size build on ELF, Release builds drop the unwind tables (0.79 MB). A crash reporter or profiler that walks the stack with `.eh_frame` then loses the frames inside Ember, and a C++ exception thrown from a host callback terminates the process. ON keeps them. |

## The size-optimised build

```sh
cmake -S . -B build-min -G Ninja -DCMAKE_BUILD_TYPE=Release -DEMBER_MIN_SIZE=ON -DEMBER_NET=OFF
cmake --build build-min
```

`EMBER_MIN_SIZE` trades speed for size. When it was added, style, layout and display-list building were measured at about 15
to 45 percent slower than in the default build (the comments in `CMakeLists.txt` record this), and the `vue_app` example
shrank from 10.9 MB to 6.0 MB on Windows (stripped, MinGW GCC; 10.1 MB to 5.5 MB without the network stack).

The size budget (library under 3.2 MiB, stripped, excluding SDL3) is measured with `EMBER_MIN_SIZE=ON`, `EMBER_NET=OFF`,
`EMBER_SHARED=ON` and `EMBER_SDL_EXTERNAL=ON`, so the figure is Ember's own code. CI builds exactly that configuration on
Linux with GCC 15, checks that the exported symbols are exactly the `EMB_API` functions of the headers, and reports the
size. The size gate (+5% over a line in `tools/bench/budget_baseline.txt`) is not enabled yet because the GCC 15 baseline
line has not been recorded. `tools/libsize.sh` produces the size reports (Linux only). See
[Architecture](architecture.md#budgets).

## Running the examples

The examples are in `build/examples/` (add `.exe` on Windows).

| Example | What it shows |
|---|---|
| `viewer` | A windowed view. `viewer page.html`; `viewer path/to/dist` (a directory is served as `app://local/`); `viewer --dark --width 1280 --height 800 page.html`; `viewer --net-allow example.com https://example.com/` for a remote page; `viewer --startup-trace page.html` prints startup timings. |
| `offscreen` | An offscreen view in a window the host owns: the host creates the GPU device, calls `emb_view_render` each frame and blits the view's texture next to a spinning quad of its own. `offscreen [--frames N] [PATH_OR_URL]`. |
| `vue_app` | A Vue 3 and TanStack Query app shipped as one executable. The Vite build is embedded with `#embed`, and the page's `fetch('api/...')` calls are answered in C. See `examples/vue-app/README.md`. The built `dist/` is committed, so npm is not needed. |
| `hello_window` | A smoke test of the renderer (a Slang triangle). It uses internal symbols and is built only against the static library. It is not an embedding example. |

`viewer` and `vue_app` use only the public API. `offscreen` also includes an internal header to load the shader blob of its
own quad.

## Running the tests

```sh
ctest --test-dir build -j2 --output-on-failure
```

Tests that need a GPU device skip (exit code 77) when none is available. See [Testing](testing.md).
