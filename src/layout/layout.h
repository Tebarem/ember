#pragma once
/*
 * Layout (W3a): the box tree, the layout driver, sizing, geometry and block flow. Internal to
 * Ember: paint (W4) turns the fragment tree into a display list, and the CSSOM bindings (W5) call
 * the geometry functions here.
 *
 * Model. embi_layout_engine is per view. It owns, for every document it lays out, a box tree and a
 * fragment tree in two arenas. Each layout rebuilds both from the DOM and the computed styles (W2),
 * so a previous layout's boxes and fragments are invalid after the next embi_layout_document call.
 * Element boxes are reached through the element's `box` slot.
 *
 * Coordinates. Fragment positions are CSS px (float), relative to the parent fragment's border-box
 * origin. The root fragment (the initial containing block) sits at (0, 0). Geometry functions
 * return document coordinates, which add up the positions along the fragment chain.
 *
 * Scope of W3a. Block flow with margin collapsing, width and height sizing, replaced boxes, and one
 * unbroken line per inline formatting context. Floats, positioning, flex, grid, tables and line
 * breaking are later tasks. Those boxes are laid out as blocks for now, and
 * the expectation files under tests/layout/expectations list what fails.
 */
#include "base/base.h"
#include "dom/dom.h"
#include "layout/fragment.h"
#include "style/style.h"
#include "layout/text_shim.h"

typedef struct embi_layout_engine embi_layout_engine;

/* Natural size of an image. Returns false when the URL cannot be sized. has_ratio is set when the
 * size gives an aspect ratio (it is then the natural ratio). */
typedef bool (*embi_layout_image_size_fn)(void *user, embi_str url, float *w, float *h,
                                          bool *has_ratio);

typedef struct embi_layout_config {
    void *user;
    embi_layout_image_size_fn image_size; /* may be nullptr */
    float scrollbar_thickness;            /* 0 for overlay scrollbars */
} embi_layout_config;

typedef struct embi_rectf {
    float x, y, w, h;
} embi_rectf;

/* text and style are borrowed and must outlive the engine. style is the style engine whose
 * documents are laid out. cfg may be nullptr. Destroying the engine clears the box slots of the
 * documents it laid out, so those documents must still be alive then (forget them first, or
 * destroy the engine before freeing them). A document is owned by one layout engine at a time:
 * the element box slots are per element, so laying out, forgetting or destroying in a second
 * engine clears the first engine's slots and its geometry queries then fail (W3h may store the
 * owner in the document state if shared documents are needed). */
embi_layout_engine *embi_layout_engine_create(embi_text *text, embi_style_engine *style,
                                              const embi_layout_config *cfg);
void embi_layout_engine_destroy(embi_layout_engine *e);
/* The viewport (initial containing block) size in CSS px. Default 800x600. */
void embi_layout_set_viewport(embi_layout_engine *e, float w, float h);

/* Resolves the document's styles if needed, builds its boxes and lays them out. Returns false on
 * allocation failure, or when a page nested deeper than 40 levels needs a 32 MiB layout thread and the
 * platform cannot start one (logged; the layout never runs with a lower depth cap). A document with no root element gets an empty fragment tree. */
bool embi_layout_document(embi_layout_engine *e, embi_document *doc);
/* Frees the document's boxes and fragments. */
void embi_layout_forget_document(embi_layout_engine *e, embi_document *doc);
/* The root fragment (the initial containing block) of the last layout, or nullptr. */
const embi_frag *embi_layout_root(const embi_layout_engine *e, const embi_document *doc);
/* The style engine of the layout engine (paint reads the ::first-line style of a block through it). */
embi_style_engine *embi_layout_style_engine(const embi_layout_engine *e);
/* W5e: the hook that tells the painter the dynamic state of a form control (value, selection, checkedness ...). out is an
 * embi_paint_ctl (paint/paint.h). Without a hook the painter reads the attributes. */
typedef bool (*embi_layout_control_fn)(void *user, const embi_element *el, void *out);
void embi_layout_set_control_hook(embi_layout_engine *e, embi_layout_control_fn fn, void *user);
embi_layout_control_fn embi_layout_control_hook(const embi_layout_engine *e, void **user);
/* The text system the engine shapes with (W5e: the painter shapes the text of form controls with it). */
embi_text *embi_layout_text(const embi_layout_engine *e);

/* ------------------------------------------------------------------ geometry (CSSOM View) */

/* The border box of an element in document coordinates. For an inline split across lines it is
 * the union of its pieces (getBoundingClientRect). False when the element has no box. */
bool embi_layout_border_box(const embi_layout_engine *e, const embi_element *el, embi_rectf *out);
/* The client rects of an element (one per fragment, document coordinates). Returns the count
 * written (at most cap). */
uint32_t embi_layout_client_rects(const embi_layout_engine *e, const embi_element *el,
                                  embi_rectf *out, uint32_t cap);
/* W6e: the same with the transforms of the element and its ancestors applied (the rectangles it paints at: the bounding box of
 * each transformed quad). Without a transform in the document they equal the two functions above. */
bool embi_layout_border_box_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out);
uint32_t embi_layout_client_rects_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out, uint32_t cap);
/* The border box where it is painted (scrollIntoView, the pointer offsets, the caret and every caller that turns a box
 * into viewport coordinates by subtracting the viewport's scroll offset): moved by the scroll offsets of the scroll
 * containers around the element, and for a position: fixed box of the viewport (and its content) moved by the viewport's
 * offset, which does not move it on screen (so the caller's subtraction leaves it in place). Equal to
 * embi_layout_border_box when nothing is scrolled. The _xf forms apply the transforms as the _xf functions above do
 * (getBoundingClientRect and getClientRects). */
bool embi_layout_painted_box(const embi_layout_engine *e, const embi_element *el, embi_rectf *out);
bool embi_layout_painted_box_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out);
uint32_t embi_layout_painted_rects_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out, uint32_t cap);
/* CSSOM View offsetParent, offsetLeft/Top/Width/Height. *parent receives the offsetParent
 * (nullptr when there is none). Widths and heights are the rounded border box size. The offsets
 * are relative to the padding edge of the offsetParent (see the spec for body). */
bool embi_layout_offset(const embi_layout_engine *e, const embi_element *el,
                        const embi_element **parent, float *x, float *y, float *w, float *h);
/* clientWidth/clientHeight: padding box size without scrollbars (rounded). */
bool embi_layout_client_size(const embi_layout_engine *e, const embi_element *el, float *w,
                             float *h);
/* The used border widths, paddings and margins of an element (left, top, right, bottom). Zero when it has
 * no box; returns false then. */
bool embi_layout_box_edges(const embi_layout_engine *e, const embi_element *el, float bdr[4],
                           float pad[4], float mg[4]);
/* scrollWidth/scrollHeight: the scrollable overflow size, at least the client size. */
bool embi_layout_scroll_size(const embi_layout_engine *e, const embi_element *el, float *w,
                             float *h);
/* The topmost element whose border box contains (x, y) in viewport coordinates (CSS px, the root's scroll offset applied as
 * paint applies it), in the paint order of src/paint (stacking contexts, z-index, positioned and floated boxes), skipping
 * pointer-events: none and visibility hidden boxes as targets (their children still count) and the parts outside an
 * ancestor's clip. A position: fixed box of the viewport is not moved by the root's offset, as paint keeps it still.
 * Implemented in hit.c (W5d2). */
embi_element *embi_layout_hit_test(const embi_layout_engine *e, const embi_document *doc, float x,
                                   float y);
/* Every element hit at the same point: the walk of embi_layout_hit_test without its stop at the first hit. Each element
 * whose box is hit (its target test, clip and transform as there) appears once, topmost first in paint order, so the first
 * entry is the one embi_layout_hit_test returns. The root element is not added here. *out receives an embi_alloc'd array
 * of *n elements (free with embi_free(out, *n * sizeof *out)); *out is nullptr when nothing is hit. Implemented in hit.c. */
void embi_layout_hit_test_all(const embi_layout_engine *e, const embi_document *doc, float x, float y,
                              embi_element ***out, uint32_t *n);
/* As embi_layout_hit_test, and local gets the point in the hit element's border box, in the box's own space (every
 * transform and scroll offset on the way undone; NAN when unknown). An iframe maps pointer events into a frame with it. */
embi_element *embi_layout_hit_test_local(const embi_layout_engine *e, const embi_document *doc, float x, float y,
                                         float local[2]);

/* R4c: the SVG hit test of an svg element's box. The hook gets the engine, the svg element, its content box (x, y, w, h
 * relative to the border box) and the point relative to the content box's origin, and returns the element of the shape
 * hit, or nullptr when the point hits no shape (the svg element is the target then). Process-wide; nullptr (the default)
 * makes the svg element itself the target. */
typedef const embi_element *(*embi_layout_svg_hit_fn)(const embi_layout_engine *e, const embi_element *svg, const float box[4],
                                                       float x, float y);
void embi_layout_set_svg_hit(embi_layout_svg_hit_fn fn);

/* R4c: foreignObject. The foreignObject elements of an svg root get block boxes of their own (children of the root's
 * replaced box, flagged EMBI_BF2_SVGFO): each is laid out as an independent formatting context, a stacking context and the
 * containing block of its positioned descendants, at its width and height. The SVG painter paints each where the walk
 * of the svg meets it (in its user space, clipped to its viewport), and the SVG hit test asks embi_layout_hit_svg_fo.
 * The hook gives the box of a foreignObject inside the svg root svg whose content box is cw x ch CSS px: out[0], out[1] are
 * where its user-space point (x, y) lands in that content box (the fragment is placed there for the geometry queries) and
 * out[2], out[3] its width and height in its user units (the size its content is laid out at). False when it renders
 * nothing. Process-wide; without a hook the plain numbers of its x, y, width and height attributes are used. */
typedef bool (*embi_layout_svg_fo_fn)(embi_style_engine *se, const embi_element *svg, const embi_element *fo, float cw,
                                      float ch, float out[4]);
void embi_layout_set_svg_fo(embi_layout_svg_fo_fn fn);
/* The element under the point (x, y) of a foreignObject's box (its own space: its border box starts at 0, 0), in the paint
 * order of the stacking context it is: its content first, then the foreignObject itself. nullptr when the point hits none
 * of it or the element has no foreignObject box. */
const embi_element *embi_layout_hit_svg_fo(const embi_layout_engine *e, const embi_element *fo, float x, float y);

/* Counts of the last layout of a document, for the bench and the tests: the boxes of the box tree
 * (elements with a box and their descendants), the fragments, and the bytes in use in the box and
 * fragment arenas. The shaped text runs are not included (they are shared text caches). Zeros when
 * the document has no layout. */
void embi_layout_stats(const embi_layout_engine *e, const embi_document *doc, uint32_t *boxes,
                       uint32_t *frags, size_t *arena_bytes);

/* The bytes of derived data the document's layout keeps between layouts besides the trees counted by
 * embi_layout_stats (W3p): the inline analysis cache (break candidates and advance tables of the simple
 * paragraphs, in an arena of its own), the style key and language tables, the scratch pool of the inline
 * formatting contexts, and the index of the shaped-run cache. They are functions of the text and styles
 * alone and are rebuilt with the box tree, so the bench counts them separately; the shaped runs
 * themselves are owned by the shared text system and are not included. Zero for an unknown document. */
size_t embi_layout_cache_bytes(const embi_layout_engine *e, const embi_document *doc);

/* How many times the box tree of the document was built by the layouts so far (W3p: a layout of an
 * unchanged document keeps the tree; a change of the styles, the viewport, the scrollbar thickness or an
 * image size builds it again). For the tests and the bench. Zero for an unknown document. */
uint32_t embi_layout_tree_builds(const embi_layout_engine *e, const embi_document *doc);

/* How many times embi_layout_document was called on the engine (a layout that changed nothing counts too): a cache of
 * fragment pointers and positions is valid only while this is unchanged (and the scroll offsets it read are). */
uint64_t embi_layout_calls(const embi_layout_engine *e);

/* Fragment allocation of the last layout, for the tests and the bench: the fragments the layout asked
 * for (including those of passes that were thrown away), and how many of them were served from the
 * recycled ones instead of new arena space. Zeros when the document has no layout. */
void embi_layout_alloc_stats(const embi_layout_engine *e, const embi_document *doc, uint32_t *frag_allocs,
                             uint32_t *frag_reused);

/* Bench-only instrumentation (the ticks are read with SDL_GetPerformanceCounter on the layout paths, the
 * counts are plain increments; nothing here affects layout results). Where the time of the layouts of a document went, summed over every layout since its state was created, in
 * SDL performance counter ticks (W3q; the first layout of a document is its cold one, so the bench reads
 * this right after it). shape_*: the text module shaping the missing runs of the shaped-run cache, split
 * into the runs of text boxes and the runs of whole paragraphs (the analysis shapes each paragraph once for
 * its breaks and bidi levels). build_ticks: the box tree builds. analysis_*: the paragraph analyses that
 * were computed (cache misses), without the shaping inside them. */
typedef struct embi_layout_cold_stats {
    uint32_t box_runs, para_runs, builds, analyses;
    uint64_t box_ticks, para_ticks, build_ticks, analysis_ticks;
} embi_layout_cold_stats;
void embi_layout_cold_stats_get(const embi_layout_engine *e, const embi_document *doc,
                                embi_layout_cold_stats *out);

/* Sets the scroll offset of fragment f of the document's current layout (W3h: the offset lives in the
 * fragment's side record, embi_fext). W4/W5 call it when the user scrolls; the offset does not change
 * the layout. */
void embi_layout_frag_set_scroll(embi_layout_engine *e, const embi_document *doc, embi_frag *f,
                                 float sx, float sy);

/* Writes an indented text dump of the fragment tree (box kind, element tag, id, class, rect and
 * text pieces). Used for debugging and by the layout fixtures. */
void embi_layout_dump(const embi_layout_engine *e, const embi_document *doc, embi_sb *sb);

/* ------------------------------------------------------------------ incremental update (W6b, incr.c) */

/* embi_layout_document patches the kept tree from the style engine's change set instead of building it again whenever it
 * can, and leaves the fragments alone when nothing layout reads changed. What the last call
 * did, for tests and benches: */
enum {
    EMBI_LU_NONE = 0, /* nothing changed: the fragments are the ones of the last layout */
    EMBI_LU_PAINT,    /* only paint properties changed: the fragments are kept, the boxes hold the new styles */
    EMBI_LU_SURGERY,  /* the box subtrees around the changes were built again and the layout ran over the kept tree */
    EMBI_LU_FULL,     /* the tree was built again */
    EMBI_LU_ABORT,    /* the layout did not run (out of memory, no thread for a deep page) */
};
typedef struct embi_layout_incr_stats {
    uint8_t last_kind;        /* EMBI_LU_* of the last embi_layout_document */
    uint32_t boxes_laid_out;  /* fragments allocated by the last layout run (0 when none ran) */
    uint32_t boxes_rebuilt;   /* boxes built by the last surgery */
    uint32_t boxes_reused;    /* fragments the last partial relayout kept instead of laying their boxes out again */
    bool partial;             /* the last SURGERY patched the fragment tree (boxes_laid_out counts the boxes laid out again) */
    uint64_t n_none, n_paint, n_surgery, n_full, n_partial;
    uint32_t tree_builds;     /* full builds of the box tree */
} embi_layout_incr_stats;
void embi_layout_incr_stats_get(const embi_layout_engine *e, const embi_document *doc, embi_layout_incr_stats *out);
/* Running totals of the engine's embi_layout_document calls, for benches (tools/bench/bench_editor.c): how many there were,
 * the time they took (SDL performance counter ticks), the parts of it in the style resolve and in the update plan (the box
 * surgery), and the calls and time by what the update did. kinds[] is indexed by EMBI_LU_*, with EMBI_LAYOUT_PROF_PARTIAL
 * for a SURGERY that patched the fragment tree (the others count under EMBI_LU_SURGERY). boxes sums boxes_laid_out. */
enum { EMBI_LAYOUT_PROF_PARTIAL = 5, EMBI_LAYOUT_PROF_KINDS = 6 };
typedef struct embi_layout_prof {
    uint64_t calls, ticks, style_ticks, plan_ticks;
    uint64_t kinds[EMBI_LAYOUT_PROF_KINDS];
    uint64_t kind_ticks[EMBI_LAYOUT_PROF_KINDS];
    uint64_t boxes;
    uint64_t reused; /* boxes whose layout the layout cache gave back (lcache.c) */
    uint64_t reused_xrun; /* ... of which an earlier layout run made (the cache is kept while the box tree is) */
} embi_layout_prof;
void embi_layout_prof_get(const embi_layout_engine *e, embi_layout_prof *out);
/* The number of fragments the last layout run allocated: the boxes it laid out (0 when the last update ran no layout). */
uint32_t embi_layout_boxes_laid_out(const embi_layout_engine *e, const embi_document *doc);
/* The next layout builds the tree again (a change the style engine's change set does not describe). */
void embi_layout_invalidate_all(embi_layout_engine *e, const embi_document *doc);
/* As embi_layout_invalidate_all, and the next layout also shapes every text again: the shaped runs are cached by style, not
 * by the faces the registry held when they were made, so a web font that registered after the first layout needs this. */
void embi_layout_flush_text(embi_layout_engine *e, const embi_document *doc);
/* The embedder promises that image sizes only change when the DOM or a style does. Without it a document with an image is
 * built again by every layout (the sizes are asked for on every build). */
void embi_layout_set_images_stable(embi_layout_engine *e, bool stable);
/* For the paint module. serial counts the layouts that rebuilt the fragments: a display list built at another serial is
 * stale. When the result is true the fragments are those of serial and only the *els (n elements, valid until the next
 * layout) changed their paint since embi_layout_paint_clean; when false everything must be painted again. */
bool embi_layout_paint_dirty(embi_layout_engine *e, const embi_document *doc, uint64_t *serial, embi_element *const **els,
                             uint32_t *n);
/* The paint module has painted: forget the dirty elements. */
void embi_layout_paint_clean(embi_layout_engine *e, const embi_document *doc);
/* The epoch of the document's shaped runs: it changes whenever a layout freed runs that a display list may name, and a layout
 * state made again for the document has a new one. A list built at another epoch must not be read (compared, reused or
 * drawn). 0 when the document has no layout state; never 0 otherwise, and never repeated. */
uint64_t embi_layout_run_epoch(embi_layout_engine *e, const embi_document *doc);
/* W4g4c, content_img.c: images of the content property (CSS Content 3). embi_lay_image_resolve picks the option of an
 * image-set() (1x, else the lowest resolution above it, else the highest below; nested sets too), and returns other
 * images unchanged (nullptr when no option applies); *dppx (when not null) is the chosen resolution.
 * embi_lay_content_image gives the index-th <image> item of a content value (url() as an EMBI_IMG_URL image), resolved,
 * when it is a url or a gradient; embi_lay_content_replaces is true when the value is exactly one <image> (with optional
 * alt text), which replaces the element. */
struct embi_image;
const struct embi_image *embi_lay_image_resolve(const struct embi_image *im, float *dppx);
bool embi_lay_content_image(const embi_value *content, uint32_t index, struct embi_image *out);
bool embi_lay_content_replaces(const embi_value *content, struct embi_image *out);
/* The source of an img from its src and srcset attributes (pixel density descriptors only, a device pixel ratio of
 * 1): a slice of src or srcset, and its density in *density (the natural size is divided by it). */
embi_str embi_lay_img_source(embi_str src, embi_str srcset, float *density);
