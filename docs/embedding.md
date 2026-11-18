# Embedding Ember

This guide covers the C API in `include/ember/`. The reference is the headers themselves, which document every function:

| Header | Contents |
|---|---|
| `ember.h` | Runtime, views, rendering, fonts, statistics. Includes the other four. |
| `ember_input.h` | Pointer, touch, keyboard, text input, focus, clipboard, navigation hooks. |
| `ember_resource.h` | Resource handlers (v2: methods, headers, status codes, async), `localStorage` persistence, `emb_runtime_mount_dir`. |
| `ember_script.h` | Evaluating JavaScript from C, the console callback, posting tasks. |
| `ember_net.h` | The host allowlist, extra root certificates, the User-Agent. |

The headers are C23 (`nullptr`, `bool`) and need no SDL include. The API is not frozen. The version is
`EMB_VERSION_STRING`, also `emb_version_string()` and `emb_version()`.

Link `libember` and, for a static library, SDL3 (the build tree has the `ember` target and `SDL3::SDL3-static`). The
examples in `examples/` are the working references: `viewer/main.c` (windowed), `offscreen/main.c` (host-owned device),
`vue-app/main.c` (a web app with a native API).

## Model

- An `emb_runtime` holds what views share: the font registry, the text system, resource handlers, allowed file directories,
  the network service and `localStorage` storage.
- An `emb_view` is one document with its own style, layout, display list, image cache, JavaScript realm and GPU state. A
  runtime outlives its views.
- Sizes and positions are CSS px. A device pixel is one CSS px times the view's scale.
- Structs passed in start with a `size` field: set it to `sizeof` the struct. The library does not yet check it, so build
  against the same headers as the library.

## Lifecycle

```c
#include <ember/ember.h>
#include <string.h>

int run_windowed(void)
{
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    emb_runtime *rt = emb_runtime_create(&rd);          /* rd may also be nullptr */
    if (rt == nullptr) {
        return 1;
    }

    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_WINDOWED;
    vd.width = 1024;                                     /* CSS px; 0 means 800 */
    vd.height = 768;                                     /* 0 means 600 */
    vd.title = "My app";
    emb_view *view = emb_view_create(rt, &vd);
    if (view == nullptr) {
        emb_runtime_destroy(rt);
        return 1;
    }

    int status = 0;
    if (!emb_view_load_url(view, "file:///path/to/index.html") || !emb_view_run(view)) {
        status = 1;                                      /* emb_view_run returns when the window closes */
    }

    emb_view_destroy(view);                              /* views first ... */
    emb_runtime_destroy(rt);                             /* ... then the runtime */
    return status;
}
```

- `emb_runtime_create` and `emb_view_create` return `nullptr` on failure and log the reason.
- Destroy every view before its runtime.
- A runtime can serve several views. Add fonts and handlers before creating the views that need them: fonts added with
  `emb_runtime_add_font` are used by views created later and by documents loaded later.
- `emb_runtime_desc` fields: `image_cache_bytes` (decoded-image budget per view, 0 means 64 MiB), `log` and `log_user` (a log
  sink), `flags` (`EMB_RUNTIME_NO_SYSTEM_FONTS`; `EMB_RUNTIME_NO_DEFAULT_FONT` is not supported and makes creation fail).
- Allocation goes through SDL. There are no allocator hooks in the descriptor; call `SDL_SetMemoryFunctions` before
  `emb_runtime_create` if you need your own.

## Rendering

A view has two modes.

### Windowed

Ember creates the SDL window and its own GPU device. The window shows once the first frame is presented.

- `emb_view_run(view)` runs the event loop until the window closes. It returns true on a normal close.
- `emb_view_pump(view)` handles pending events, updates and presents once, waits briefly if idle, and returns false when
  the window has closed. Use it when you own the loop:

```c
#include <ember/ember.h>

bool run_with_own_loop(emb_view *view)
{
    while (emb_view_pump(view)) {
        /* your per-frame work */
    }
    return true;
}
```

The window's display scale decides the view's scale. `emb_view_resize` resizes the window. A windowed program that uses a
plain `main()` needs `#define SDL_MAIN_HANDLED` before including SDL headers (see `examples/viewer/main.c`).

### Offscreen

You own the SDL window and the `SDL_GPUDevice`. The view renders into a texture it owns, and you composite that texture.

```c
#include <ember/ember.h>
#include <SDL3/SDL.h>
#include <string.h>

/* dev is your SDL_GPUDevice; it must outlive the view. */
emb_view *make_offscreen_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 640;
    vd.height = 588;
    vd.scale = 1.0f;                                    /* device px per CSS px; 0 means 1 */
    vd.device = dev;
    return emb_view_create(rt, &vd);
}

/* One frame: the view records into cmd only when its picture changed. */
bool draw_frame(SDL_GPUDevice *dev, SDL_Window *win, emb_view *view)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(view, cmd);           /* style, layout, paint, damage, GPU commands */
    (void)changed;
    SDL_GPUTexture *swapchain = nullptr;
    uint32_t w = 0, h = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swapchain, &w, &h) && swapchain != nullptr) {
        SDL_GPUBlitInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.source.texture = emb_view_texture(view);      /* fetch it every frame: a resize replaces it */
        bi.source.w = 640;
        bi.source.h = 588;
        bi.destination.texture = swapchain;
        bi.destination.w = 640;
        bi.destination.h = 588;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &bi);
    }
    SDL_SubmitGPUCommandBuffer(cmd);                     /* you submit; Ember never does in this mode */
    return true;
}
```

- `emb_view_render` returns true when the picture changed. The first call draws the whole view. A frame that changed
  nothing records no GPU work, so you can skip your own composite when it returns false and your target keeps its content.
- While an animation or a smooth scroll runs, `emb_view_render` keeps returning true: keep calling it every frame.
- `emb_view_texture` returns the view's texture. Any size or scale change that alters the pixel size replaces it, so fetch
  it again after `emb_view_resize` or `emb_view_set_scale`.
- `emb_view_set_scale`, `emb_view_set_color_scheme` (feeds `prefers-color-scheme`), `emb_view_set_text_aa` and
  `emb_view_set_text_hinting` repaint the view.
- Text hinting is chosen per view with `emb_view_desc.text_hinting` (or `emb_view_set_text_hinting`):
  `EMB_TEXT_HINTING_NATURAL` (the default) hints vertically under 36 px, snaps baselines and darkens stems;
  `EMB_TEXT_HINTING_SHARP` does the same and also fits x and rounds the pen x of every glyph to a whole device px, for HUD
  text and pixel fonts; `EMB_TEXT_HINTING_NONE` does no fitting at all (no hinting, no snap, no darkening, as Chrome's
  `text-rendering: geometricPrecision`), with the contrast curve kept. A page can draw one element's text as NONE with
  `text-rendering: geometricPrecision`. A descriptor without the field (the older size) is NATURAL. Canvas text and the
  text of form controls are drawn NATURAL whatever the mode.
- Offscreen text uses grayscale antialiasing in `EMB_TEXT_AA_AUTO`, since you may composite the texture over anything.
- The texture is RGBA8 UNORM by default; `emb_view_desc.format` can make it BGRA8. Colors are premultiplied alpha in gamma
  (sRGB-encoded) space, so composite with `ONE` / `ONE_MINUS_SRC_ALPHA`, and sample the texture as UNORM, not as an `_SRGB`
  texture.
- `examples/offscreen/main.c` is the full loop: window and device creation, event handling, the blit.

### Rendering into your own textures

`emb_view_render_to` draws a view into a rectangle of a texture the host owns, such as an atlas that holds several views.
Ember records into the command buffer you pass and never submits it.

```c
#include <ember/ember.h>
#include <SDL3/SDL.h>
#include <string.h>

/* atlas: an RGBA8 texture of 2048 x 1024 with COLOR_TARGET | SAMPLER usage. left and right are two offscreen views. */
bool draw_two_views(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *atlas, emb_view *left, emb_view *right, bool preserved)
{
    emb_render_target t;
    memset(&t, 0, sizeof t);
    t.size = sizeof t;
    t.texture = atlas;
    t.texture_w = 2048;
    t.texture_h = 1024;
    t.flags = preserved ? EMB_RENDER_PRESERVED : 0;
    t.x = 0;                                             /* the left view's rectangle: top-left at (0, 0) */
    bool a = emb_view_render_to(left, cmd, &t);
    t.x = 1040;                                          /* the right one, with a 32 px gutter between them */
    bool b = emb_view_render_to(right, cmd, &t);
    return a || b;                                       /* true when either recorded a picture */
}
```

- The rectangle is `emb_view_device_size` at (x, y). It must lie inside the texture; a rectangle that does not fit is refused
  (logged, nothing recorded).
- The texture must be in the view's format (`emb_view_desc.format`) and have COLOR_TARGET usage. Only mip level 0 is drawn.
- Ember writes only inside the rectangle, and it never submits. Pass ordering and the submit are yours.
- `EMB_RENDER_PRESERVED` says that the rectangle still holds the view's previous picture: the same texture, x, y and device
  size, and nothing else drawn there since. Then only the damage is drawn again, and the call returns false when nothing
  changed. Without the flag, or when any of those differ from the previous call, the whole rectangle is drawn. Ember keeps
  the record itself, so passing the flag on every call is safe, the first one included and after an error.
- Create a view with `EMB_VIEW_NO_TEXTURE` in `emb_view_desc.flags` when it is drawn only this way. The view then makes no
  texture of its own: `emb_view_texture` returns nullptr, and `emb_view_render` records nothing and returns false.
- A view drawn with `emb_view_render_to` can also be drawn with `emb_view_render`. Each switch between the two draws the
  whole picture once.

### UI in a 3D world

A view drawn on a panel in a 3D scene shows its text at whatever size the panel has on screen. Text is rasterized from its
outlines at the device pixel size, so a panel drawn at its CSS size and then magnified by the GPU looks blurred up close.
`emb_view_set_raster_scale(view, r)` draws the view at `scale * r` device px per CSS px. The document does not see it:
layout, style, `devicePixelRatio` and media queries stay as they are, and only the drawing is redone. The cost is one GPU
redraw of the view each time the scale changes. What the page sizes in device pixels itself keeps that size and is
magnified: a `<canvas>` backing store sized from `devicePixelRatio`, and the snapping of hairline borders.

- Pick `r` from the panel's size on screen: its height in device px over its height in CSS px.
- Quantize `r` and change it only when the wanted value leaves the current step. Each change redraws the whole view, so
  a panel that breathes around a step should keep its scale:

```c
#include <math.h>

/* The raster scale for the next frame. Steps are powers of 2^(1/4); a change needs the wanted value to leave the current
 * step by more than 1/8 octave. max is the largest scale whose device size fits the panel's slot. */
float next_raster(float current, float wanted, float max)
{
    float d = fminf(fmaxf(wanted, 0.25f), max);
    if (fabsf(log2f(d) - log2f(current)) <= 0.125f) {
        return current;                                  /* inside the hysteresis band: keep it */
    }
    return fminf(exp2f(roundf(4.0f * log2f(d)) / 4.0f), max);
}
```

- Minification: a panel drawn smaller than its texture needs mipmaps. A view's own texture gets them with
  `EMB_VIEW_MIPMAPS`, which makes the chain after each changed frame. An atlas gets them from the host: after the views
  drew into it, call `SDL_GenerateMipmapsForGPUTexture` on the atlas when any of them recorded a picture. Mip level 0 must be
  complete first, and the views write only level 0.
- Mips of an atlas mix texels across the edges of neighbouring slots. Leave a gutter of 32 px between slots, and sample only
  the panel's own sub-rectangle (its device size, not the slot's).
- Text in a texture uses grayscale antialiasing, as for any offscreen view. Subpixel (LCD) antialiasing is not used, since the
  panel may be rotated or scaled.
- Input: cast the mouse ray through the inverse of the view-projection matrix, intersect it with the panel's plane, and
  convert the hit to the panel's unit square (u, v in [0, 1]). The view gets `(u * css_width, v * css_height)` as the pointer
  position. Send `emb_view_mouse_leave` to a view that is no longer hit, and send the wheel to the panel under the pointer.

`examples/world_panel` puts these together: two views in one atlas, a perspective camera that moves, dynamic raster scales,
and mouse input through the ray.

### Shared libraries and SDL

A shared `libember` carries its own SDL3. A host that links its own SDL3 and gives an offscreen view its `SDL_GPUDevice` then
runs two SDL copies, which works only if both are the same SDL version. Build with `-DEMBER_SHARED=ON -DEMBER_SDL_EXTERNAL=ON`
to leave SDL out of the library and use the host's. A static `libember` shares the host's SDL as linked.

### Scrolling, resizing, statistics

- `emb_view_scroll_by`, `emb_view_scroll_to` and `emb_view_wheel(view, x, y, dx, dy)`. The wheel call dispatches a `wheel`
  event first and scrolls only if no listener canceled it; the innermost scroll container under the point that can move
  takes it.
- `emb_view_title(view)` is the document title; the pointer is valid until the next load.
- `emb_view_stats_get` fills an `emb_view_stats` (set `size`): parse, style, layout, display list and GPU times, item,
  layer and draw-call counts. `emb_runtime_stats_get` reports the system font index.

## Fonts

Text uses, in order: faces registered with `emb_runtime_add_font` and `@font-face`, fonts installed on the system (indexed on
a background thread, so nothing waits for the index), the embedder's generic-family choices
(`emb_runtime_set_generic_family`), per-character system fallback, and the bundled default font. `EMB_RUNTIME_NO_SYSTEM_FONTS`
(or the environment variable `EMBER_NO_SYSTEM_FONTS`) restricts a runtime to registered and bundled faces, which makes
output machine-independent. `emb_runtime_trim_fonts` frees system faces no layout uses; call it between frames.

In a library built with `EMBER_DEFAULT_FONT=OFF` you must supply a font (a system font or `emb_runtime_add_font`), or no
text is drawn. The first font registered then becomes the last-resort face.

## Loading content

| Call | What it does |
|---|---|
| `emb_view_load_html(view, html, len, base_url)` | Parses UTF-8 HTML. `base_url` resolves relative URLs; `nullptr` or `""` means none. Replaces the document. Returns false if the document could not be laid out. |
| `emb_view_load_url(view, url)` | Loads the document through the handlers (a `file:` URL, a bare path, a scheme with a handler, or `http`/`https` with networking), then as `emb_view_load_html` with `url` as the base. |

Scripts run in both. A view with `no_script = true` in its descriptor runs none and builds `<noscript>` content.

Where URLs come from, for the document and everything it references (stylesheets, images, fonts, scripts, `fetch()`):

1. A resource handler registered for the URL's scheme (`emb_runtime_set_resource_handler`, `..._v2`, `..._async`) or a
   mounted directory (`emb_runtime_mount_dir`).
2. The built-in `data:` loader.
3. The built-in `file:` loader, which reads only under directories you allow with `emb_runtime_allow_path(rt, dir)`
   (absolute path, recursive; a path with a `..` segment is refused).
4. `http` and `https` over the network stack, from hosts you allow (see Networking). A handler registered for `http` or
   `https` wins over the network, which is useful for mocks and offline packs.

`emb_view_load_html` and `emb_view_load_url` for a network page return `true` as soon as the request has started. The
previous page stays, and the new one replaces it when its blocking resources arrive. A failed network load shows an error
page instead of returning false.

### Serving an app from a directory

The shortest way to run a built web app (a Vite `dist/`):

```c
#include <ember/ember.h>

bool open_app(emb_runtime *rt, emb_view *view)
{
    /* app://local/index.html is served from dist/index.html; "/" is index.html. */
    return emb_runtime_mount_dir(rt, "app", "local", "dist") && emb_view_load_url(view, "app://local/index.html");
}
```

The media type comes from the file's extension. A missing file is a 404; a path with `..`, a backslash or a colon is
refused. Use a custom scheme such as `app`; `file`, `data` and `blob` are refused.

### Resource handlers

A v2 handler receives the method, URL, headers and body and fills in a status, media type, headers and body. It serves
documents, subresources and `fetch()`/XMLHttpRequest calls of the page alike. This is the pattern of `examples/vue-app`,
which answers `fetch('api/notes')` in C:

```c
#include <ember/ember.h>
#include <string.h>

static bool serve(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    static const char json[] = "{\"ok\":true}";
    if (req->method_len == 3 && memcmp(req->method, "GET", 3) == 0 && req->url_len > 0) {
        out->status = 200;                                /* 0 also means 200 */
        out->mime = "application/json";
        out->mime_len = strlen(out->mime);
        out->data = (const uint8_t *)json;                /* copied before this function returns */
        out->len = sizeof json - 1;
        return true;
    }
    return false;                                         /* a network-style failure for fetch */
}

void install(emb_runtime *rt)
{
    emb_runtime_set_resource_handler_v2(rt, "app", serve, nullptr);
}
```

- `emb_resource_fn` (v1, in `ember.h`) is the simple form: `bool fn(user, url, url_len, emb_resource *out)`, with `data` and
  `len` filled in; register it with `emb_runtime_set_resource_handler`. A handler registered for a scheme replaces any
  earlier one, v1 or v2. Passing `nullptr` removes it. `file` and `data` cannot be overridden.
- The response bytes are copied before the handler returns, so a handler may reuse its buffer.
- Request strings (`method`, `url`, headers, `body`) are valid only for the call. Copy what you keep.
- Asynchronous handlers (`emb_runtime_set_resource_handler_async`) get a `start` call with an `emb_request *` in
  `req->request` and must call `emb_resource_complete(request, &response)` exactly once, from any thread; Ember marshals the
  result to the view's thread. The optional `cancel` callback is called if the page aborts the request, and you must still
  complete the request once (the result is dropped). Document loads (`<img>`, `<link>`, `<script>`) are synchronous: an
  async handler serves them only if it completes inside `start`.

### Persistent storage

`localStorage` is memory-only unless you call `emb_runtime_set_storage(rt, org, app)` (an SDL user storage container) or
`emb_runtime_set_storage_dir(rt, dir)` before a view runs its first script. Both return false if a backend is already set or
cannot be opened.

### Navigation

A link click, a `location` assignment or a form submission asks the host to navigate, because a view has one document and
no browsing history UI. Install `emb_view_set_navigate_handler(view, fn, user)` to receive the absolute URL, or
`emb_view_set_navigate_ex_handler` to also receive the method, body and content type of a form submission. Typically the
handler calls `emb_view_load_url`. Same-document `#fragment` links are handled by the view itself.

## Input

A windowed view translates SDL events itself. An offscreen host forwards its own events with the functions of
`ember_input.h`. Each call dispatches the DOM events a browser would (pointer, mouse, keyboard, focus, composition) and then
runs the default actions (link and button activation, label forwarding, Tab navigation, scroll keys, clipboard shortcuts)
unless a listener canceled the event. Positions are CSS px of the view.

```c
#include <ember/ember.h>
#include <SDL3/SDL.h>

/* Forward SDL events that fall inside the view, whose top-left corner is at (vx, vy) in the window. */
void forward_event(emb_view *view, const SDL_Event *ev, float vx, float vy)
{
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        emb_view_mouse_move(view, ev->motion.x - vx, ev->motion.y - vy, 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        emb_view_mouse_button(view, ev->button.x - vx, ev->button.y - vy,
                              ev->button.button == SDL_BUTTON_LEFT ? EMB_BUTTON_PRIMARY : EMB_BUTTON_SECONDARY,
                              ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN, 0);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        emb_view_wheel(view, ev->wheel.mouse_x - vx, ev->wheel.mouse_y - vy, ev->wheel.x * 53.0f, -ev->wheel.y * 53.0f);
        break;
    case SDL_EVENT_TEXT_INPUT:
        emb_view_text_input(view, ev->text.text, SDL_strlen(ev->text.text));
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        emb_key_event ke;
        SDL_zero(ke);
        ke.size = sizeof ke;
        ke.down = ev->type == SDL_EVENT_KEY_DOWN;
        ke.repeat = ev->key.repeat;
        ke.key = SDL_GetKeyName(ev->key.key);             /* map to DOM key values such as "Enter" and "ArrowLeft" */
        ke.code = "";
        emb_view_key(view, &ke);
        break;
    }
    default:
        break;
    }
}
```

The key mapping above is a sketch: `emb_key_event.key` expects DOM key values (`"a"`, `"Enter"`, `"ArrowLeft"`) and `code`
the physical key (`"KeyA"`); see `ember_input.h` for the full contract, and `src/platform/keys.c` for how a windowed view
maps SDL keys. Other calls: `emb_view_touch` (down, move, up, cancel per finger id), `emb_view_mouse_leave`,
`emb_view_text_editing` (IME composition), `emb_view_focus` (host focus gained or lost), `emb_view_clipboard` (copy, cut,
paste commands from a menu). Modifier bits are `EMB_MOD_SHIFT`, `EMB_MOD_CTRL`, `EMB_MOD_ALT` and `EMB_MOD_META`.

## Scripts: the bridge between C and JavaScript

C calls into the page with `emb_view_eval`. The page calls into C by making requests that a resource handler answers
(usually `fetch`), and by `console` output that a callback receives. There is no direct "expose a C function to JavaScript"
call; the request/response path is the supported bridge.

```c
#include <ember/ember.h>
#include <stdio.h>

static void on_result(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    printf("%s: %.*s\n", ok ? "result" : "threw", (int)len, text);   /* text is valid only during the call */
}

static void on_console(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)user;
    printf("[console %d] %.*s\n", (int)level, (int)len, text);
}

void script_demo(emb_view *view)
{
    emb_view_set_console_callback(view, on_console, nullptr);
    static const char js[] = "document.title = 'hi'; 6 * 7";
    emb_view_eval(view, js, sizeof js - 1, on_result, nullptr);     /* prints "result: 42" */
}
```

- `emb_view_eval` runs the code as a classic script in the view's realm, then runs the microtasks it queued. The
  completion value (or the exception) goes to `result` as a UTF-8 string before the call returns. It returns false if the code
  threw or the view has no document.
- A view creates its JavaScript realm on the first script, so a static page pays nothing.
- Script errors are logged at `EMB_LOG_ERROR` with a line that starts with `uncaught exception:`, `unhandled rejection:` or
  `console.error:`. `emb_view_set_console_callback` also receives `console` output (`debug`, `log`, `info` as
  `EMB_LOG_DEBUG`/`EMB_LOG_INFO`, `warn`, `error`); the runtime log gets it too.
- `emb_view_post_task(view, fn, user)` queues `fn(user)` on the view's event loop. It runs on the view's thread during the
  next update, in order with timers and input.

### A native API behind `fetch`

The page uses ordinary `fetch('api/notes')` with a relative URL; the host's v2 handler for the `app` scheme answers it, as
`examples/vue-app` does with a Vue and TanStack Query app. The same page also runs against a real HTTP backend in a browser.
For calls the host starts, use `emb_view_eval` to run a function the page defines, or a page-defined event
(`window.dispatchEvent(...)`) from `emb_view_eval`.

## Networking

The network stack is built with `EMBER_NET` (on by default). It speaks HTTP/1.1 over TCP and TLS (TLS 1.2 or newer through
mbedTLS). It serves remote pages with their images, stylesheets, fonts and scripts, and `fetch`, XMLHttpRequest, WebSocket
(`ws:` and `wss:`) and EventSource. It has a cookie jar. All socket work runs on one network thread; results are delivered
on the view's thread.

Security model:

- Nothing is reachable by default. A request to a host that no `emb_runtime_net_allow(rt, pattern)` entry matches fails
  (a `TypeError` for `fetch`, an `error` event for XHR, WebSocket and EventSource) and logs one warning naming the host.
  Patterns: `host`, `host:port`, `*.example.com` (subdomains only, not the bare domain; optional `:port`), `*` (any host),
  IPv6 in brackets (`[::1]:8080`).
- The allowlist is checked on the host name, before DNS and for every redirect. Ember does not filter the addresses a name
  resolves to, so an allowed name that resolves to a private or loopback address is reachable: allow only hosts whose DNS you
  trust.
- There is no CORS. A request to an allowed host goes out like a native application's request.
- Certificate checks cannot be turned off. TLS 1.2 is the minimum, and SNI and hostname verification are always on.
- Trusted roots are Mozilla's list embedded in the library (disable with `emb_runtime_net_use_bundled_roots(rt, false)`) plus
  those you add with `emb_runtime_net_add_root(rt, pem_or_der, len)` (PEM text, which may hold several certificates, or one
  DER certificate), for a company or test CA. Changes apply to connections made afterwards.
- A page loaded from the network cannot load `file:` or `app:` resources: they are refused with a logged warning.
- `emb_runtime_net_set_user_agent(rt, ua)` changes the User-Agent (default `Ember/<version>`).
- With `EMBER_NET=OFF` these functions still exist but return false, and pages use only your handlers and files.

```c
#include <ember/ember.h>

void allow_api(emb_runtime *rt)
{
    emb_runtime_net_allow(rt, "api.example.com");        /* false: malformed pattern, or built without networking */
    emb_runtime_net_allow(rt, "*.cdn.example.com");
}
```

## Threading

- Use a runtime and all its views from one thread, the view's thread. This includes `emb_view_render`, `emb_view_pump`, every
  input call, `emb_view_eval` and all callbacks (handlers, console, navigation, tasks, eval results), which run on that
  thread.
- Thread-safe calls: `emb_view_post_task` (the only view call allowed from another thread), `emb_resource_complete` (any
  thread), and `emb_runtime_net_allow`.
- Ember starts background threads of its own: network I/O, system font indexing, the JavaScript realm (prepared ahead of the
  first script), GPU pipeline warm-up and a few one-shot start-up jobs. They do not call your handlers or callbacks, with one
  exception to plan for: the runtime log callback can be called from Ember's own threads (the network thread logs through
  it), so make the `log` function thread-safe.
- `emb_runtime_create` must happen before any view; call `SDL_SetMemoryFunctions` (if you use it) before it.

## Error handling

- Creation functions return `nullptr`. Boolean functions return false for a bad argument or an unavailable feature. All of
  them log through the runtime's `log` callback (default `SDL_Log`) with an `emb_log_level`.
- `emb_view_render` returning false means "nothing changed", not an error.
- A network page that fails to load shows an error page and logs the reason; `emb_view_load_url` returned true when the
  request started.
- JavaScript errors are logged (see above) and never stop the host.
- A windowed view whose GPU device cannot be created ends `emb_view_pump` and `emb_view_run` with false.

## Memory ownership

| Thing | Owner |
|---|---|
| `emb_runtime`, `emb_view` | You, until `emb_runtime_destroy` or `emb_view_destroy`. Views before their runtime. |
| `SDL_GPUDevice` of an offscreen view | You. It must outlive the view; destroying the view leaves it alone. |
| `SDL_GPUTexture` from `emb_view_texture` | The view. Do not release it. Fetch it again after a resize or scale change. |
| Strings you pass (titles, URLs, scheme and host names, allowlist patterns, directory paths, fonts, HTML) | You. Ember copies what it keeps, including font bytes and `emb_runtime_mount_dir` strings. |
| `emb_view_title` result | The view; valid until the next load. |
| Strings Ember passes to your callbacks (URLs, headers, bodies, console text, eval results, log lines) | Ember; valid only during the call. |
| Handler response bytes | You, but Ember copies them before your handler returns. |
| `emb_request *` | Ember; valid until you call `emb_resource_complete` on it. |

## Practical notes

- Set `size` on every descriptor and zero the rest (`memset` or `= {.size = sizeof ...}`).
- `EMB_API` marks the exported functions. On a shared library only these are exported.
