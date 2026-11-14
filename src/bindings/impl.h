#pragma once
/*
 * Helpers shared by the hand-written implementations (src/bindings/impl_*.c). Private to the
 * bindings module.
 */
#include "bindings/bind.h"
#include "bindings/gen/types.h"
#include "dom/event.h"

/* The record of a node's wrapper (the wrapper is created on first use). */
static inline embi_bind_obj *embi_impl_node_rec(embi_bind_ctx *c, embi_node *n)
{
    (void)embi_bind_node_value(c, n);
    return (embi_bind_obj *)n->wrapper;
}

static inline embi_dstr embi_impl_lit(const char *s)
{
    return (embi_dstr){(const void *)s, (uint32_t)SDL_strlen(s), false};
}

/* A nullable DOMString from a DOM string, as a JS value. */
static inline embi_js_value embi_impl_dstr_or_null(embi_bind_ctx *c, bool present, embi_dstr d)
{
    return present ? embi_bind_dstr_value(c, d) : EMBI_JS_NULL;
}

/* Throws a DOMException by name and message and returns the "already thrown" marker. */
static inline embi_dom_err embi_impl_throw(embi_bind_ctx *c, const char *name, const char *msg)
{
    (void)embi_bind_throw_named(c, name, msg);
    return EMBI_BIND_THREW;
}

/* W6e: the client rectangle (x0, y0, x1, y1) of an SVG graphics element, which has no CSS box (impl_svg.c). */
#if EMBER_SVG
bool embi_impl_svg_client_bbox(embi_bind_ctx *c, embi_node *n, double r[4]);
#else
static inline bool embi_impl_svg_client_bbox(embi_bind_ctx *c, embi_node *n, double r[4])
{
    (void)c;
    (void)n;
    (void)r;
    return false;
}
#endif

/* ------------------------------------------------------------------ scrolling (impl_scroll.c) */

/* The viewport's scroll offset now (window.scrollX/scrollY, the root's scrollTop/scrollLeft, the origin of the client
 * rectangles). Also stores it in c->scroll_x and scroll_y. */
void embi_impl_root_offset(embi_bind_ctx *c, double *x, double *y);
/* Scrolls the viewport to (x, y), clamped to its scroll range (NaN and negative offsets are 0). */
void embi_impl_scroll_root_to(embi_bind_ctx *c, double x, double y);
/* Element.scrollIntoView: block and inl are EMBI_ENUM_ScrollLogicalPosition values (vertical and horizontal). */
void embi_impl_scroll_into_view(embi_bind_ctx *c, embi_element *el, int block, int inl);

/* ------------------------------------------------------------------ collections (impl_coll.c) */

enum {
    EMBI_COLL_CHILDNODES = 1, /* NodeList: the children of root */
    EMBI_COLL_CHILDREN,       /* HTMLCollection: the element children of root */
    EMBI_COLL_TAG,            /* getElementsByTagName(a) */
    EMBI_COLL_TAG_NS,         /* getElementsByTagNameNS(ns: a, local: b; has_ns says null or not) */
    EMBI_COLL_CLASS,          /* getElementsByClassName(a) */
    EMBI_COLL_IMAGES,
    EMBI_COLL_LINKS,
    EMBI_COLL_FORMS,
    EMBI_COLL_SCRIPTS,
    EMBI_COLL_EMBEDS,
    EMBI_COLL_ANCHORS,
    EMBI_COLL_ALL,
    EMBI_COLL_TBODIES,        /* table.tBodies: the tbody children of the table */
    EMBI_COLL_TABLE_ROWS,     /* table.rows: thead rows, then the tr children and tbody rows, then tfoot rows */
    EMBI_COLL_SECTION_ROWS,   /* tbody/thead/tfoot.rows: the tr children */
    EMBI_COLL_CELLS,          /* tr.cells: the td and th children */
};

/* A live collection over root. */
embi_js_value embi_impl_coll_new(embi_bind_ctx *c, int kind, embi_node *root, embi_str a,
                                 embi_str b, bool has_ns);
/* A static NodeList of the given nodes (querySelectorAll). */
embi_js_value embi_impl_nodelist_static(embi_bind_ctx *c, embi_node *const *nodes, uint32_t n);
/* The DOMTokenList over the attribute attr of the element record (classList, relList). */
embi_js_value embi_impl_tokenlist_new(embi_bind_ctx *c, embi_bind_obj *el, embi_str attr);
/* The NamedNodeMap of an element, and an Attr of one of its attributes. */
embi_js_value embi_impl_attrmap_new(embi_bind_ctx *c, embi_bind_obj *el);
embi_js_value embi_impl_attr_new(embi_bind_ctx *c, embi_bind_obj *el, const embi_attr *a);
/* The DOMStringMap (dataset) of an element. */
embi_js_value embi_impl_dataset_new(embi_bind_ctx *c, embi_bind_obj *el);
/* A DOMRect, and a DOMRectList of the given rectangles (x, y, w, h in client coordinates). */
embi_js_value embi_impl_rect_new(embi_bind_ctx *c, double x, double y, double w, double h);
embi_js_value embi_impl_rectlist_new(embi_bind_ctx *c, const double *rects, uint32_t n);
/* CSSStyleDeclaration views: element.style (inline, read/write) and getComputedStyle (read-only). */
embi_js_value embi_impl_style_inline(embi_bind_ctx *c, embi_bind_obj *el);
embi_js_value embi_impl_style_computed(embi_bind_ctx *c, embi_bind_obj *el);

/* ------------------------------------------------------------------ selectors and fragments */

/* Parses selectors (scratch memory of the scope) and runs queries. Return EMBI_BIND_THREW with a
 * SyntaxError pending when the selector is invalid. */
embi_dom_err embi_impl_query_first(embi_bind_ctx *c, embi_node *root, embi_element *scope,
                                   embi_dstr selectors, embi_node **out);
embi_dom_err embi_impl_query_all(embi_bind_ctx *c, embi_node *root, embi_element *scope,
                                 embi_dstr selectors, embi_js_value *out);
embi_dom_err embi_impl_matches(embi_bind_ctx *c, embi_element *el, embi_dstr selectors, bool *out);
embi_dom_err embi_impl_closest(embi_bind_ctx *c, embi_element *el, embi_dstr selectors,
                               embi_node **out);

/* Events (impl_events.c): fires a trusted event of type at target (event may be an Event wrapper, which is
 * dispatched as it is; otherwise a plain Event of type). Dispatch of a wrapper: embi_impl_dispatch_value. */
void embi_impl_fire(embi_bind_ctx *c, embi_bind_obj *target, const char *type, embi_js_value event);
/* Makes the window's handler of a window event from the body's content attribute (onload on body), if there is one. */
void embi_impl_sync_body_window_handler(embi_bind_ctx *c, const char *type);
bool embi_impl_dispatch_value(embi_bind_ctx *c, embi_bind_obj *target, embi_js_value ev_v);
/* A new Event wrapper of interface iface (EMBI_IF_*) of type; flags are EMBI_EVF_* (dom/event.h); payload is an
 * object of interface fields (or undefined for none). */
embi_js_value embi_impl_event_make(embi_bind_ctx *c, uint16_t iface, const char *type, uint32_t flags,
                                   embi_js_value payload);
/* Document.createEvent(name): an uninitialized event; NotSupportedError for unknown names. */
embi_dom_err embi_impl_create_event(embi_bind_ctx *c, embi_dstr name, embi_js_value *out);

/* The Attr node of attribute a of the element record el (made and cached on first use), its qualified name, and the
 * hook that detaches the Attr of a removed attribute (called by the context's observer). */
embi_attr_node *embi_impl_attr_for(embi_bind_ctx *c, embi_bind_obj *el, const embi_attr *a);
embi_dstr embi_impl_attr_qname(embi_bind_ctx *c, const embi_attr_node *an);
/* setAttributeNode: puts attr on the element; *out receives the Attr it replaced (nullptr if none). */
embi_dom_err embi_impl_set_attr_node(embi_bind_ctx *c, embi_node *self, embi_node *attr_n, embi_node **out);
/* Parses and sanitizes HTML for innerHTML-like setters; see impl_element.c. */

/* Registers a MediaQueryList whose change listeners have to hear about viewport changes. */
void embi_impl_mql_watch(embi_bind_ctx *c, embi_bind_obj *bo);

/* W6c (impl_anim.c): installs requestAnimationFrame, cancelAnimationFrame, Element.prototype.animate and getAnimations,
 * Document.prototype.timeline and getAnimations, and the lazy globals of the Web Animations classes. */
void embi_impl_anim_install(embi_bind_ctx *c);

/* W8b (impl_hit.c): Document.prototype.elementFromPoint and elementsFromPoint on the layout's hit test. */
void embi_impl_hit_install(embi_bind_ctx *c);
void embi_impl_css_ns_install(embi_bind_ctx *c);
/* cssom_shorthand.c (W6e): the computed value of the transition and animation shorthands; false for other names. */
typedef struct embi_style embi_style;
bool embi_impl_computed_shorthand(const embi_style *s, embi_str name, embi_sb *out);

/* Evaluates one of the prelude scripts embedded from src/bindings/js (the arrays in the impl_*.c files). Under
 * EMBER_COMPRESS_ASSETS the array is the tools/lz stream and is expanded for the call; otherwise it is the source plus a
 * NUL. Returns the script's function like embi_js_eval. */
embi_js_value embi_bind_eval_asset(embi_bind_ctx *c, const unsigned char *data, size_t size, const char *name);
