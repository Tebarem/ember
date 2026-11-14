#pragma once
/*
 * Ember public API. C23 only: the header compiles with
 * -std=c23 -Wall -Wextra -Werror -pedantic and needs no SDL header.
 *
 * Model. An emb_runtime holds what views share: the font registry (with the bundled default
 * face), the text system, the resource handlers and the allowed file directories. An emb_view is
 * one document with its own style engine, layout, display list, image cache and GPU state. A URL is
 * loaded by a handler the embedder registered, by the built-in file: and data: loaders (a file: URL
 * only inside an allowed directory), or, in a build with networking (EMBER_NET), over http and https
 * from the hosts the embedder allowed (ember_net.h). No host is allowed by default.
 *
 * Views come in two modes:
 *   EMB_VIEW_OFFSCREEN  the host owns the SDL_GPUDevice. emb_view_render records one frame into
 *                       the view's own texture (emb_view_texture) and reports whether the picture
 *                       changed. Nothing is submitted: the host submits its command buffer.
 *                       SDL copies: the host's SDL_GPUDevice* and SDL_GPUCommandBuffer* are plain
 *                       SDL3 objects that Ember's SDL calls into. A shared libember (EMBER_SHARED)
 *                       exports emb_* only and carries a private SDL3, so a host that links its own
 *                       SDL3 then runs two SDL copies (Windows DLLs always behaved this way).
 *                       That works only while both are the same SDL version. To have one copy,
 *                       build libember with -DEMBER_SDL_EXTERNAL=ON: it then leaves SDL3 and
 *                       SDL_image out and resolves them from the host's SDL3 at load time. A static
 *                       libember shares the host's SDL3 as it is linked.
 *   EMB_VIEW_WINDOWED   Ember creates the SDL_Window and its own device. emb_view_run runs the
 *                       event loop until the window closes; emb_view_pump runs one iteration for
 *                       hosts with their own loop.
 *
 * Threading. Every call comes from one thread (the view's thread). Handlers run on that thread.
 *
 * Coordinates. Sizes and positions are CSS px. A device px is one CSS px times the view's scale.
 *
 * Structs passed to the API start with a `size` field: set it to sizeof the struct. This version
 * does not yet check it, so a struct must come from the same header the library was built with.
 * Stats structs are filled with at most the caller's size, so a caller's older struct stays valid.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define EMB_VERSION_MAJOR 0
#define EMB_VERSION_MINOR 0
#define EMB_VERSION_PATCH 1
#define EMB_VERSION_STRING "0.0.1"

#if defined(_WIN32)
#  if defined(EMBER_SHARED)
#    if defined(EMBER_BUILDING)
#      define EMB_API __declspec(dllexport)
#    else
#      define EMB_API __declspec(dllimport)
#    endif
#  else
#    define EMB_API
#  endif
#else
#  define EMB_API __attribute__((visibility("default")))
#endif

/* Returns "MAJOR.MINOR.PATCH" as a static string. */
EMB_API const char *emb_version_string(void);

/* Returns (MAJOR << 16) | (MINOR << 8) | PATCH. */
EMB_API uint32_t emb_version(void);

/* ------------------------------------------------------------------ SDL GPU handles */

/* The SDL3 GPU types, forward declared so that this header needs no SDL include. SDL declares the
 * same typedefs, and C11 and later accept identical redeclarations. */
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUTexture SDL_GPUTexture;

/* ------------------------------------------------------------------ runtime */

typedef struct emb_runtime emb_runtime;
typedef struct emb_view emb_view;

/* emb_runtime_desc.flags */
/* Leaves the bundled default face out of the font registry. Not supported yet: the default face is
 * compiled into the library (EMBER_DEFAULT_FONT=OFF removes it at build time) and the registry cannot
 * drop a face, so emb_runtime_create logs an error and returns nullptr when this flag is set rather than
 * silently keeping the face. */
#define EMB_RUNTIME_NO_DEFAULT_FONT 0x1u

typedef enum emb_log_level {
    EMB_LOG_DEBUG = 0,
    EMB_LOG_INFO,
    EMB_LOG_WARN,
    EMB_LOG_ERROR,
} emb_log_level;

/* There are no allocator hooks in the descriptor: allocation goes through SDL, and SDL_SetMemoryFunctions is
 * process-wide and only safe before the first allocation. An embedder that needs its own allocator calls it itself,
 * before emb_runtime_create. */
typedef struct emb_runtime_desc {
    uint32_t size;            /* sizeof(emb_runtime_desc) */
    uint32_t flags;           /* EMB_RUNTIME_* */
    size_t image_cache_bytes; /* decoded image budget of each view; 0 is 64 MiB */
    void *log_user;
    /* Receives the library's log lines (no trailing newline). nullptr sends them to SDL_Log. */
    void (*log)(void *user, emb_log_level level, const char *message);
} emb_runtime_desc;

/* The bytes a resource handler returns. The library sets size (to sizeof(emb_resource)) and zeroes
 * the other fields before it calls the handler; the handler fills data and len and leaves size alone.
 * The struct can grow later (a media type, a free callback) without breaking handlers built against
 * this version. The bytes are copied before the handler's call returns, so data may point into the
 * handler's own storage that it reuses afterwards. */
typedef struct emb_resource {
    uint32_t size;       /* sizeof(emb_resource), set by the library */
    const uint8_t *data; /* set by the handler */
    size_t len;          /* set by the handler */
} emb_resource;

/* Loads url (absolute, with the scheme the handler was registered for) into *out and returns
 * true. Returning false fails the load: the image, font or stylesheet is then skipped. */
typedef bool (*emb_resource_fn)(void *user, const char *url, size_t url_len, emb_resource *out);

/* Creates a runtime (text system, default font, empty handler and path tables). Returns nullptr
 * on failure. desc may be nullptr for the defaults. */
EMB_API emb_runtime *emb_runtime_create(const emb_runtime_desc *desc);
/* Destroys the runtime. Every view made from it must be destroyed first. */
EMB_API void emb_runtime_destroy(emb_runtime *rt);

/* Adds a font file (TTF, OTF, TTC, WOFF, WOFF2) to the registry. The bytes are copied. Returns
 * false when the data holds no usable face. Fonts added here are used by views created later and
 * by the stylesheets of documents loaded later. */
EMB_API bool emb_runtime_add_font(emb_runtime *rt, const void *data, size_t len);

/* Registers the handler for URLs with scheme (for example "https", ASCII case-insensitive). The
 * handler is called for every URL of that scheme that a load needs (stylesheets, images, fonts,
 * and the document of emb_view_load_url). Passing fn == nullptr removes the handler. Returns false
 * for a bad scheme or when the scheme is file or data (those are built in). */
EMB_API bool emb_runtime_set_resource_handler(emb_runtime *rt, const char *scheme, emb_resource_fn fn,
                                              void *user);

/* Allows the file: loader to read files under dir (recursively). dir is an absolute path; the
 * loader refuses any path with a ".." segment. Returns false when dir is empty or too long. */
EMB_API bool emb_runtime_allow_path(emb_runtime *rt, const char *dir);

/* ------------------------------------------------------------------ fonts (system fonts)
 *
 * Any font: the registered and @font-face faces, the fonts installed on the system (found through SDL3's file APIs in
 * the platform's font directories and indexed on a job thread, so nothing waits for the index), and the bundled default.
 * CSS matching takes the family list in order (registered faces, then system faces by typographic, legacy and localized
 * family name), then the embedder's fallbacks, then per-character system fallback, then the bundled default. local()
 * in @font-face matches a system face by full or PostScript name. A system face is loaded whole, when matching first
 * picks it, and stays loaded until emb_runtime_trim_fonts or emb_runtime_destroy.
 *
 * Views created from a runtime with EMB_RUNTIME_NO_SYSTEM_FONTS see the registered and bundled faces only (the reftests and
 * the golden images use it, so their results do not depend on the machine). The environment variable
 * EMBER_NO_SYSTEM_FONTS (set to anything but empty or 0) does the same for every runtime of the process; the test harness
 * sets it.
 *
 * A library built with EMBER_DEFAULT_FONT=OFF (the default with EMBER_MIN_SIZE) has no bundled default face: text is drawn with
 * the system fonts and the fonts registered with emb_runtime_add_font. The embedder must provide one of them, and with
 * EMB_RUNTIME_NO_SYSTEM_FONTS a registered font. Text that no face can draw is skipped (each character takes half an em, nothing is
 * painted) and the runtime logs one EMB_LOG_WARN saying so, the first time it happens. In such a build the first font registered
 * with emb_runtime_add_font also becomes the default (last-resort) face, so pages that ask for sans-serif or an unknown family use it. */

/* emb_runtime_desc.flags: no system fonts. Off by default, so system fonts are used. */
#define EMB_RUNTIME_NO_SYSTEM_FONTS 0x2u

/* Sets the families that a CSS generic family resolves to: the first of them that is available wins. The names are
 * copied. count 0 restores the platform's list. generic is one of serif, sans-serif, monospace, cursive, fantasy,
 * system-ui, ui-serif, ui-sans-serif, ui-monospace, ui-rounded, emoji, math or fangsong (ASCII case-insensitive).
 * Returns false for any other name. */
EMB_API bool emb_runtime_set_generic_family(emb_runtime *rt, const char *generic, const char *const *families,
                                            size_t count);

/* Frees the system faces that no live layout uses. They are loaded again when matching picks them. Call it between
 * frames, not during a layout. */
EMB_API void emb_runtime_trim_fonts(emb_runtime *rt);

/* The state of the system font index and of the faces loaded from it. size must be set to sizeof(emb_runtime_stats). */
typedef struct emb_runtime_stats {
    uint32_t size;                 /* sizeof(emb_runtime_stats), set by the caller */
    uint32_t system_fonts;         /* 1 when system fonts are on (EMB_RUNTIME_NO_SYSTEM_FONTS not set) */
    uint32_t system_index_ready;   /* 1 once the index is built (0 while it is still being built) */
    uint32_t system_index_warm;    /* 1 when the index came from the cache (unchanged files were not parsed) */
    uint32_t system_files;         /* font files found */
    uint32_t system_index_entries; /* faces in the index */
    uint32_t system_index_reparsed; /* files parsed by the last build (all of them when cold) */
    uint32_t system_faces_loaded;  /* system faces loaded now */
    uint64_t system_font_bytes;    /* bytes of the loaded system faces */
    uint64_t system_index_bytes;   /* memory of the index itself */
    double system_index_ms;        /* wall time of the last index build */
} emb_runtime_stats;

EMB_API void emb_runtime_stats_get(const emb_runtime *rt, emb_runtime_stats *out);

/* ------------------------------------------------------------------ views */

typedef enum emb_view_mode {
    EMB_VIEW_OFFSCREEN = 0, /* the host's device, the host's command buffers */
    EMB_VIEW_WINDOWED,      /* an SDL window and a device of Ember's own */
} emb_view_mode;

typedef enum emb_color_scheme {
    EMB_SCHEME_LIGHT = 0,
    EMB_SCHEME_DARK,
} emb_color_scheme;

/* Text antialiasing. AUTO picks LCD (RGB stripes) on a windowed view on Windows and Linux when the
 * device scale is below 2, and grayscale elsewhere (macOS, 2x and above, and offscreen views, whose
 * texture the host may composite over anything). LCD is also drawn as grayscale wherever the
 * backdrop of the text is not opaque (layers with transparency, opacity groups), under rotation,
 * skew or scale animation. */
typedef enum emb_text_aa {
    EMB_TEXT_AA_AUTO = 0,
    EMB_TEXT_AA_GRAY,
    EMB_TEXT_AA_LCD_RGB,
    EMB_TEXT_AA_LCD_BGR,
} emb_text_aa;

typedef struct emb_view_desc {
    uint32_t size;          /* sizeof(emb_view_desc) */
    emb_view_mode mode;
    uint32_t width;         /* CSS px; 0 is 800 */
    uint32_t height;        /* CSS px; 0 is 600 */
    float scale;            /* offscreen: device px per CSS px, 0 is 1. Windowed: ignored, the window sets the scale */
    SDL_GPUDevice *device;  /* offscreen: the host's device, not owned, must outlive the view */
    const char *title;      /* windowed: the window title until the document has a <title>; nullptr: "Ember" */
    emb_color_scheme scheme;
    emb_text_aa text_aa;    /* 0 is EMB_TEXT_AA_AUTO. Read only when size covers it */
    bool no_script;         /* true: scripts do not run and <noscript> content is built. Read only when size covers it */
    bool transparent;       /* offscreen, true: the canvas is transparent where the page paints nothing (no root or body
                             * background), for a host that composites the texture over its own picture; false: the
                             * Canvas color of the root's color scheme, as a browser tab. Read only when size covers it */
} emb_view_desc;

/* The stages of the most recent run of each, in milliseconds. The first_paint fields hold the
 * cold start of the view (runtime_init_ms is the runtime's own creation) and are kept until the
 * next load of a document; the others are the values of the latest update. */
typedef struct emb_view_stats {
    uint32_t size;               /* sizeof(emb_view_stats), set by the caller */
    double runtime_init_ms;      /* emb_runtime_create */
    double gpu_pipelines_ms;     /* view creation: renderer, atlas, layer pool (the buffer warm-up thread runs on after this) */
    double fonts_ms;             /* the first text shaping of the runtime (font loading and face setup); a part of layout_ms */
    double parse_ms;             /* the HTML parse of the last load */
    double style_ms;             /* the style resolve of the last layout */
    double layout_ms;            /* the layout of the last layout (includes fonts_ms) */
    double display_list_ms;      /* the display list build of the last paint */
    double upload_ms;            /* the image uploads of the last frame: a part of gpu_ms (glyph uploads are not timed apart) */
    double submit_ms;            /* the last queue submit made by the view (windowed mode only) */
    double paint_ms;             /* style, layout and display list of the last update */
    double gpu_ms;               /* the recording of the last frame: passes, draws and uploads (upload_ms is inside it) */
    uint32_t items;              /* display list items of the last paint */
    uint32_t layers;             /* intermediate layers the last frame drew */
    uint32_t draw_calls;         /* draw calls of the last frame */
    uint32_t damage_rects;       /* damage rects of the last frame (0 for a full frame or no change) */
    uint64_t damage_area;        /* damage area of the last frame in device px */
    bool full_redraw;            /* the last frame redrew the whole view */
    double stylesheets_ms;       /* the <style> and <link rel=stylesheet> sheets of the last load: fetch, read and CSS parse */
    double fetch_ms;             /* the document fetch of the last emb_view_load_url (the handler or the file read) */
    uint32_t items_recorded;     /* display list items the last frame recorded into the GPU frame */
    uint32_t items_culled;       /* display list items the last frame skipped because they lie outside what it draws */
    uint32_t batches;            /* instanced batches (draw groups) of the last frame; draw_calls counts the GPU calls */
    double damage_ms;            /* the damage computation of the last paint (0 when a pure scroll reused the list) */
    bool scroll_reused;          /* the last paint was a pure scroll: the display list was reused, not rebuilt */
    /* Inside parse_ms (the scripts of the last load): the realm taken or built, the JavaScript compiler, and the rest of
     * what the scripts ran (their top-level code and the jobs that follow). The remainder of parse_ms is the HTML parse. */
    double script_realm_ms;
    double script_compile_ms;
    double script_run_ms;
} emb_view_stats;

/* Creates a view that uses the runtime rt, which must outlive it. Returns nullptr on failure (logged).
 * Offscreen mode requires desc->device. Windowed mode creates its GPU device on first need (the first frame, or the
 * first image or canvas), so the page loads while the driver starts, and shows the window once its first frame is
 * presented. A device that cannot be created then ends emb_view_pump and emb_view_run (false); emb_view_texture is
 * nullptr until it exists. */
EMB_API emb_view *emb_view_create(emb_runtime *rt, const emb_view_desc *desc);
/* Destroys the view and its GPU resources. The device of an offscreen view is left alone. */
EMB_API void emb_view_destroy(emb_view *view);

/* Parses html (UTF-8, a leading BOM is dropped) and loads it as the document. base_url is the
 * document's URL, used to resolve its relative URLs; nullptr or "" means none. Scripts run unless
 * the view was created with no_script. Returns false when the document could not be laid out. The previous document is replaced.
 * A document that needs network resources before it can be shown (a script or a stylesheet from an http or https URL,
 * when the library is built with networking) does not make the call wait: it returns true, the view keeps showing the
 * previous page and answers events, and the new page appears when those resources have arrived (or failed or timed out).
 * Called from inside a script of the current page (a navigation hook), the load is deferred until that script returns and
 * the call also returns true. */
EMB_API bool emb_view_load_html(emb_view *view, const char *html, size_t len, const char *base_url);
/* Loads url through the handlers (a file: URL, a bare path, or a scheme with a handler) and then
 * as emb_view_load_html with url as base. Returns false when the URL could not be loaded.
 * An http or https URL (when the library is built with networking) loads asynchronously: the call returns true when
 * the request has started, the previous document stays until the response arrives, and a failure (the network, the
 * status, a Content-Type that is not HTML, the allowlist) shows an error page instead of returning false. The
 * #fragment stays in the document's URL but is not sent. Requests are subject to the runtime's host allowlist. */
EMB_API bool emb_view_load_url(emb_view *view, const char *url);

/* Sets the viewport in CSS px (0 keeps the old value). The page is laid out again on the next
 * update. On a windowed view this resizes the window. */
EMB_API void emb_view_resize(emb_view *view, uint32_t width, uint32_t height);
/* Sets the device px per CSS px (values <= 0 are ignored). Everything is repainted. Ignored on a windowed view: the
 * window's display scale decides it, and it follows the window's size and display changes. */
EMB_API void emb_view_set_scale(emb_view *view, float scale);
/* Sets the color scheme: feeds prefers-color-scheme and the system colors. */
EMB_API void emb_view_set_color_scheme(emb_view *view, emb_color_scheme scheme);
/* Sets the text antialiasing (see emb_text_aa). Everything is repainted. */
EMB_API void emb_view_set_text_aa(emb_view *view, emb_text_aa mode);

/* Scrolls the viewport by (dx, dy) CSS px at once, clamped to its range. Returns nothing; the
 * change shows on the next update. */
EMB_API void emb_view_scroll_by(emb_view *view, float dx, float dy);
/* Scrolls the viewport to (x, y) CSS px. When the root element has scroll-behavior: smooth the
 * scroll animates over 150 ms: the host must keep updating the view while the animation runs (each
 * emb_view_render or emb_view_pump returns changed until it ends). */
EMB_API void emb_view_scroll_to(emb_view *view, float x, float y);
/* Scrolls at the point (x, y) CSS px of the viewport by (dx, dy): the innermost scroll container
 * under the point that can move in that direction takes it, else the viewport. This is what the
 * mouse wheel does. */
EMB_API void emb_view_wheel(emb_view *view, float x, float y, float dx, float dy);

/* The document's <title>, or "" when it has none. The pointer stays valid until the next load. */
EMB_API const char *emb_view_title(const emb_view *view);

/* The view's own texture (the frame's target in offscreen mode, and the source of the window's
 * present in windowed mode). It is replaced by any change of the device size (a resize or a scale change that
 * alters the pixel size), so fetch it again after each such change. */
EMB_API SDL_GPUTexture *emb_view_texture(const emb_view *view);

/* Offscreen mode. Updates the view (style, layout, display list, damage) and records the frame
 * damage into cmd and into emb_view_texture. Returns true when the picture changed. A frame that
 * changed nothing records no GPU work. The first call always draws the whole view. Records nothing
 * on a windowed view (use emb_view_pump there). */
EMB_API bool emb_view_render(emb_view *view, SDL_GPUCommandBuffer *cmd);

/* Windowed mode. Runs the event loop until the window is closed. Returns true when it closed
 * normally and false on an error (not a windowed view, or the GPU failed repeatedly). */
EMB_API bool emb_view_run(emb_view *view);
/* Windowed mode. Handles the pending events, updates and presents the view once, and waits a
 * short time when there is nothing to do. Returns false once the window has been closed. */
EMB_API bool emb_view_pump(emb_view *view);

/* Fills the stats (see emb_view_stats). out->size must be set. */
EMB_API void emb_view_stats_get(const emb_view *view, emb_view_stats *out);

#ifdef __cplusplus
}
#endif

/* The script, input and resource APIs (each header also stands alone). */
#include <ember/ember_script.h>
#include <ember/ember_input.h>
#include <ember/ember_resource.h>
#include <ember/ember_net.h>
