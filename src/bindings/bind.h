#pragma once
#ifndef EMBER_CANVAS
#error "EMBER_CANVAS must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * Binding runtime (W5a). The generated glue in src/bindings/gen/ and
 * the hand-written implementations in src/bindings/impl_*.c are built on these declarations.
 *
 * One binding context (embi_bind_ctx) per document and realm. It owns the realm, the prototypes of
 * every generated interface and a scratch arena. The context is stored on the realm's global
 * object (the Window wrapper's record), so there is no process-wide table: any number of contexts
 * can exist on any number of threads (a context, its runtime and its document stay on one thread).
 * Each native call opens a scope (embi_bind_enter) and closes it with embi_bind_leave, which frees
 * the call's scratch memory and unpins its temporary strings.
 *
 * Wrapper lifetime (the node model of the spec):
 *   - A node has at most one wrapper, cached in node->wrapper (an embi_bind_obj). It is created
 *     lazily and is a plain collectable JS object; the context holds no strong reference to it.
 *   - The wrapper keeps its node alive: the node is never freed while its wrapper lives.
 *   - The node keeps its wrapper alive (so expandos and identity survive) while the node is
 *     connected to a document (a root callback marks those wrappers), and while any wrapper in its
 *     detached tree is reachable (an ephemeron rule marks every wrapper of a tree once one of them
 *     is marked). A detached tree that nothing in JS refers to is therefore collectable.
 *   - When the wrapper of a detached tree dies, its finalizer frees the whole tree with
 *     embi_destroy_detached (the ownership rule of src/dom/dom.h).
 * Views (NodeList, DOMTokenList, CSSStyleDeclaration, ...) are non-node objects whose record names
 * an owner record (embi_bind_obj.owner); the owner's wrapper is kept alive by the view.
 * Lifetime rule: embi_bind_free before the runtime is freed, and the runtime before the document.
 * After embi_bind_free every wrapper is stale: reads return undefined and setters do nothing.
 *
 * Errors: functions that throw return EMBI_JS_EXCEPTION (value results) or false (bool results)
 * with the exception pending on the runtime, as the engine does.
 */
#include "base/base.h"
#include "dom/dom.h"
#include "js/js.h"

typedef struct embi_bind_ctx embi_bind_ctx;
typedef struct embi_bind_member embi_bind_member;
typedef struct embi_bind_obj embi_bind_obj;

/* The internal slot of a wrapper. ptr is the wrapped C object (an embi_node for node wrappers,
 * the implementation's own pointer otherwise, nullptr when the wrapper is stale). owned is C memory
 * the finalizer frees (owned_size bytes). same caches the [SameObject] values of the wrapper. */
struct embi_bind_obj {
    embi_js_object *obj;
    embi_bind_ctx *ctx;
    void *ptr;
    void *owned;
    uint32_t owned_size;
    uint32_t nsame;
    embi_js_value *same;
    embi_bind_obj *owner; /* views: the record of the object this one depends on (traced) */
    embi_js_value ref;    /* a traced value of the implementation (0 = none) */
    embi_js_value listeners; /* event listeners (impl_eventtarget.c), traced; 0 = none */
    uint32_t aux;         /* small implementation payload (a view kind, ...) */
    uint32_t widx;        /* index in ctx->wrapped, UINT32_MAX when the record is not listed */
    uint32_t seen;        /* GC epoch stamp of the tree grouping pass */
    uint16_t iface;       /* EMBI_IF_* of the most derived interface */
    bool is_node;         /* ptr is an embi_node */
    /* W7d: releases what owned names (a refcounted C object), called before owned is freed; nullptr: nothing to release. */
    void (*release)(void *owned);
};

/* True when the wrapper no longer refers to a live C object: embi_bind_free ran, or the owner of
 * a view went stale. Generated code returns undefined for stale [BindObj] receivers. */
static inline bool embi_bind_stale(const embi_bind_obj *bo)
{
    return bo->ptr == nullptr || (bo->owner && bo->owner->ptr == nullptr);
}

/* Event hooks the embedder gives the context (all optional). */
enum { EMBI_BIND_LOG_INFO = 0, EMBI_BIND_LOG_WARN, EMBI_BIND_LOG_ERROR, EMBI_BIND_LOG_DEBUG };
typedef struct embi_style_engine embi_style_engine;
typedef struct embi_layout_engine embi_layout_engine;
typedef struct embi_bind_host {
    void *user;
    /* console output besides embi_log_*. level is EMBI_BIND_LOG_*. */
    void (*log)(void *user, int level, embi_str text);
    /* location assignment and history.back/forward across documents. Returns true when handled. */
    bool (*navigate)(void *user, embi_str url, bool replace);
    /* W5e: a form submission: method ("GET" or "POST"), the absolute URL, the body of a POST and its content type. Returns
     * true when handled; false (or nullptr) falls back to navigate with the URL. */
    bool (*navigate_form)(void *user, embi_str method, embi_str url, embi_str body, embi_str content_type);
    /* innerWidth/innerHeight in CSS pixels and devicePixelRatio. */
    void (*viewport)(void *user, double *w, double *h, double *dpr);
    /* The document URL (default "about:blank") and the user's language (default "en-US"). Strings
     * must stay valid until the next call. */
    embi_str (*url)(void *user);
    embi_str (*language)(void *user);
    /* A monotonic clock in milliseconds for console.time (default: SDL ticks). */
    double (*now_ms)(void *user);
    /* Called after window.scrollTo/scrollBy with the new offsets. */
    void (*scroll)(void *user, double x, double y);
    /* Engines for getComputedStyle and the geometry members; nullptr leaves them empty/zero. */
    embi_style_engine *style;
    embi_layout_engine *layout;
    /* W5c: the document's time origin on the clock of now_ms (performance.timeOrigin, performance.now() = now_ms - origin);
     * default 0. */
    double (*time_origin_ms)(void *user);
    /* W5c: document.readyState: 0 loading, 1 interactive, 2 complete (default complete). */
    int (*ready_state)(void *user);
    /* W5c: an error nothing handled (no window "error" listener cancelled it), as a line of text for the log. */
    void (*uncaught)(void *user, embi_str text);
    /* W7d: document.adoptedStyleSheets changed. sheets are the adopted CSSStyleSheets (embi_css_sheet *, borrowed for the
     * call: the embedder refs what it keeps). Not called when nullptr. */
    void (*adopt)(void *user, void *const *sheets, uint32_t n);
    /* W6c: the document's animator (src/anim/frame.h): Element.animate and the animation objects. nullptr: none. */
    struct embi_animator *animator;
    /* R4c: the natural size of an image URL as written in the document (HTMLImageElement naturalWidth, naturalHeight and
     * complete). Returns true when the image loaded; loads are synchronous, so complete is true either way. nullptr: the
     * stub answers (0 x 0, complete when there is no src). */
    bool (*image_size)(void *user, embi_str url, float *w, float *h);
    /* W4g4b: the scroll offset of a scroll container element (scrollTop and scrollLeft). set: scroll to (*x, *y) first (clamped to
     * the scroll range). The offset after that is written to *x and *y. False when the element is not a scroll container. */
    bool (*scroll_el)(void *user, embi_element *el, bool set, double *x, double *y);
    /* W6e: called before the bindings read computed styles or geometry (embi_bind_style, embi_bind_layout): the embedder brings
     * the document's sheets up to date (a <style> inserted by script, or the parse still running). nullptr: none. */
    void (*sync_sheets)(void *user);
    /* N1d: document.cookie of a remote (http or https) document: the getter appends "a=1; b=2" (no HttpOnly cookies) to out
     * and returns true; the setter takes one Set-Cookie style string and returns true when it was handled. Both nullptr: the
     * document has no cookie jar (the getter gives "" and the setter does nothing). */
    bool (*cookie_get)(void *user, embi_sb *out);
    bool (*cookie_set)(void *user, embi_str cookie);
    /* N1d: whether the image at url (as written in the document) is still being fetched over the network
     * (HTMLImageElement.complete is false meanwhile). nullptr: images are complete. */
    bool (*image_pending)(void *user, embi_str url);
} embi_bind_host;

/* A sequence<T> argument or result: n elements of T's C type. */
typedef struct embi_bind_seq {
    void *data;
    uint32_t len;
} embi_bind_seq;

/* A record<K,V> argument or result: len keys and len values of V's C type. */
typedef struct embi_bind_rec {
    embi_dstr *keys;
    void *vals;
    uint32_t len;
} embi_bind_rec;

/* A nullable DOMString argument. */
typedef struct embi_bind_nstr {
    embi_dstr s;
    bool is_null;
} embi_bind_nstr;

/* Union member categories, in the order WebIDL tries them (see embi_bind_conv_union). */
enum {
    EMBI_UM_NODE = 0,   /* a Node interface (iface) */
    EMBI_UM_IFACE,      /* another platform interface (iface) */
    EMBI_UM_CALLBACK,   /* a callable */
    EMBI_UM_SEQ,        /* an object with @@iterator */
    EMBI_UM_OBJECT,     /* object, dictionary, record: any object */
    EMBI_UM_BOOL,
    EMBI_UM_NUM,        /* sub: embi_bind_conv_int kind, or 8 for double */
    EMBI_UM_STR,        /* DOMString, USVString, ByteString, enumerations */
    EMBI_UM_ANY,
};
typedef struct embi_bind_um {
    uint8_t cat;       /* EMBI_UM_* */
    uint8_t sub;
    bool nullable;     /* the member is a nullable type, or a dictionary (accepts undefined) */
    uint16_t iface;
} embi_bind_um;

/* A union value: kind is the 1-based index of the member that matched (0: null or undefined for a
 * nullable union or one with a dictionary member). Which field is set follows the member:
 * node (Node interfaces), ptr (other platform interfaces, dictionaries: an arena struct),
 * str (strings), num (numbers), b (boolean), v (callbacks, objects, sequences), i (enums). */
typedef struct embi_bind_union {
    uint8_t kind;
    union {
        embi_node *node;
        void *ptr;
        embi_dstr str;
        double num;
        int64_t i;
        bool b;
        embi_js_value v;
    } u;
} embi_bind_union;

/* One overload of an operation, for embi_bind_dispatch. */
typedef struct embi_bind_overload {
    embi_js_native fn;
    uint8_t min_args, max_args; /* max_args 255: variadic */
    uint8_t ncat;               /* categories described below */
    embi_bind_um cats[6];       /* the leading arguments */
} embi_bind_overload;

/* The payload of a DOMException. name and message point into the wrapper's owned block. */
typedef struct embi_bind_dom_exc {
    uint16_t code;
    embi_dstr name;
    embi_dstr message;
} embi_bind_dom_exc;

/* The operations of an iterable that is not backed by an indexed getter (generated). */
typedef struct embi_bind_iter_ops {
    uint16_t iface;
    bool pairs; /* iterable<K,V>: entries() is the default iterator, otherwise values() */
    uint32_t (*size)(embi_bind_ctx *c, embi_bind_obj *self);
    embi_js_value (*key)(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i);
    embi_js_value (*value)(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i);
} embi_bind_iter_ops;

/* Timer hooks the embedder gives the context (W5c, impl_timers.c): the embedder's event loop owns the timers. add
 * schedules a timer that fires after timeout_ms (repeating when repeat is set) and returns its id (> 0, 0 on failure);
 * payload is the context's record of the timer and comes back when it fires (see embi_bind_fire_timer). clear cancels
 * the timer id and returns its payload, or nullptr when there is no such timer. Both are optional: without them the
 * timer functions do nothing. */
typedef struct embi_bind_timers {
    void *user;
    uint32_t (*add)(void *user, double timeout_ms, bool repeat, void *payload);
    void *(*clear)(void *user, uint32_t id);
} embi_bind_timers;

/* ------------------------------------------------------------------ the context */

typedef struct embi_bind_counter {
    char *label;
    uint32_t len;
    uint32_t n;
    double start; /* console.time start (ms), valid when timing */
    bool timing;
} embi_bind_counter;

/* One history entry (W7d). The state is kept as the structured serialization of the value pushed (src/loader/clone.h):
 * history.state and popstate hand out a fresh copy, made from the bytes when first read, and cached in state (traced
 * through the context's root, 0 until then). has_state is false for an entry with no state (null). */
typedef struct embi_bind_hist {
    char *url;
    uint32_t url_len;
    embi_sb bytes;
    bool has_state;
    embi_js_value state;
} embi_bind_hist;

struct embi_bind_ctx {
    embi_js_runtime *rt;
    embi_js_realm *realm;
    embi_document *doc;
    embi_bind_obj *gbo;      /* the global's record */
    embi_js_value *protos;   /* EMBI_IF_COUNT prototypes, indexed by EMBI_IF_*, rooted */
    embi_js_value *pins;     /* temporaries kept alive for the current scopes */
    uint32_t npins, cap_pins;
    embi_arena arena;        /* scratch memory of the current scopes */
    embi_bind_obj **wrapped; /* records of node wrappers (not strong references) */
    uint32_t nwrapped, cap_wrapped;
    uint32_t epoch;          /* GC epoch, bumped by every root pass */
    embi_js_value win_event; /* window.event: the event whose listener runs (kept alive by the dispatch) */
    bool has_win_event;
    uint32_t mut_epoch;      /* bumped on every DOM mutation (collection caches) */
    const struct embi_svg_geometry_ops *svg_geom; /* R4d/R4c: the geometry of this context's SVG (svg_geometry.h), nullptr: none */
    void *svg_geom_user;     /* its first callback argument */
    void *canvas_host;       /* R4b: the canvas hooks the embedder installed (impl_canvas.c), nullptr: none */
    embi_bind_host host;
    embi_bind_counter *counters; /* console.count and console.time labels */
    uint32_t ncounters, cap_counters;
    embi_bind_hist *hist;    /* history entries */
    uint32_t nhist, cap_hist, hist_pos;
    double scroll_x, scroll_y;
    embi_js_value iter_proto;  /* the shared prototype of generated iterators, rooted */
    embi_js_value mqls;      /* JS array of the MediaQueryLists with change listeners, rooted */
    embi_js_value win_name;  /* window.name (a JS string), rooted */
    void *style_cache;       /* parsed style attribute of the last CSSStyleDeclaration access (impl_cssom.c) */
    uint32_t group_depth;    /* console.group nesting */
    embi_node *active;       /* document.activeElement (focus()), cleared when it leaves the tree */
    bool freed;
    bool testing;            /* the [Testing] interfaces are exposed on the global */
    embi_bind_timers timers; /* W5c: the embedder's timer hooks (impl_timers.c) */
    uint32_t perf_obj;       /* W5c: persistent handle plus one of window.performance, 0 if not made yet */
    uint32_t perf_entries;   /* W5c: persistent handle plus one of the performance.mark/measure entries array, 0 if none */
    uint32_t current_script; /* W5c: persistent handle plus one of document.currentScript's wrapper, 0 for null */
    uint32_t current_script_own; /* W5c: the pin made by embi_bind_set_current_script (freed at the next call), 0 if none */
    embi_document **xdocs;   /* W5f: documents made by script (new Document(), DOMImplementation, DOMParser), freed with the context */
    uint32_t nxdocs, cap_xdocs;
    struct embi_trav **iters; /* W5f: live NodeIterators (weak; impl_traversal.c) */
    uint32_t niters, cap_iters;
    uint32_t adopted;        /* W7d: persistent handle plus one of document.adoptedStyleSheets (impl_cssom_sheet.c), 0 if none */
    uint32_t css_sheet_fn;   /* W6e: persistent handle plus one of the function behind HTMLStyleElement.sheet (impl_css_ns.c) */
    uint32_t anim_exports;   /* W6c: persistent handle plus one of the animation prelude's exports (impl_anim.c), 0 before first use */
    uint32_t raf_count;      /* W6c: requestAnimationFrame callbacks waiting */
    bool anim_idle_tried;    /* CF2: embi_bind_idle has evaluated (or tried) the animation prelude */
    const embi_bind_member *site; /* the generated member being called (names it in error messages, see EMBI_CTX_ARG) */
    char ctx_buf[192];       /* the text embi_bind_ctx_text formats */
};

/* Scope of one native call. */
typedef struct embi_bind_scope {
    embi_bind_ctx *c;
    embi_arena_pos arena;
    uint32_t pins;
    const embi_bind_member *prev_site; /* embi_bind_ctx.site of the enclosing call, restored by embi_bind_leave */
    embi_js_value early;               /* embi_bind_begin: what the native returns when it gives up */
} embi_bind_scope;

/* ------------------------------------------------------------------ contexts */

/* Creates the binding context of doc in rt: the realm (global class Window), every generated
 * interface and namespace installed on it. Returns nullptr on failure. */
[[nodiscard]] embi_bind_ctx *embi_bind_new(embi_js_runtime *rt, embi_document *doc);
/* The first half of embi_bind_new: the realm and every interface, with no document yet (nothing here reads or
 * observes it), so it may run on another thread than the one that uses the context. embi_bind_attach_doc then binds the
 * document and starts observing it; a context freed before that has no document. FP2a. */
[[nodiscard]] embi_bind_ctx *embi_bind_new_detached(embi_js_runtime *rt);
void embi_bind_attach_doc(embi_bind_ctx *c, embi_document *doc);
/* Releases the realm and the scratch memory. Wrappers become stale: they stop referring to their
 * nodes. Detached subtrees that still have a wrapper are freed with the document. Call before
 * the runtime and the document are freed. */
void embi_bind_free(embi_bind_ctx *c);
/* Removes the loader state of c (impl_loader_state.c); a no-op when there is none. embi_bind_free calls it first. */
void embi_loader_detach(embi_bind_ctx *c);
embi_js_realm *embi_bind_realm(const embi_bind_ctx *c);
embi_document *embi_bind_document(const embi_bind_ctx *c);
embi_js_runtime *embi_bind_runtime(const embi_bind_ctx *c);
/* The context of a realm created by embi_bind_new (nullptr for other realms). */
embi_bind_ctx *embi_bind_ctx_of(embi_js_realm *r);
/* Installs the embedder hooks (copied). A zeroed host restores the defaults. */
void embi_bind_set_host(embi_bind_ctx *c, const embi_bind_host *h);
/* Defines the [Testing] interfaces (testing.webidl) on the global. For the unit tests only. */
void embi_bind_expose_testing(embi_bind_ctx *c);
/* Number of console.count calls for label, including this one. */
uint32_t embi_bind_console_count(embi_bind_ctx *c, embi_str label);
/* Resets the console.count counter of label. Returns false when it does not exist. */
bool embi_bind_console_count_reset(embi_bind_ctx *c, embi_str label);
/* Records the start of console.time(label), or returns the elapsed ms of console.timeEnd (< 0 when
 * the timer does not exist) and stops it. */
void embi_bind_timer_start(embi_bind_ctx *c, embi_str label);
double embi_bind_timer_end(embi_bind_ctx *c, embi_str label, bool stop);
double embi_bind_now_ms(embi_bind_ctx *c);
/* Writes a console line to the host hook and embi_log_*. */
void embi_bind_log(embi_bind_ctx *c, int level, embi_str text);

/* Installs the timer hooks (copied). A zeroed struct removes them. */
void embi_bind_set_timers(embi_bind_ctx *c, const embi_bind_timers *t);
/* Runs the callback of a timer fired by the embedder's loop. payload is the value the add hook was given. Returns the
 * callback's result, or EMBI_JS_EXCEPTION with the exception pending. A timer whose callback has been released returns
 * undefined. */
embi_js_value embi_bind_fire_timer(embi_bind_ctx *c, void *payload);
/* Releases the record of a timer (the embedder calls it when a one-shot timer has fired or a timer was cleared). */
void embi_bind_release_timer(embi_bind_ctx *c, void *payload);
/* Fires a document lifecycle event (W5c): "DOMContentLoaded" at the document, "load" at the window. The listeners get a trusted Event
 * object (W5b). Listeners that throw are reported like other uncaught errors. */
void embi_bind_fire_lifecycle(embi_bind_ctx *c, const char *type);
/* Fires a plain trusted event (no bubbling) of the given type at a node: the load and error of an <img> (N1d). */
void embi_bind_fire_node_event(embi_bind_ctx *c, embi_node *node, const char *type);
/* document.currentScript (W5c): sets it to the wrapper of node (a script element), or to null for nullptr. The wrapper is
 * kept alive, and with it the element, until the next call. */
void embi_bind_set_current_script(embi_bind_ctx *c, embi_node *node);
/* Pins the wrapper of node (so the element outlives removal from the tree) and returns a token (never 0, or 0 on failure).
 * embi_bind_set_current_script_token makes the pinned element the current script; embi_bind_unpin_node releases the pin. */
uint32_t embi_bind_pin_node(embi_bind_ctx *c, embi_node *node);
void embi_bind_set_current_script_token(embi_bind_ctx *c, uint32_t token);
void embi_bind_unpin_node(embi_bind_ctx *c, uint32_t token);
/* Reports an error value like an uncaught exception (W5c): an ErrorEvent "error" at the window; when no listener cancels
 * it, the host's uncaught hook gets "<message> (<file>:<line>:<col>)" (the location only when file is not nullptr). The
 * exception must not be pending. */
void embi_bind_report_error(embi_bind_ctx *c, embi_js_value error, const char *file, uint32_t line, uint32_t col);
/* W6c (impl_anim.c): the animator reports a promise or event of the animation with this id (EMBI_ANN_*), a CSS animation or
 * transition event for el (EMBI_CEV_*), and the frame runs the requestAnimationFrame callbacks queued so far with the
 * frame's timestamp (each callback is followed by a microtask checkpoint; errors are reported like other uncaught errors).
 * All are no-ops until a script uses the animation API. */
void embi_bind_anim_notify(embi_bind_ctx *c, uint32_t id, int what, double time_ms);
void embi_bind_anim_css_event(embi_bind_ctx *c, struct embi_element *el, int kind, embi_str name, double elapsed);
void embi_bind_anim_checkpoint(embi_bind_ctx *c);
/* CF2: one step of work ahead of need while the view is idle (the embedder's frame had nothing to do and no script is
 * running). anim_soon says the document's styles declare a CSS transition or animation: the animation prelude their events
 * need is evaluated now (once), instead of in the frame of the first event. Otherwise a collection runs when half the
 * allocation budget is spent (embi_js_gc_idle). Returns true when it did something. */
bool embi_bind_idle(embi_bind_ctx *c, bool anim_soon);
void embi_bind_run_animation_frame(embi_bind_ctx *c, double timestamp_ms);
bool embi_bind_raf_pending(const embi_bind_ctx *c);
/* Viewport and device pixel ratio from the host (1024x768 at 1x without one). */
void embi_bind_viewport(embi_bind_ctx *c, double *w, double *h, double *dpr);
embi_str embi_bind_language(embi_bind_ctx *c);
embi_str embi_bind_url(embi_bind_ctx *c);
/* Flushes style and layout (when the host has engines) so geometry reads are current. */
embi_layout_engine *embi_bind_layout(embi_bind_ctx *c);
embi_style_engine *embi_bind_style(embi_bind_ctx *c);

/* ------------------------------------------------------------------ scopes */

embi_bind_scope embi_bind_enter(embi_bind_ctx *c);
embi_js_value embi_bind_leave(embi_bind_scope sc, embi_js_value v);
/* embi_bind_leave for generated code: the scope is passed by pointer (smaller code). */
embi_js_value embi_bind_end(const embi_bind_scope *sc, embi_js_value v);
/* A JS number (out of line, so that generated getters stay small). */
embi_js_value embi_bind_num(double v);
/* Keeps v alive until the scope ends. */
embi_js_value embi_bind_pin(embi_bind_ctx *c, embi_js_value v);

/* ------------------------------------------------------------------ strings */

/* A DOM string holding a copy of the UTF-8 text, valid until the scope ends. */
embi_dstr embi_bind_tmp_utf8(embi_bind_ctx *c, embi_str text);
/* A DOM string of n UTF-16 units (narrowed to bytes when they all fit), valid until the scope ends. */
embi_dstr embi_bind_tmp_units(embi_bind_ctx *c, const uint16_t *u, uint32_t n);
/* n bytes of scratch memory, valid until the scope ends. */
char *embi_bind_alloc(embi_bind_ctx *c, size_t n);
/* A copy of d, valid until the scope ends. */
embi_dstr embi_bind_tmp_copy(embi_bind_ctx *c, embi_dstr d);
/* The UTF-8 text of d, valid until the scope ends. */
embi_str embi_bind_utf8(embi_bind_ctx *c, embi_dstr d);
/* A JS string value with the contents of d. */
embi_js_value embi_bind_dstr_value(embi_bind_ctx *c, embi_dstr d);
/* A JS string value with UTF-8 text. */
embi_js_value embi_bind_utf8_value(embi_bind_ctx *c, embi_str s);
/* The DOMString of any JS value (ToString), pinned until the scope ends. */
bool embi_bind_to_dstr(embi_bind_ctx *c, embi_js_value v, embi_dstr *out);

/* ------------------------------------------------------------------ wrappers */

/* The wrapper of a node (created on first use), or null for nullptr. */
embi_js_value embi_bind_node_value(embi_bind_ctx *c, embi_node *n);
/* A wrapper that refers to a document-lifetime C object, with no tracking: ptr may be anything the
 * implementation understands, and owned (owned_size bytes, may be nullptr) is freed by the
 * finalizer. */
embi_js_value embi_bind_other_value(embi_bind_ctx *c, uint16_t iface, void *ptr, void *owned,
                                    uint32_t owned_size);
/* A view of owner (the record of a wrapper): a wrapper whose ptr is the owner's record, kept alive
 * together with the owner. owned (may be nullptr) is freed by the finalizer. aux is stored. */
embi_js_value embi_bind_view_new(embi_bind_ctx *c, uint16_t iface, embi_bind_obj *owner,
                                 uint32_t aux, void *owned, uint32_t owned_size);
/* The record of a wrapper made by this runtime (a Node or any generated interface), or nullptr. */
embi_bind_obj *embi_bind_obj_of(embi_js_value v);
/* The wrapper object of a record. */
embi_js_value embi_bind_obj_value(const embi_bind_obj *bo);
/* The wrapper of this value when it is an instance of iface (the brand check). */
bool embi_bind_this(embi_bind_ctx *c, embi_js_value this_v, int iface, embi_bind_obj **out);
/* True when interface have is want or inherits from it. */
bool embi_bind_iface_is(uint16_t have, int want);
/* The [SameObject] cache slot of a wrapper. */
embi_js_value *embi_bind_same(embi_bind_obj *bo, uint32_t slot);
/* The node of an owner record (nullptr when stale). */
static inline embi_node *embi_bind_owner_node(const embi_bind_obj *bo)
{
    return (bo->owner && bo->owner->ptr && bo->owner->is_node) ? bo->owner->ptr : nullptr;
}
/* Stores v in the traced slot of bo (with the write barrier). */
void embi_bind_set_ref(embi_bind_obj *bo, embi_js_value v);
void embi_bind_set_listeners(embi_bind_obj *bo, embi_js_value v);
/* The most derived interface for an element (the tag table), used by the wrapper factory. */
uint16_t embi_bind_iface_for_tag(uint8_t ns, embi_atom local);

/* ------------------------------------------------------------------ values and errors */

embi_js_value embi_bind_enum_value(embi_bind_ctx *c, const char *const *names, int count,
                                   int32_t v);
/* A DOMException with the legacy code of err, pending on the runtime. */
embi_js_value embi_bind_throw_dom(embi_bind_ctx *c, embi_dom_err err);
/* A DOMException of the given name and message (the code follows the name), pending. */
embi_js_value embi_bind_throw_named(embi_bind_ctx *c, const char *name, const char *msg);
/* A DOMException with the given code, name and message (copied into the wrapper's payload). */
embi_js_value embi_bind_dom_exception(embi_bind_ctx *c, uint16_t code, embi_dstr name,
                                      embi_dstr message);
/* The legacy code of a DOMException name (0 when it has none). */
uint16_t embi_bind_legacy_code(embi_dstr name);
/* A TypeError for a call with fewer than required arguments. */
embi_js_value embi_bind_throw_args(embi_bind_ctx *c, const char *ctx, int required, int argc);
/* A scratch array of n union values (nullptr when n is 0), freed by the scope. */
embi_bind_union *embi_bind_union_array(embi_bind_ctx *c, int n);
/* Arrays and plain objects built from C. */
embi_js_value embi_bind_array_new(embi_bind_ctx *c, uint32_t len);
void embi_bind_array_set(embi_bind_ctx *c, embi_js_value arr, uint32_t i, embi_js_value v);
embi_js_value embi_bind_array_get(embi_bind_ctx *c, embi_js_value arr, uint32_t i);
uint32_t embi_bind_array_len(embi_bind_ctx *c, embi_js_value arr);
embi_js_value embi_bind_object_new(embi_bind_ctx *c);
void embi_bind_object_set(embi_bind_ctx *c, embi_js_value obj, embi_dstr key, embi_js_value v);
void embi_bind_object_set_c(embi_bind_ctx *c, embi_js_value obj, const char *key, embi_js_value v);
/* Promises: resolved with v, rejected with v, or rejected with the pending exception (which is
 * cleared). */
embi_js_value embi_bind_promise_resolved(embi_bind_ctx *c, embi_js_value v);
embi_js_value embi_bind_promise_rejected(embi_bind_ctx *c, embi_js_value reason);
embi_js_value embi_bind_promise_from_pending(embi_bind_ctx *c);

/* ------------------------------------------------------------------ argument conversion */

/* kind: 0 int8, 1 int16, 2 int32, 3 int64, 4 uint8, 5 uint16, 6 uint32, 7 uint64, plus
 * 0x10 for [Clamp] and 0x20 for [EnforceRange]. *out holds the value; for the 64-bit unsigned
 * kind it holds the two's complement bit pattern (cast it to uint64_t). */
bool embi_bind_conv_int(embi_bind_ctx *c, embi_js_value v, int kind, const char *ctx,
                        int64_t *out);
bool embi_bind_conv_double(embi_bind_ctx *c, embi_js_value v, bool unrestricted, const char *ctx,
                           double *out);
bool embi_bind_conv_object(embi_bind_ctx *c, embi_js_value v, bool nullable, const char *ctx,
                           embi_js_value *out);
bool embi_bind_conv_callback(embi_bind_ctx *c, embi_js_value v, bool nullable, bool iface_obj,
                             const char *ctx, embi_js_value *out);
bool embi_bind_conv_str(embi_bind_ctx *c, embi_js_value v, const char *ctx, bool legacy_nte,
                        embi_dstr *out);
bool embi_bind_conv_nstr(embi_bind_ctx *c, embi_js_value v, const char *ctx, bool legacy_nte,
                         embi_bind_nstr *out);
/* *out is the index of the name, or -1 when v is null/undefined and nullable. */
bool embi_bind_conv_enum(embi_bind_ctx *c, embi_js_value v, bool nullable,
                         const char *const *names, int count, const char *ctx, int32_t *out);
bool embi_bind_conv_node(embi_bind_ctx *c, embi_js_value v, int iface, bool nullable,
                         const char *ctx, embi_node **out);
bool embi_bind_conv_other(embi_bind_ctx *c, embi_js_value v, int iface, bool nullable,
                          const char *ctx, void **out);
bool embi_bind_conv_union(embi_bind_ctx *c, embi_js_value v, const embi_bind_um *members,
                          int nmembers, bool nullable, const char *ctx, embi_bind_union *out);
/* Sequence and record arguments: the JS values of the elements (pinned until the scope ends). */
bool embi_bind_seq_open(embi_bind_ctx *c, embi_js_value v, const char *ctx, embi_js_value **items,
                        uint32_t *n);
bool embi_bind_rec_open(embi_bind_ctx *c, embi_js_value v, const char *ctx, embi_dstr **keys,
                        embi_js_value **vals, uint32_t *n);
/* True when v can be the argument of an overload category (overload resolution). */
bool embi_bind_arg_is(embi_bind_ctx *c, embi_js_value v, const embi_bind_um *m);
/* Picks and calls the overload of an operation for the call's arguments. */
embi_js_value embi_bind_dispatch(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, const embi_bind_overload *ov, int nov,
                                 const char *name);

/* Dictionaries: check that v is an object or nullish, read a member (undefined when v is not an
 * object), and report a missing required member. */
bool embi_bind_dict_check(embi_bind_ctx *c, embi_js_value v, const char *ctx);
bool embi_bind_dict_get(embi_bind_ctx *c, embi_js_value v, const char *name, embi_js_value *out);
bool embi_bind_dict_required(embi_bind_ctx *c, const char *name, const char *ctx);

/* [Replaceable] and [PutForwards] setters. */
bool embi_bind_replace_prop(embi_bind_ctx *c, embi_js_value this_v, const char *name,
                            embi_js_value v);
bool embi_bind_put_forwards(embi_bind_ctx *c, embi_js_value this_v, const char *attr,
                            const char *forward, embi_js_value v);

/* ------------------------------------------------------------------ reflected attributes */

bool embi_bind_reflect_get_bool(embi_element *el, embi_str name);
uint32_t embi_bind_reflect_get_ulong(embi_element *el, embi_str name);
int32_t embi_bind_reflect_get_long(embi_element *el, embi_str name, int32_t def);
double embi_bind_reflect_get_double(embi_element *el, embi_str name, double def);
embi_js_value embi_bind_reflect_get_str(embi_bind_ctx *c, embi_element *el, embi_str name);
void embi_bind_reflect_set_bool(embi_element *el, embi_str name, bool v);
void embi_bind_reflect_set_ulong(embi_element *el, embi_str name, uint32_t v);
void embi_bind_reflect_set_long(embi_element *el, embi_str name, int32_t v);
void embi_bind_reflect_set_double(embi_element *el, embi_str name, double v);
void embi_bind_reflect_set_str(embi_bind_ctx *c, embi_element *el, embi_str name, embi_dstr v);

/* ------------------------------------------------------------------ iterables */

/* Generated entry points of non-indexed iterables (data is an embi_bind_iter_ops, magic the kind:
 * 0 keys, 1 values, 2 entries) and of forEach. */
embi_js_value embi_bind_iter_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic);
embi_js_value embi_bind_foreach_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic);
/* Defines keys, values, entries, forEach and @@iterator on proto for the ops (generated). */
void embi_bind_install_iterable(embi_bind_ctx *c, embi_js_object *proto,
                                const embi_bind_iter_ops *ops);
/* For iterable<V> on an interface with an indexed getter and length: the Array.prototype
 * functions (WebIDL "iterable declaration"). */
void embi_bind_install_array_iterable(embi_bind_ctx *c, embi_js_object *proto, bool symbol_only);
/* Sets proto's @@toStringTag and the constructor's [[Prototype]] (the parent's interface object). */
void embi_bind_finish_iface(embi_bind_ctx *c, embi_js_object *proto, embi_js_object *ctor,
                            const char *name, embi_js_value parent_proto);
void embi_bind_finish_ns(embi_bind_ctx *c, embi_js_object *ns, const char *name);
/* CSS property accessors (generated from src/css/props.txt): defines the dashed and camelCase
 * accessor pair of property index idx on proto. */
void embi_bind_define_css_prop(embi_bind_ctx *c, embi_js_object *proto, const char *dashed,
                               const char *camel, int idx, embi_js_native getter,
                               embi_js_native setter);

/* ------------------------------------------------------------------ class hooks */

/* Named (non-index) own properties of a wrapper: the ordinary lookup. Used by the generated
 * indexed-getter hook. Returns EMBI_JS_YES or EMBI_JS_NO (EMBI_JS_NO is 0). */
int embi_bind_own_named(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                        embi_js_pdesc *out);
/* True when the prototype chain of o has a property k (the named property visibility algorithm). */
bool embi_bind_proto_has(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* The ordinary [[OwnPropertyKeys]] of o (the class hook has to call this to list named keys). */
int embi_bind_own_keys_ordinary(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out);
void embi_bind_trace(embi_js_object *o, embi_js_tracer *t);
void embi_bind_finalize(embi_js_object *o);

/* impl_window.c: the named properties object of the Window (ids as global names), between Window.prototype and
 * EventTarget.prototype. */
void embi_impl_window_install_named(embi_bind_ctx *c);
/* W5f: registers a document made by script with the context: observed like the main document, freed by embi_bind_free. */
void embi_bind_add_xdoc(embi_bind_ctx *c, embi_document *doc);
void embi_bind_observe_doc(embi_bind_ctx *c, embi_document *doc);
/* impl_traversal.c: the removing steps of NodeIterator (an embi_observer pre_remove hook), and the release of every live
 * iterator's link to the context (embi_bind_free). */
void embi_impl_trav_pre_remove(void *ctx, embi_node *node);
void embi_impl_trav_detach_all(embi_bind_ctx *c);
void embi_impl_trav_drop_dying(embi_bind_ctx *c);
/* impl_coll.c: an attribute of el changed or was removed (detaches its Attr object). */
void embi_impl_attr_changed(embi_bind_ctx *c, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local,
                            embi_dstr old_value, bool had_old);
/* R4b (impl_canvas.c): a canvas width or height attribute changed (resets the bitmap); the hooks of the context are released. */
void embi_impl_canvas_attr_changed(embi_bind_ctx *c, embi_element *el, embi_atom local);
void embi_impl_canvas_release(embi_bind_ctx *c);

/* Mutation counter for collection caches. */
static inline uint32_t embi_bind_mut_epoch(const embi_bind_ctx *c)
{
    return c->mut_epoch;
}

/* ------------------------------------------------------------------ generated-code helpers */

/* ------------------------------------------------------------------ generated members */

/* Every generated attribute, operation and constant is one row of a const table (embi_bind_member),
 * and the installer (embi_bind_install_members) makes the JS functions from the rows. The native
 * functions of rows are shared: tools/idlgen writes one (gen/shapes.c) per distinct body, and a
 * native finds its own row as the data of the function being called (r->cur_data). So the
 * implementation, the member's name and the interface in error messages are data, not code. */
typedef void (*embi_gen_fn)(void);
enum {
    EMBI_MEMBER_OP = 0,   /* a method (fn); static operations and namespace members too */
    EMBI_MEMBER_SOP,      /* a static method, defined on the interface object */
    EMBI_MEMBER_ATTR,     /* an accessor (fn getter, set setter or nullptr) */
    EMBI_MEMBER_CONST,    /* a constant, on the prototype and the interface object (num) */
    EMBI_MEMBER_ITER,     /* keys, values, entries, forEach and @@iterator of a non-indexed iterable (data: embi_bind_iter_ops) */
    EMBI_MEMBER_ARRAY_ITER, /* the Array.prototype functions of an indexed iterable (flags EMBI_MF_SYMBOL_ONLY) */
    EMBI_MEMBER_CSSPROPS, /* the accessors of every CSS property (fn getter, set setter, magic the property index) */
};
enum {
    EMBI_MF_UNFORGEABLE = 1, /* [LegacyUnforgeable]: not configurable */
    EMBI_MF_NS = 2,          /* iface indexes embi_bind_ns_names (a namespace member), not embi_bind_names */
    EMBI_MF_SYMBOL_ONLY = 4, /* EMBI_MEMBER_ARRAY_ITER: only @@iterator */
};
struct embi_bind_member {
    const char *name;
    union {
        embi_gen_fn impl; /* the implementation (getter) the native calls, typed by its shape */
        double num;       /* constants, and the default of a reflected number */
        const void *data; /* EMBI_MEMBER_ITER */
    };
    union {
        embi_gen_fn impl2;  /* the setter implementation */
        const char *attr;   /* reflected attributes: the content attribute name (lower case) */
    };
    embi_js_native fn;  /* the method, or the getter */
    embi_js_native set; /* the setter, nullptr for none */
    uint16_t iface;     /* the interface (EMBI_IF_*) or namespace the member is declared on */
    uint8_t kind;       /* EMBI_MEMBER_* */
    uint8_t len;        /* the length of a method */
    uint8_t flags;      /* EMBI_MF_* */
};
/* One interface (or namespace): its members in definition order, the [LegacyUnforgeable] ones
 * that every instance gets, and what the installer needs to make the prototype and the
 * interface object. The parent, the name and the [Testing] flag are in the tables of ifaces.c. */
enum {
    EMBI_IFACE_ERROR = 1,  /* the prototype's parent is Error.prototype */
    EMBI_IFACE_GLOBAL = 2, /* [Global]: an object for the named properties sits between the prototype and its parent */
};
typedef struct embi_bind_iface {
    const embi_bind_member *members;
    const embi_bind_member *unforge;
    embi_js_native_ctor ctor; /* nullptr: Illegal constructor */
    uint16_t nmembers, nunforge;
    uint8_t ctor_len;
    uint8_t flags;
} embi_bind_iface;
/* Creates (once) the prototype and the interface object of interface id and its parents, and
 * defines the interface on the global. Returns the prototype. */
embi_js_object *embi_bind_install_iface(embi_bind_ctx *c, int id);
/* Creates the object of namespace k. */
embi_js_object *embi_bind_install_ns(embi_bind_ctx *c, int k);

/* Defines the members rows[0..n) on proto (methods and accessors), on ctor (static methods) or on
 * both (constants), in order. */
void embi_bind_install_members(embi_bind_ctx *c, embi_js_object *proto, embi_js_object *ctor,
                               const embi_bind_member *rows, size_t n);

/* What a native expects of its receiver (embi_bind_begin). */
enum {
    EMBI_SELF_NODE = 0, /* a Node wrapper: the native gets bo->ptr, a stale wrapper answers undefined */
    EMBI_SELF_OBJ = 1,  /* a wrapper record ([BindObj]): stale when embi_bind_stale */
    EMBI_SELF_PTR = 2,  /* another wrapper: bo->ptr, undefined when it is null */
    EMBI_SELF_NONE = 3, /* static members and namespaces: no receiver */
    EMBI_SELF_GLOBAL = 8, /* [Global]: an undefined or null receiver means the global object */
};
/* The start of a generated native: finds the binding context, opens the scope sc, makes m the
 * member named in conversion errors, brand-checks the receiver against m->iface and stores its
 * wrapper in *bo. Returns the context, or nullptr when the native must return sc->early (an
 * exception, or undefined for a stale receiver); the scope is closed then. */
embi_bind_ctx *embi_bind_begin(embi_js_realm *r, embi_js_value this_v, const embi_bind_member *m,
                               unsigned how, embi_bind_scope *sc, embi_bind_obj **bo);
/* Error contexts of generated conversions. A context is a plain string for hand-written callers.
 * The generated code passes EMBI_CTX_ARG(n), a marker (first byte 1) that embi_bind_ctx_text
 * expands from the member being called at the time of the error: "Failed to execute 'm' on 'I':
 * parameter n" (n 0: without the parameter), or EMBI_CTX_SET for "Failed to set the 'm' property
 * on 'I'". */
#define EMBI_CTX_ARG(n) "\001e" #n
#define EMBI_CTX_SET "\001s"
/* The same for a constructor: "Failed to construct 'I': parameter n" (n 0: without the parameter), I
 * being the interface of the member. */
#define EMBI_CTX_CTOR(n) "\001c" #n
const char *embi_bind_ctx_text(embi_bind_ctx *c, const char *ctx);
/* The TypeError of a constructor called without new; site is the constructor's row (only .iface). */
embi_js_value embi_bind_ctor_needs_new(embi_js_realm *r, const embi_bind_member *site);
/* The native of an interface without a constructor. */
embi_js_value embi_bind_illegal_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic, embi_js_value new_target);
/* Natives of reflected content attributes (the row's attr is the content attribute name, num the
 * default of the number kinds). */
embi_js_value embi_bind_refl_get_str(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_str(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_get_nstr(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_nstr(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_get_bool(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_bool(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_get_long(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_long(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_get_ulong(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_ulong(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_get_double(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
embi_js_value embi_bind_refl_set_double(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);

/* Defines an operation on obj: a function named name with the given length, enumerable like the
 * WebIDL operations of interfaces (writable, configurable); enumerable false for [LegacyUnforgeable]
 * members, which are also non-configurable. */
void embi_bind_define_op(embi_bind_ctx *c, embi_js_object *obj, const char *name, embi_js_native fn,
                         int length, bool configurable);
/* A nullable DOMString as a JS value. */
static inline embi_js_value embi_bind_nstr_value(embi_bind_ctx *c, embi_bind_nstr s)
{
    return s.is_null ? EMBI_JS_NULL : embi_bind_dstr_value(c, s.s);
}

/* An implementation that has already thrown (embi_bind_throw_named, ...) returns this from a
 * [Throws] function: the generated code leaves the exception as it is. */
#define EMBI_BIND_THREW ((embi_dom_err)-1)
/* Gives the object v the prototype of new_target.prototype (constructors called by a subclass). */
void embi_bind_adopt_new_target(embi_bind_ctx *c, embi_js_value v, embi_js_value new_target);
/* Defines the global Image factory (impl_html.c); called once the interfaces are installed. */
void embi_impl_html_install_image(embi_bind_ctx *c);
/* Frees the CSSStyleDeclaration parse cache (impl_cssom.c). */
void embi_impl_style_cache_free(embi_bind_ctx *c);
/* The UTF-8 text of a JS property key (index keys are formatted). Valid until the scope ends. */
embi_str embi_bind_key_utf8(embi_bind_ctx *c, embi_js_key k);
/* A property key from UTF-8 text. */
embi_js_key embi_bind_key_from_utf8(embi_bind_ctx *c, embi_str s);

/* Re-evaluates the MediaQueryLists that have change listeners after the viewport changed, and fires "change" on those whose result changed. */
void embi_bind_viewport_changed(embi_bind_ctx *c);
