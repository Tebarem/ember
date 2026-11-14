#pragma once
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * The document side of a view (W4e): parsing, the stylesheets of the document, style, layout, the display list, the
 * scroll state of the viewport and of the scroll containers, and the scrollbars. Implemented in doc.c, which includes
 * the CSS, style, layout and paint headers. This header includes only the display list types, so the view and the
 * GPU side never see those headers.
 */
#include "base/base.h"
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_text embi_text;
typedef struct embi_fontdb embi_fontdb;
typedef struct embi_doc embi_doc;

/* Callbacks the document needs from the view. fetch loads an absolute URL and returns SDL_malloc'd bytes (the caller
 * frees them with SDL_free); false when it cannot. image_size gives the natural size of an image (absolute URL). */
typedef struct embi_doc_cb {
    void *user;
    bool (*fetch)(void *user, embi_str url, uint8_t **data, size_t *len);
    bool (*image_size)(void *user, embi_str url, float *w, float *h, bool *has_ratio);
    void *owner; /* the emb_view that owns the document (embi_doc_owner); nullptr for a document made without a view */
} embi_doc_cb;
/* N1d, N1f: network loads (optional, set once after embi_doc_create). fetch_script loads the source of a script or module that has
 * arrived (cb->fetch is used without it). pending_count counts the loads that answered "not yet" (a network resource that has
 * not arrived): a consumer that sees it change during a fetch knows the failure is temporary and the resource is tried again
 * after embi_doc_net_arrived. prefetch starts a download that will be needed. net_pending says whether an absolute URL is a
 * network resource still downloading (it starts the download if nobody asked yet). All get cb->user. */
void embi_doc_set_net(embi_doc *d, bool (*fetch_script)(void *user, embi_str url, uint8_t **data, size_t *len),
                      uint32_t (*pending_count)(void *user), void (*prefetch)(void *user, embi_str url),
                      bool (*net_pending)(void *user, embi_str url));

/* The times of a document step, in ms. */
typedef struct embi_doc_times {
    double parse_ms, style_ms, layout_ms;
    double sheets_ms; /* a load only: the <style> and <link rel=stylesheet> sheets (fetch, read and CSS parse) */
    double script_realm_ms, script_compile_ms, script_run_ms; /* a load only, inside parse_ms (FP2a) */
} embi_doc_times;

/* A scrollbar to draw: the key of its scroller (the element, or the document for the viewport) and the alpha of its
 * fade. */
typedef struct embi_doc_sb {
    const void *key;
    float alpha;
} embi_doc_sb;

/* A smooth programmatic scroll of the viewport (scroll-behavior: smooth), in CSS px. t0 is in ms. */
typedef struct embi_doc_anim {
    bool active;
    float x0, y0, x1, y1;
    double t0;
} embi_doc_anim;

typedef struct embi_css_sheet embi_css_sheet;
/* Sets the adopted sheets of the document (document.adoptedStyleSheets, W7d): the cascade takes them after the document's own
 * sheets, and the style is marked dirty. The document refs the sheets it keeps and drops the ones it held before. A load
 * clears them (a new document adopts nothing). */
void embi_doc_set_adopted(embi_doc *d, embi_css_sheet *const *list, uint32_t n);
/* W6e: collects the document's <style> and <link> sheets again when one was inserted, removed or edited since the last
 * time (and during the parse, for a script that reads styles). A no-op otherwise. The update calls it; the script bindings
 * call it before they read styles or geometry. */
void embi_doc_sync_sheets(embi_doc *d);

/* Creates a document state over the text system and the font registry (both borrowed, they must outlive it). cb is
 * copied. Returns nullptr on failure. */
embi_doc *embi_doc_create(embi_text *text, embi_fontdb *fonts, const embi_doc_cb *cb);
/* W4i: the loader of @font-face sources (alone may resolve local: names); without one, cb->fetch loads them. */
void embi_doc_set_font_fetch(embi_doc *d, bool (*fetch)(void *user, embi_str url, uint8_t **data, size_t *len));
void embi_doc_destroy(embi_doc *d);

/* The system font index of the registry (W4i, fonts.c): the number of index builds that finished (0 before the first), and
 * the lookups so far that used a fallback. A document whose last layout used a fallback restyles when the epoch changes. */
uint64_t embi_fonts_sys_epoch(const embi_fontdb *fonts);
uint64_t embi_fonts_fallback_uses(const embi_fontdb *fonts);

/* The viewport in CSS px, the device pixel ratio (media queries) and the color scheme. Marks the styles and the layout
 * dirty when they change. */
void embi_doc_set_viewport(embi_doc *d, float w, float h, float dppx, bool dark);
/* FP2b: hands the style engine the media environment of the viewport now, while no document is loaded (so the load
 * does not re-flatten the sheets it has just applied). A no-op once a document exists. */
void embi_doc_prime_env(embi_doc *d);
/* W8b: the viewport set by embi_doc_set_viewport (0 x 0 before the first call) and the device pixel ratio. */
void embi_doc_viewport(const embi_doc *d, float *w, float *h, float *dppx);

/* Replaces the document with the HTML in html, whose relative URLs resolve against base_url (empty: none). Collects the
 * <style> and <link rel=stylesheet> sheets, resolves the styles and lays the page out. Returns false when the layout
 * failed.
 *
 * A load can be resumable (N1f). A parser-blocking script, a render-blocking stylesheet, a deferred or module script of a
 * page from the network that has not arrived suspends the load instead of making the thread wait: embi_doc_load_begin returns
 * EMBI_DOC_LOAD_SUSPENDED, the document is half built, and embi_doc_net_arrived resumes the load from the task that reports
 * the arrival. The load that finishes later reports its result to the view (embi_view_load_done in embed_int.h). Content that
 * is local (files, app://, a handler) never suspends. A new load, or destroying the document, ends a suspended one.
 * embi_doc_load is begin for callers without a view: it returns true when the load was suspended. */
typedef enum embi_doc_load_status {
    EMBI_DOC_LOAD_FAILED = 0, /* finished; the layout failed */
    EMBI_DOC_LOAD_DONE,       /* finished */
    EMBI_DOC_LOAD_SUSPENDED,  /* waiting for the network */
    EMBI_DOC_LOAD_GONE,       /* a script replaced the document during the load: the replacement reports its own result */
} embi_doc_load_status;
embi_doc_load_status embi_doc_load_begin(embi_doc *d, embi_str html, embi_str base_url, embi_doc_times *t);
bool embi_doc_load(embi_doc *d, embi_str html, embi_str base_url, embi_doc_times *t);
/* Called with cb->user when a suspended load finishes later (the loads that finish inside embi_doc_load_begin report through its
 * return value, not through this): the view takes the times and starts painting the new document. */
void embi_doc_set_load_done(embi_doc *d, void (*fn)(void *user, const embi_doc_times *t, bool ok));
/* Whether a load is suspended: the document is half built, has no style or layout, and shows nothing. */
bool embi_doc_load_suspended(const embi_doc *d);
/* Turns scripts on (the default) or off for later loads: off, no script runs and <noscript> content is built as elements. */
void embi_doc_set_scripting(embi_doc *d, bool on);
/* Resolves the styles and lays the page out again when the document or the viewport changed. Returns true when the
 * fragments were rebuilt (the layout's text runs are new, so the previous display list must not be compared with the next
 * one). An update that changed nothing, or only paint properties (W6b), returns false and keeps the fragments; the latter
 * leaves a paint pending (embi_doc_take_paint_pending). */
bool embi_doc_update(embi_doc *d, embi_doc_times *t);
/* Whether a paint-only update happened since the last call (and forgets it). */
bool embi_doc_take_paint_pending(embi_doc *d);
/* N1f: whether a <link rel=stylesheet> of the part of the document built so far is still downloading (a script waits for it,
 * as it waits for the styles it may read). Starts the downloads it finds. Always false without the network. */
bool embi_doc_sheets_pending(embi_doc *d);
/* N1d: the network resource url the document asked for without waiting has arrived, or failed (ok false); kinds: EMBI_NL_* of
 * embed/netload.h. An <img> with that src gets its load or error event. images
 * lay out and paint again, @font-face sources that were pending are registered, stylesheets are collected again. */
/* A network image that was refused (no request): the error event of its <img> elements, no relayout. */
void embi_doc_image_failed(embi_doc *d, embi_str url);
void embi_doc_net_arrived(embi_doc *d, unsigned kinds, embi_str url, bool ok);
/* After an arrival of a dropped or stale record was counted off: the gated load event may be free to fire. */
void embi_doc_recheck_gate(embi_doc *d);
/* Whether a document is loaded. */
bool embi_doc_loaded(const embi_doc *d);
/* The document's style and layout engines (the binding context's geometry and computed style read them). */
typedef struct embi_style_engine embi_style_engine;
typedef struct embi_layout_engine embi_layout_engine;
embi_style_engine *embi_doc_style_engine(embi_doc *d);
/* R4c: the lookup of SVG images: the image behind an absolute URL, or nullptr when it is not an SVG image. The view
 * provides it (its image cache sees the bytes first). user is the cb.user of the document. */
/* The natural size of an image URL as written in the document (R4c: img.naturalWidth); false when it is not loaded. */
bool embi_doc_image_natural(embi_doc *d, embi_str raw, float *w, float *h);
struct embi_svg_image;
void embi_doc_set_svg_image(embi_doc *d, const struct embi_svg_image *(*fn)(void *user, embi_str url));
/* R4c (svg_geom.c): installs the SVG DOM geometry of the document on a bind context. */
struct embi_bind_ctx;
#if EMBER_SVG
void embi_doc_install_svg_geometry(struct embi_bind_ctx *c, embi_doc *d);
#else
static inline void embi_doc_install_svg_geometry(struct embi_bind_ctx *c, embi_doc *d)
{
    (void)c;
    (void)d;
}
#endif
embi_layout_engine *embi_doc_layout_engine(embi_doc *d);
/* Marks the style and layout dirty: the next embi_doc_update restyles and lays out (a state change of the input layer, the
 * pseudo-classes, reads it). */
void embi_doc_invalidate(embi_doc *d);

/* Resolves a URL of the document (an image, a font) against its base: the result is owned by the document and valid
 * until the next load. */
embi_str embi_doc_resolve(embi_doc *d, embi_str raw);

/* Builds the display list of the current layout into out for a viewport of vw x vh CSS px (out is reset). Scrollbars
 * are not included: see embi_doc_emit_scrollbars. Returns the time in ms. */
double embi_doc_paint(embi_doc *d, embi_dl *out, float vw, float vh, uint32_t focus_rgba);
/* The same through the document's display list cache (W6b): with incremental, and only paint properties changed since the
 * last cached paint, only the stacking contexts that hold the changed elements are built again. */
double embi_doc_paint_ex(embi_doc *d, embi_dl *out, float vw, float vh, uint32_t focus_rgba, bool cached, bool incremental);
/* scroll60: a paint after a change of scroll offsets only: the list on screen (its first content_len items are the page) is
 * moved to the new offsets in place and cut back to the page; nothing is built. Returns 0 when it cannot (paint as usual),
 * 1 when no offset changed, 2 when the list was moved. */
int embi_doc_paint_scroll(embi_doc *d, embi_dl *list, uint32_t content_len, float vw, float vh, uint32_t focus_rgba,
                         embi_scroll_damage *dmg, double *ms);
/* Appends the scrollbars of the scrollers listed in sbs (those with alpha > 0) to out, after their content. Returns the
 * number of items appended. */
uint32_t embi_doc_emit_scrollbars(embi_doc *d, embi_dl *out, const embi_doc_sb *sbs, uint32_t n);
/* The keys of the scrollers that have a scrollbar to show: the document key (embi_doc_root_key) for the viewport. */
const void *embi_doc_root_key(const embi_doc *d);
/* Whether the scroller with key has an overflow in some axis (so it can show a bar). */
bool embi_doc_scroller_overflows(embi_doc *d, const void *key, float vw, float vh);

/* The color the canvas is cleared to before the page paints: the Canvas system color of the root element's used color
 * scheme (white unless the root opts in to dark and the view prefers dark), as 0xAABBGGRR with alpha 0xFF. */
uint32_t embi_doc_canvas_rgba(const embi_doc *d);

/* Whether the body element has an onload attribute (a handler for the window's load event that no script created). */
bool embi_doc_body_has_load_handler(embi_doc *d);

/* The viewport's scroll offset (CSS px). */
void embi_doc_root_offset(const embi_doc *d, float *x, float *y);
struct embi_element;
/* W5e: the byte offset in the value of text control el at the document point (x, y), and its caret rectangle (document px).
 * bb is the border box of el in document px. False when el is not a laid out text control. */
bool embi_doc_control_offset(const embi_doc *d, const struct embi_element *el, const float bb[4], float x, float y, uint32_t *byte);
bool embi_doc_control_caret(const embi_doc *d, const struct embi_element *el, const float bb[4], float rect[4]);
/* The scroll offset of the scroller with key (the document key is the viewport). False when there is no such scroller. */
bool embi_doc_scroller_offset(embi_doc *d, const void *key, float *x, float *y);
/* Scrolls the viewport by (dx, dy), clamped. Returns true when the offset changed. */
bool embi_doc_scroll_root_by(embi_doc *d, float dx, float dy, float vw, float vh);
/* Sets the viewport's offset, clamped. Returns true when it changed. */
bool embi_doc_scroll_root_to(embi_doc *d, float x, float y, float vw, float vh);
/* Whether the root element has scroll-behavior: smooth. */
bool embi_doc_root_smooth(embi_doc *d);
/* Scrolls the innermost scroller under the point (x, y) (viewport CSS px) that can move by (dx, dy), else the viewport.
 * Returns true when something moved and sets *key to the key of the scroller that moved. */
bool embi_doc_wheel(embi_doc *d, float x, float y, float dx, float dy, float vw, float vh, const void **key);
/* Scrolls the scroller with key by an absolute offset (the root for the document key). Returns true when it changed. */
bool embi_doc_scroll_key_by(embi_doc *d, const void *key, float dx, float dy, float vw, float vh);

/* Starts a smooth scroll of the viewport from its current offset to (x, y) at now_ms. */
void embi_doc_anim_start(embi_doc *d, embi_doc_anim *a, float x, float y, double now_ms, float vw, float vh);
/* Steps a running smooth scroll at now_ms. Returns true while it runs (the offset is set). */
bool embi_doc_anim_step(embi_doc *d, embi_doc_anim *a, double now_ms, float vw, float vh);

/* The alpha of a scrollbar that was last used at last_ms (seen false: never). */
/* Logs the fragment tree (kind, tag, border box, text byte range): a debugging aid (test_paint_reftest --dump). */
void embi_doc_dump_layout(const embi_doc *d);
float embi_doc_fade_alpha(double last_ms, bool seen, double now_ms);

/* The document's <title> text ("" when none). The pointer stays valid until the next load. */
const char *embi_doc_title(const embi_doc *d);

/* The current layout's counts for the stats: boxes and fragments. */
void embi_doc_counts(const embi_doc *d, uint32_t *boxes, uint32_t *frags);

/* The display list group decision of W4d (paint/layer.h), as the numbers of enum embi_group_mode: 0 empty, 1 fold,
 * 2 target. Used by the GPU side, which does not include the paint headers. */
int embi_doc_group_mode(const embi_dl *d, uint32_t begin, uint32_t end);
