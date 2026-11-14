# Ember

Ember is a small, embeddable HTML/CSS/JavaScript engine for app and game UIs, written in C23 on
[SDL3](https://libsdl.org). It is not a wrapper around Chromium or a system webview: the HTML parser, CSS engine,
layout, JavaScript engine and GPU renderer are all its own. Use it where you would reach for Electron or CEF and want a
much smaller footprint.

> **Status: preview.** Ember runs real framework apps, but the API is not frozen and the size and speed goals are not all met
> yet. Expect breaking changes. See [Status](#status).

## What it does

- **HTML and DOM:** a WHATWG tokenizer and tree builder, the DOM with events and observers, and forms with IME input.
- **CSS:** selectors level 4, cascade layers, custom properties, media and container queries, `@font-face`, transitions and
  animations. Block, inline, flex, grid, table and positioned layout. Shadows, filters, backdrop-filter, blend modes,
  masks, clip-path and 3D transforms.
- **JavaScript:** its own engine (bytecode VM, shapes and inline caches, garbage collector, RegExp), tested with test262.
  ES modules and import maps, so Vite builds load.
- **Graphics:** a GPU renderer on SDL_gpu (Direct3D 12, Vulkan, Metal), text drawn from glyph outlines, SVG and Canvas 2D.
  Shaders are written in Slang.
- **Text:** OpenType, CFF, variable and colour fonts, WOFF/WOFF2, shaping with kb_text_shape, bidi with SheenBidi, system
  font fallback.
- **Two ways to embed:** *windowed* (Ember opens the window and runs the loop) or *offscreen* (you pass your own
  `SDL_GPUDevice` and get a texture per view to composite into your app or game).
- **Networking:** HTTP/1.1 and TLS for `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource` and remote pages. Nothing is
  reachable until the embedder allows a host. Local content loads from files or from your own resource handlers (for
  example `app://`), which also answer `fetch()`.

Not included: Workers, WebAssembly, IndexedDB, shadow DOM, custom elements, iframes, WebGL, media playback and
vertical text layout. See [Web platform support](docs/web-platform.md) for the full list.

## Building

You need GCC 15+ or Clang 19+ (C23 and `#embed`), CMake 3.28+, Ninja, and
[slangc](https://github.com/shader-slang/slang/releases) on `PATH` (or `-DEMBER_SLANGC=<path>`). On Windows the DXIL compiler
from the Windows SDK is found automatically.

```sh
git clone --recursive https://github.com/Tebarem/ember
cd ember
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

[docs/building.md](docs/building.md) has platform notes (Windows, Linux, macOS), every CMake option and the size-optimised
build (`-DEMBER_MIN_SIZE=ON`).

## Trying it

```sh
build/examples/viewer path/to/index.html
build/examples/viewer path/to/vite-app/dist      # a directory is served as app://local/
build/examples/viewer --net-allow example.com https://example.com/   # a remote page
```

`examples/offscreen` renders a view into a texture of the host's own GPU device. [`examples/vue-app`](examples/vue-app) ships a
Vue 3 and TanStack Query app as one executable: the Vite build is embedded with `#embed` and the app's `fetch('api/...')`
calls are answered by the host in C.

## Embedding

The public API is in [`include/ember`](include/ember). A windowed view:

```c
#include <ember/ember.h>

int main(void)
{
    emb_runtime_desc rd = {.size = sizeof rd};
    emb_runtime *rt = emb_runtime_create(&rd);

    /* Serve a built app directory as app://local/ */
    emb_runtime_mount_dir(rt, "app", "local", "dist");

    emb_view_desc vd = {.size = sizeof vd, .mode = EMB_VIEW_WINDOWED, .width = 1024, .height = 768, .title = "My app"};
    emb_view *view = emb_view_create(rt, &vd);
    emb_view_load_url(view, "app://local/index.html");
    emb_view_run(view); /* returns when the window is closed */

    emb_view_destroy(view);
    emb_runtime_destroy(rt);
    return 0;
}
```

The [embedding guide](docs/embedding.md) covers offscreen rendering, input, resource handlers, calling scripts from C,
networking and its security model, threading and ownership.

## Status

Ember is a preview, built and tested on Windows, Linux and macOS (macOS is less exercised).

- Goals: library under 3.2 MiB in the size configuration, under 15 MB per idle view, first paint under 20 ms, a steady 60 fps.
  [Architecture](docs/architecture.md#budgets) says how each is measured and which are not met yet.
- test262: every variant of `test/language` and `test/built-ins` that is not skipped for an unsupported feature passes,
  except 4. Intl is a minimal en-US subset.
- Layout and render reftests (a subset of web-platform-tests run by Ember's own runner): 10,355 of 12,098 pass (85.6%), from a
  full run at commit `7e80abf3`.
- html5lib tree construction: at least 99% pass (the unit test enforces it).

## Documentation

| | |
|---|---|
| [docs/building.md](docs/building.md) | Requirements, platforms, CMake options, size build, examples |
| [docs/embedding.md](docs/embedding.md) | The C API: runtime, views, rendering, input, resources, scripts, networking |
| [docs/web-platform.md](docs/web-platform.md) | Supported and unsupported web features, conformance numbers |
| [docs/architecture.md](docs/architecture.md) | Modules, pipeline, threading, budgets |
| [docs/testing.md](docs/testing.md) | Tests, conformance suites, fuzzers, benchmarks |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Code rules and how to submit changes |

## License

Ember is released under the [MIT License](LICENSE). Third-party code, data and test suites keep
their own licenses; see [`third_party/NOTICE`](third_party/NOTICE).
