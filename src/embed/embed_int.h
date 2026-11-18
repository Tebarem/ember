#pragma once
/*
 * Internal declarations of the embedding layer (W4e): the runtime and view records and the functions the runtime,
 * the view and the windowed loop share. The public API is include/ember/ember.h.
 *
 * Layering. embed/url.c and runtime.c are plain; fonts.c includes the text registry; doc.c includes the CSS, style,
 * layout and paint headers; gfx.c includes the renderer headers; view.c and window.c see only the plain interfaces
 * (doc.h, gfx.h) and the display list types, so the headers of the different worlds never meet in one unit.
 */
#include <ember/ember.h>
#include <ember/ember_script.h>

#include "base/base.h"
#include "embed/doc.h"
#include "embed/gfx.h"
#include "gpu/gpu.h"
#include "paint/damage.h"
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_video.h>

typedef struct embi_text embi_text;
typedef struct embi_fontdb embi_fontdb;
typedef struct embi_gpu embi_gpu;

#define EMBI_RT_MAX_HANDLERS 8u
#define EMBI_RT_MAX_DIRS 16u
#define EMBI_VIEW_WHEEL_PX 53.0f  /* CSS px per wheel notch (as Chromium) */
#define EMBI_VIEW_KEY_PX 40.0f    /* CSS px per arrow key */

typedef struct embi_rt_handler {
    char *scheme;   /* lowercase, SDL_malloc'd; nullptr when the slot is free */
    emb_resource_fn fn;
    void *user;
} embi_rt_handler;

struct emb_runtime {
    uint32_t flags;
    size_t image_bytes;
    void *log_user;
    void (*log)(void *user, emb_log_level level, const char *message);
    embi_text *text;     /* the text system (glyphs, shaping); owns the font registry */
    embi_fontdb *fonts;  /* the registry of text (borrowed from text) */
    embi_rt_handler handlers[EMBI_RT_MAX_HANDLERS];
    char *dirs[EMBI_RT_MAX_DIRS]; /* allowed directories for file: (normalized, no trailing slash) */
    uint32_t ndirs;
    double runtime_init_ms;
    SDL_Thread *ua_thread;           /* parses the shared UA sheet while the host sets up (FP2b); joined at destroy */
    struct embi_css_sheet *ua_sheet; /* the reference that thread took (written before its exit, read after the join) */
    struct embi_net *net; /* the network service (EMBER_NET builds; nullptr otherwise): its thread starts at the first request */
};

/* The scrollers that scrolled recently: their key and the time of their last scroll, for the fade of their bars. */
typedef struct embi_view_track {
    const void *key;
    double last_ms;
    bool seen;
    bool shown; /* the bar was visible at the last paint */
} embi_view_track;

struct emb_view {
    emb_runtime *rt;
    emb_view_mode mode;
    emb_color_scheme scheme;
    emb_text_aa text_aa;
    emb_text_hinting text_hinting;
    bool transparent; /* offscreen: the canvas clears to transparent (emb_view_desc.transparent) */
    /* The device: the host's (offscreen) or Ember's own (windowed). */
    embi_gpu *gpu;
    bool gpu_owned;
    SDL_GPUDevice *dev;
    SDL_GPUTextureFormat fmt;
    SDL_Window *window;        /* windowed only */
    bool window_claimed;
    bool window_ref;           /* this view holds a reference on the SDL video subsystem */
    /* Rendering. */
    embi_gfx *gfx;
    SDL_GPUTexture *tex;       /* the view's texture, dev_w x dev_h device px */
    uint32_t dev_w, dev_h;
    float scale;               /* device px per CSS px */
    float raster;              /* offscreen: an extra factor for the drawn pixels (emb_view_set_raster_scale), 1 by default */
    uint32_t flags;            /* offscreen: the EMB_VIEW_* of emb_view_desc.flags */
    uint32_t css_w, css_h;     /* the viewport in CSS px */
    /* The document and the display lists: two of them, the current one drawn and the one being built. */
    embi_doc *doc;
    embi_dl dl[2];
    uint32_t cur;
    uint32_t content_len[2];   /* scroll60: the items of each list that are the page (the scrollbars follow them) */
    bool have_prev;            /* dl[cur] is the list on screen and its runs are valid */
    uint64_t prev_serial;      /* the layout serial (embi_doc_layout_serial) dl[cur] was built at */
    uint64_t prev_runs;        /* the run epoch (embi_doc_run_epoch) dl[cur] was built at */
    embi_damage damage;
    bool paint_dirty, force_full, need_present, closed;
    bool scroll_dirty;         /* scroll60: a scroll offset changed since the last paint (no other reason is implied) */
    bool sbar_dirty;           /* the hover of a styled scrollbar's thumb changed: its bars are made again */
    bool style_paint;          /* W6b: the last update changed only paint properties (no other reason to repaint) */
    bool failed;               /* windowed: the loop stopped on an error (the GPU kept failing) */
    bool shown;                /* windowed: the window was shown (it stays hidden until the first frame is presented) */
    uint32_t gpu_fails;        /* windowed: consecutive frames whose command buffer could not be acquired */
    /* Scrolling. */
    embi_view_track *tracks;   /* SDL_malloc'd, grown as needed */
    uint32_t ntracks, captracks;
    embi_doc_anim anim;
    char *fallback_title;      /* windowed: the title given in the descriptor (SDL_malloc'd) */
    emb_view_stats st;
    /* The input layer's state (input.c): created on the first input call or navigation hook, freed with the view. */
    struct embi_input *input;
    /* N1d: the network loads of the view's document (netload.c); created on the first http(s) load. */
    struct embi_netload *netload;
    /* A load requested while a script of the document runs (an embedder's navigation hook calling emb_view_load_html from
     * location.assign): the document and its realm cannot be reset under the running script, so the request waits here
     * and runs when the outermost script returns (view.c, script.c). */
    char *defer_html;
    size_t defer_len;
    char *defer_base;
    bool defer_has;

    /* The document came from the network (a navigation the view's netload finished), not from the embedder: it alone is
     * kept from local schemes and has cookies. net_doc_next is raised just for the load that nav_done makes. */
    bool net_doc, net_doc_next;

    /* HX1: the host channel (emb_view_set_host_channel). The name is for the documents loaded from now on: a realm keeps the
     * name it was given (script.c). fn and user are read at each call, so a new set_host_channel replaces them at once. */
    bool host_channel;
    char host_name[64];
    emb_host_message_fn host_fn;
    void *host_user;
    /* IF1: the view's frames (its iframes' child views) and, for a child view, its place under its parent (frames.c). */
    struct embi_frames *frames;
    /* emb_view_render_to: the target of the last picture drawn into a host texture, valid while rt_valid. drew_own is set
     * when the last picture went to the view's own texture (a render_to then draws the whole rectangle). */
    SDL_GPUTexture *rt_tex;
    int32_t rt_x, rt_y;
    uint32_t rt_w, rt_h;
    bool rt_valid;
    bool drew_own;
};

/* The device px per CSS px the view draws at: the scale times the raster scale. The document keeps the scale alone. */
static inline float embi_view_draw_scale(const emb_view *v)
{
    return v->scale * v->raster;
}

/* ------------------------------------------------------------------ runtime (runtime.c) */

/* Logs a line through the runtime's callback, or SDL_Log. */
void embi_rt_log(emb_runtime *rt, emb_log_level level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* Loads an absolute URL (or a bare path) through the built-in schemes and the handlers. The bytes are SDL_malloc'd. */
bool embi_rt_fetch(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len);
/* The same for an @font-face source: it alone may also resolve local(NAME) (the "local:" scheme), which every other
 * fetcher (stylesheets, images, handlers) refuses so a page cannot read or probe the installed fonts. */
bool embi_rt_fetch_font(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len);

/* ------------------------------------------------------------------ fonts (fonts.c) */

/* Adds a face file to the registry. owned: the bytes are SDL_malloc'd and the registry takes them. Returns the number
 * of faces added. */
uint32_t embi_fonts_add(embi_fontdb *db, const uint8_t *data, size_t len, bool owned);
/* System fonts (W4i): starts the index unless the runtime flags turn them off. */
void embi_fonts_system_start(embi_fontdb *db, bool enabled);
/* The bytes of the system font named name (full or PostScript name), in SDL memory: the local() source. */
bool embi_fonts_local(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len);
/* Frees the system faces no layout uses (t is the text system that shapes with them). Returns the count. */
uint32_t embi_fonts_trim(embi_fontdb *db, embi_text *t);
/* The number of faces in the registry (registered, @font-face and bundled; not the system faces that are indexed only). */
uint32_t embi_fonts_face_count(const embi_fontdb *db);
/* Test hook: the directories every later system font index scans instead of the platform's (a copy), and the delay of the
 * index job in ms. dirs nullptr restores the platform's. Call before emb_runtime_create. */
void embi_fonts_test_dirs(const char *const *dirs, uint32_t n, uint32_t delay_ms);
/* emb_runtime_set_generic_family. */
/* SZ3b: where the registry's one-time "no font can draw this text" warning goes (text.h, embi_fontdb_set_warn). */
void embi_fonts_default_if_none(embi_fontdb *db, uint32_t index);
void embi_fonts_set_warn(embi_fontdb *db, void (*fn)(void *user, const char *msg), void *user);
bool embi_fonts_generic(embi_fontdb *db, const char *generic, const char *const *families, size_t count);
/* emb_runtime_stats_get. */
void embi_fonts_stats(const embi_fontdb *db, bool enabled, emb_runtime_stats *out);

/* ------------------------------------------------------------------ view (view.c) */

/* Updates the view: applies the scroll animation, relays out when the document or the viewport changed, paints when
 * something changed, and computes the damage against the list on screen. Returns true when the picture changed. */
bool embi_view_update(emb_view *v);
/* Brings style and layout up to date without painting, for hit testing and focus: the damage of the next frame still
 * compares against the list on screen. */
void embi_view_layout(emb_view *v);
/* Updates and draws the damage into the view's texture, recording into cmd. Returns true when the picture changed. */
bool embi_view_frame(emb_view *v, SDL_GPUCommandBuffer *cmd);
/* Sets the window's size (CSS px, the logical size) and its device size (px), used by the windowed loop. */
void embi_view_set_window_size(emb_view *v, uint32_t css_w, uint32_t css_h, uint32_t dev_w, uint32_t dev_h);
/* Records a scroll of the element or the viewport key for its bar. */
void embi_view_note_scroll(emb_view *v, const void *key);
/* Whether the view has something animating (a smooth scroll or a visible scrollbar). */
bool embi_view_animating(const emb_view *v);
/* Sets the window title from the document, or the descriptor's title. */
void embi_view_apply_title(emb_view *v);
/* Scrolls the viewport or a container under a point (windowed input). Returns true when something scrolled. */
bool embi_view_wheel(emb_view *v, float x, float y, float dx, float dy);
/* Loads a document given as the bytes a transport delivered (the encoding is sniffed from the BOM, the Content-Type in mime
 * (may be nullptr) and the meta tags; UTF-8 otherwise) with base_url as its URL. */
/* Whether the view's document is half built and waiting for the network (N1f): it has no layout, so input is ignored. */
static inline bool embi_view_load_suspended(const emb_view *v)
{
    return v->doc != nullptr && embi_doc_load_suspended(v->doc);
}
/* A document finished loading, now or later (a suspended load, N1f): the view starts painting it. */
void embi_view_load_done(emb_view *v, const embi_doc_times *t, bool ok);
bool embi_view_load_bytes(emb_view *v, const uint8_t *bytes, size_t len, const embi_sb *mime, const char *base_url);
/* Scrolls the root viewport by a delta at once (keyboard input). */
void embi_view_scroll_root(emb_view *v, float dx, float dy);
/* Reads the window's sizes (logical and pixel) into the view and asks for a present; windowed mode only. */
void embi_view_window_reflow(emb_view *v);
/* IF1: a child view for an iframe of parent: offscreen, sharing the root view's device and renderer, drawing into a texture
 * of its own (made once the renderer exists). Its document is created empty. */
emb_view *embi_view_new_child(emb_view *parent);
/* IF1: gives a child view the root's renderer (a windowed root makes it at its first frame) and its texture. */
void embi_view_child_gfx(emb_view *v);
/* IF1: sets a child view's viewport (CSS px; its scale is the root's). */
void embi_view_set_child_size(emb_view *v, uint32_t css_w, uint32_t css_h);
/* Draws the damage of the last update into the view's texture (changed: that update changed the picture). Returns changed. */
bool embi_view_draw(emb_view *v, SDL_GPUCommandBuffer *cmd, bool changed);
/* IF1: the texture of an iframe's frame below the root view user (the renderer's "ember-frame:" lookup). */
SDL_GPUTexture *embi_frames_texture(void *user, const void *key, uint32_t *w, uint32_t *h);

/* ------------------------------------------------------------------ window (window.c) */

/* Creates the SDL window of a windowed view (video initialized, the window shown with the placeholder color, no GPU
 * device yet). Returns false on failure. */
bool embi_window_open(emb_view *v, const emb_view_desc *desc);
/* Creates the GPU device of a windowed view and gives it the window (embi_view_gfx_ready calls it once). */
bool embi_window_gpu_open(emb_view *v);
/* Makes sure the view has its renderer, creating the GPU device of a windowed view on first use. False when that
 * failed (logged; the view is marked failed). */
bool embi_view_gfx_ready(emb_view *v);
/* Handles one event. Returns false when the window should close. */
bool embi_window_event(emb_view *v, const SDL_Event *ev);
/* Releases the window's claim on the device. Call it before the device is destroyed. */
void embi_window_release(emb_view *v);
/* Destroys the window and, when this view opened the video subsystem, quits it. Call it after the device is gone. */
void embi_window_close(emb_view *v);
