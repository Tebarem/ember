#pragma once
/*
 * Internal declarations of the paint module (W4c). paint.c walks the fragment tree and calls the painters
 * of background.c, border.c and text_paint.c through the state below. Nothing here is used outside the
 * module: the public surface is paint.h.
 */
#include "paint/paint.h"

#include "css/props.h"
#include "layout/box.h"
#include "layout/fragment.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "layout/xform.h"
#include "style/style.h"

#include <stdbool.h>
#include <stdint.h>

/* An entry of a stacking context's pending list: a child stacking context with its parent state. */
typedef struct embi_pent {
    const embi_frag *f;
    float ox, oy;      /* the origin of its border box (sticky offset included) */
    uint32_t xform;    /* the transform of the parent state */
    uint32_t clip;     /* the clip of the parent state */
    int32_t z;
    uint32_t seq;      /* its position in the pending list when it was added (the order of equal z) */
    const uint64_t *kp; /* its document-order key (see paint.c), valid only while the entries are sorted; nullptr when
                         * the entries cannot be compared by key */
    uint32_t klen;     /* the key length in words */
    uint32_t koff;     /* its offset in the key stack while the keys are built */
} embi_pent;

/* A child of a flex or grid container with its order key, for the order-modified paint order (sorted by order, then
 * tree order). */
typedef struct embi_ordk {
    int64_t order;
    uint32_t seq;
    const embi_frag *f;
} embi_ordk;

/* W6b (paint/incr.c): what paint_ctx was called with for a stacking context, to build that context again on its own. The
 * records are parallel to the context table of the list; sub_end is the index after the context's last nested context. */
typedef struct embi_pctx_rec {
    const embi_frag *f;
    float kx, ky;
    uint32_t xf_parent, clip_parent, parent_ctx, sub_end;
} embi_pctx_rec;
typedef EMBI_VEC(embi_pctx_rec) embi_pctx_vec;
/* A box that has a perspective, while its content is painted: the matrix it gives its children (R3c). */
typedef struct embi_pers {
    const embi_element *el;   /* the element of the box with the perspective */
    embi_m4 m;
} embi_pers;

struct embi_p3ctx;

/* The state of one build. */
typedef struct embi_pst {
    embi_dl *d;
    const embi_paint_cfg *cfg;
    float vw, vh;                 /* the viewport (the root fragment's size) */
    float rem;                    /* the root element's font size (rem units in transforms) */
    float lh;                     /* the line-height in px of the box whose image layers are being painted (the lh unit) */
    bool canvas_bg;               /* painting the root's background onto the canvas: the clip area is the viewport */
    const embi_element *bg_skip;  /* the element whose background moved to the canvas (it paints none), or nullptr */
    const embi_frag *bg_f;        /* the box whose background (or border image) is being painted: its relative units resolve here */
    const embi_style *bg_s;
    const float *bg_raw;          /* the border box of bg_f before pixel snapping (self_paint), or nullptr: background
                                   * positions are computed from it and then snapped, as Blink does */
    uint32_t cur_ctx;             /* the stacking context being painted (its index in the context table) */
    EMBI_VEC(embi_pent) pend;     /* pending stacking children; each context owns a segment */
    uint32_t clamp_clip;          /* the clip of the floats of the line-clamp container being walked (paint.c), 0 when none */
    EMBI_VEC(const embi_frag *) ord; /* scratch: the children of flex and grid containers in paint order (a stack) */
    EMBI_VEC(embi_ordk) oks;      /* scratch: their order keys while they are sorted (a stack) */
    EMBI_VEC(embi_pent) units;    /* scratch: the step 6 units of the open scopes (a stack) */
    EMBI_VEC(uint64_t) keys;      /* scratch: the document-order keys of the entries being sorted (a stack) */
    embi_hashmap sibs;            /* sibling indices of nodes in wide parents (node -> index) */
    embi_hashmap sibs_done;       /* the wide parents whose children are all in sibs */
    embi_hashmap scr;             /* scroll60: scroll container fragment -> (outer transform << 32 | content transform) made for it */
    embi_style_engine *se;        /* the style engine: the ::first-line styles of blocks */
    const embi_frag *root;        /* the root fragment (the initial containing block) */
    embi_text *text;              /* W5e: the text system of the layout (control text is shaped at paint time) */
    embi_layout_control_fn ctl_fn; /* W5e: the form control state hook of the engine */
    void *ctl_user;
    bool popup;                   /* W5e: an open select's popup list is drawn last (control.c) */
    embi_paint_ctl popup_ctl;
    float popup_bb[4];
    const embi_style *popup_s;
    const embi_frag *html;        /* the document element's fragment when it is a child of root, else nullptr: root and
                                   * html form one stacking context (CSS 2.1 9.9.1: the root element's context) */
    embi_pctx_vec *rec;           /* W6b: records the stacking contexts (nullptr: not recorded) */
    uint32_t rec_xf_parent, rec_clip_parent; /* W6b: the parent state paint_ctx gave the context being recorded */
    EMBI_VEC(embi_pers) pers;     /* the boxes with a perspective whose content is being painted (a stack) */
    struct embi_p3ctx *c3;        /* the 3D rendering context being painted (plane3d.c), or nullptr */
    const embi_frag *bg_src;      /* R3b: the canvas background (root or body) is painted inside the root context's group when it has one */
    const embi_style *bg_style;
    float bg_rbb[4];
    uint32_t bg_vclip;
    bool bg_pending;              /* the background is still to be painted by the root context */
    bool bg_deferred;             /* it was deferred: an incremental build cannot rebuild the root context in place */
    uint32_t nblend;              /* R3b: the blend groups finished so far that no isolated group has absorbed yet */
    uint32_t nbackdrop;           /* R3b: the backdrop-filter items emitted so far that no backdrop root has absorbed yet */
    bool mask_svg;                /* R4e: the mask layer just painted is an SVG mask (url(#id)): its items hold the mask value as alpha */
} embi_pst;

/* W6b: the setup of embi_paint_document, split so that paint/incr.c can build one stacking context again. begin fills p
 * (zeroed by the caller) for the document in d; false when there is no root fragment. With emit_bg the canvas background
 * is painted and *vclip is the viewport clip; without it nothing is added to d and *vclip is 0. end frees the scratch
 * state. reenter paints the context f exactly as the full build did. */
bool embi_paint_begin(embi_pst *p, embi_dl *d, const embi_layout_engine *e, const embi_document *doc,
                      const embi_paint_cfg *cfg, bool emit_bg, uint32_t *vclip);
void embi_paint_end(embi_pst *p);
void embi_paint_reenter(embi_pst *p, const embi_frag *f, float kx, float ky, uint32_t xf_parent, uint32_t clip_parent,
                        uint32_t parent_ctx);

/* The painting of a stacking context f (its border box at kx, ky) from its own items down, under the transform xf and the
 * clip clip_parent. alpha below 1 puts it in an opacity layer. paint_ctx and plane3d.c share it. */
void embi_pb_paint_body(embi_pst *p, const embi_frag *f, float kx, float ky, const float bb[4], uint32_t xf,
                        uint32_t clip_parent, uint32_t parent_ctx, float alpha);

/* A value read from a computed style, in px where a length is relative. ref is the 100% base, fs the font
 * size for em. ok is false for keywords and values of another type. */
float embi_pb_len(const embi_pst *p, const embi_value *v, float ref, float fs, bool *ok);
/* The color of a computed value (COLOR, or LIGHT_DARK as its light member), as RGBA8. currentcolor
 * resolves to the element's color property. False for other kinds. */
bool embi_pb_color(const embi_style *s, const embi_value *v, uint32_t *rgba);
/* The same color before it is rounded to 8 bits: currentcolor, light-dark() and a deferred color resolved. */
bool embi_pb_color_full(const embi_style *s, const embi_value *v, embi_color *out);
/* The keyword of a computed value, or 0. */
uint16_t embi_pb_kw(const embi_style *s, embi_prop_id prop);
/* The font size of a style in px (16 when it is not a px length). */
float embi_pb_fontsize(const embi_style *s);
/* The computed line-height of s in px (normal is 1.2 times the font size). */
float embi_pb_lineheight(const embi_style *s);
/* The border radii of a box of size w x h (border box) as circular px, scaled as CSS Backgrounds 3 section
 * 5.5 when two adjacent radii overflow a side. Order: top-left, top-right, bottom-right, bottom-left. */
void embi_pb_radii(const embi_pst *p, const embi_style *s, float w, float h, float fs, float r[4]);
/* As embi_pb_radii, with the corners of the bits of zero (1 << corner) square before the radii are scaled. */
void embi_pb_radii_mask(const embi_pst *p, const embi_style *s, float w, float h, float fs, unsigned zero, float r[4]);
/* The radii of fragment f: as embi_pb_radii, with the corners at the broken ends of an inline box piece square (a piece
 * that does not start or end its box: box-decoration-break: slice rounds only the box's own ends). */
void embi_pb_frag_radii(const embi_pst *p, const embi_frag *f, const embi_style *s, float w, float h, float fs,
                        float r[4]);
/* The radii of the inner edge of a border (the padding edge), from the outer radii and the widths
 * (left, top, right, bottom). */
void embi_pb_inner_radii(const float outer[4], const float bdr[4], float out[4]);
/* The paint rects of a box: the border box bb (x, y, w, h), the padding box and the content box. */
void embi_pb_boxes(const embi_frag *f, const float bb[4], float pb[4], float cb[4]);

/* Clip and transform entries. The clip's parent is the one given. Return the index. */
uint32_t embi_pb_clip(embi_pst *p, const float rect[4], const float radii[4], uint32_t xform, uint32_t parent);
uint32_t embi_pb_xform(embi_pst *p, const float m[6]);
/* scroll60: the scroll offset of the container k as a translation. Returns the transform k's content is in: xf moved by minus
 * the offset (rounded to device px), recorded in the list's scroll table so that a scroll can move it; xf itself when k has no
 * offset to apply (nothing scrolls). The transforms made while the content is painted carry the container (dl->cur_scroll). */
uint32_t embi_pb_scroll_enter(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf);
/* The scroll container tag (index + 1) of transform xf, 0 when it is in none. */
uint32_t embi_pb_xform_scroll(const embi_dl *d, uint32_t xf);
/* The offsets of the list's containers are not the only thing that places the content: the list must be built again to scroll. */
void embi_pb_scroll_rigid(embi_pst *p);
/* The translation that the scroll containers around transform xf add to it (what a viewport-fixed rect must be moved back by). */
void embi_pb_unscroll_delta(embi_pst *p, uint32_t xf, float *dx, float *dy);
/* Item builders. Each returns false when the item is not emitted (the empty rect, a transparent color). */
embi_ditem *embi_pb_item(embi_pst *p, uint8_t kind, uint32_t xform, uint32_t clip);
bool embi_pb_fill(embi_pst *p, const float rect[4], const float radii[4], uint32_t rgba, uint32_t xform,
                  uint32_t clip);
void embi_pb_gradient(embi_pst *p, uint8_t kind, const float rect[4], const float radii[4], const float geo[6],
                      const embi_dstop *stops, uint32_t n, bool repeat, uint32_t xform, uint32_t clip);
/* A gradient item whose stops are already in dl->stops at [first, first + n): several items may share one stop range. */
void embi_pb_gradient_at(embi_pst *p, uint8_t kind, const float rect[4], const float geo[6], uint32_t first,
                         uint32_t n, bool repeat, uint32_t xform, uint32_t clip);
void embi_pb_image(embi_pst *p, embi_str url, const float rect[4], const float uv[4], uint32_t tint,
                   bool nearest, uint32_t xform, uint32_t clip);
uint32_t embi_pb_url(embi_pst *p, embi_str url);
/* A tiled image (EMBI_DF_TILE, see EMBI_DI_IMAGE): the area span, the first tile origin, the tile size and the step; sub (or
 * nullptr for the whole image) is the uv rect (x0, y0, x1, y1) of the part of the image a tile shows. */
void embi_pb_image_tile(embi_pst *p, embi_str url, const float span[4], const float first[2], const float size[2],
                        const float step[2], const float sub[4], uint32_t tint, bool nearest, uint32_t xform, uint32_t clip);
/* A triangle (a disclosure marker) in its box, pointing dir (EMBI_TRI_*). */
void embi_pb_triangle(embi_pst *p, const float rect[4], uint8_t dir, uint32_t rgba, uint32_t xform, uint32_t clip);

/* Painters. The box's own boxes (bb: border box, in page px) with the transform and clip of its paint. */
void embi_pb_background(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                        uint32_t clip);
void embi_pb_border(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                    uint32_t clip);
/* The rules between the columns of a multi-column container (column-rule-*), over its border. */
void embi_pb_colrules(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                      uint32_t clip);
void embi_pb_outline(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                     uint32_t clip);
/* A replaced element (img) with its content box. */
void embi_pb_replaced(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4],
                      uint32_t xform, uint32_t clip);
/* Form controls (W5e, control.c). _pre paints the face under the background and border, embi_pb_control the content and chrome
 * over the border, and embi_pb_control_popup the open select's list on top of the page at the end of the build. */
void embi_pb_control_pre(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4], uint32_t xform,
                         uint32_t clip);
void embi_pb_control(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4], uint32_t xform,
                     uint32_t clip);
void embi_pb_control_popup(embi_pst *p);
/* The collapsed borders of a table fragment (border-collapse: collapse) from the W3 per-edge winners, drawn
 * at the table's border-box origin (ox, oy). Nothing for a table with separate borders. */
void embi_pb_collapsed(embi_pst *p, const embi_frag *t, float ox, float oy, uint32_t xf, uint32_t clip);
/* border-image (bimage.c): true when it painted the border of box bb (page px, snapped) instead of the border styles,
 * which it does not when the source is none, an image that has not loaded, or a table in the collapsing model. */
bool embi_pb_border_image(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                          uint32_t clip);
/* The url() or gradient an image() or image-set() paints, or nullptr (background.c). */
const embi_image *embi_pb_simple_image(const embi_pst *p, const embi_image *im);
/* A gradient prepared for border-image pieces: its kind and geometry over the box img (page px), and its stops at
 * [*first, *first + *n) (background.c). False when it has no color stop. */
bool embi_pb_grad_prepare(embi_pst *p, const embi_gradient *g, const embi_style *s, const float img[4], float fs,
                          uint8_t *kind, float geo[6], uint32_t *first, uint32_t *n);
/* A text fragment: its rect (page px) and the baseline origin. */
/* baseline is the run's own (vertical-align shifted) baseline, line_baseline the line's: a decoration that an ancestor
 * sets is drawn on the line's, one that the text's own element sets on the run's. */
void embi_pb_text(embi_pst *p, const embi_frag *f, const embi_box *b, const float rect[4], float baseline, float line_baseline,
                  uint32_t xform, uint32_t clip);
/* A list marker drawn as a shape (list-style-type disc, circle, square, disclosure-open, disclosure-closed) instead
 * of its text. True when the marker is a shape: the caller then paints nothing of its text fragment. The shape is
 * emitted by the first fragment of the marker only. */
bool embi_pb_marker_shape(embi_pst *p, const embi_frag *f, const embi_box *b, const float rect[4], float baseline,
                          uint32_t xform, uint32_t clip);
/* The transform of a box (its own transforms only): returns the composed transform, or the parent one when
 * the box has none. A 3D transform is composed as 4x4 and projected to 2D (see paint.c). */
uint32_t embi_pb_box_xform(embi_pst *p, const embi_style *s, const float bb[4], uint32_t parent_xform);
/* True when the value of visibility on s hides the box's own painting. */
bool embi_pb_hidden(const embi_style *s);
/* True when the element is a HTML element with the given (lowercase ASCII) name. */
bool embi_pb_named(const embi_element *el, const char *name);
/* R4c: the environment of the SVG painter for this build. */
void embi_pb_svg_env(embi_pst *p, embi_svg_env *env);
const embi_svg_image *embi_pb_svg_lookup(void *user, embi_str url);
/* R4c: paints the stacking context of a foreignObject's box (its content) with its border box at the origin of the transform
 * xf, under clip (the SVG walk gives its user space, viewport clip and group effects). Nothing when it has no box. */
void embi_pb_paint_foreign(embi_pst *p, const embi_element *fo, uint32_t xf, uint32_t clip);
/* The alpha of opacity on s, clamped to 0..1 (1 when none). */
float embi_pb_opacity(const embi_style *s);

/* ---- R3a effects (effects.c) ---- */
/* The radius of a corner of a shadow shape after a spread (CSS Backgrounds 3 section 7.1.1). */
float embi_pb_spread_radius(float r, float spread);
/* The outer (inset == false) or inset box shadows of a box with border box bb, as SHADOW items. */
void embi_pb_shadows(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], bool inset, uint32_t xform,
                     uint32_t clip);
/* Appends the steps of the filter list `prop` (filter or backdrop-filter) of s to dl->fx. Returns the number of steps
 * and sets *first to the index of the first. */
uint32_t embi_pb_filter_ops(embi_pst *p, const embi_style *s, embi_prop_id prop, uint32_t *first);
/* Opens a group (LAYER_BEGIN) with opacity and the fx steps [first, first + n) (filtered when n > 0) and closes it
 * (LAYER_END). embi_pb_fx_end drops a group that holds no item. Returns the index of the begin item. */
uint32_t embi_pb_fx_begin(embi_pst *p, uint32_t first, uint32_t n, float opacity, uint32_t xform, uint32_t clip);
void embi_pb_fx_end(embi_pst *p, uint32_t begin);
/* The backdrop-filter item of a box with border box bb, when it has one. */
void embi_pb_backdrop(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                      uint32_t clip);

/* ---- R3b: blend modes, isolation, masks, clip-path (composite.c, background.c) ---- */
/* The EMBI_BM_* of a blend mode keyword (normal for any other). */
uint32_t embi_pb_blend_kw(uint16_t kw);
/* Opens a COMPOSITE group (dl.h) with the blend mode and, with isolate, the isolation flag; xform and clip are what the
 * composite applies. Returns the index of its LAYER_BEGIN. embi_pb_group_end closes it, drops it when it holds no item, and
 * counts a blend group in p->nblend. */
uint32_t embi_pb_group_begin(embi_pst *p, uint32_t blend, bool isolate, uint32_t xform, uint32_t clip);
void embi_pb_group_end(embi_pst *p, uint32_t begin);
/* A mask layer group (EMBI_LS_MASK) with its mode and composite operator; an empty one is kept. */
uint32_t embi_pb_mask_begin(embi_pst *p, uint32_t mode, uint32_t comp, uint32_t xform, uint32_t clip);
void embi_pb_mask_end(embi_pst *p, uint32_t begin);
/* The area of a background, mask or clip-path box keyword (EMBI_KW_border_box, padding_box, content_box, margin_box, fill_box,
 * stroke_box, view_box) of a box with border box bb, and its corner radii (background.c). */
void embi_pb_area(const embi_frag *f, const float bb[4], uint16_t kw, float out[4]);
void embi_pb_area_radii(const embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4], uint16_t kw,
                        float out[4]);
/* The mask layers of the box (mask-image, mask-mode, ...) as mask groups; returns their number (background.c). */
uint32_t embi_pb_mask_layers(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xform,
                             uint32_t clip);

/* What the effect properties of a style ask of its stacking context (R3b). */
typedef struct embi_pcomp {
    uint32_t blend;        /* mix-blend-mode, EMBI_BM_* */
    bool isolate;          /* isolation: isolate */
    bool clip_path;        /* a clip-path that is a shape or a box */
    bool clip_ref;         /* R4e: a clip-path url(#id) */
    bool mask;             /* a mask-image with an image layer */
    bool backdrop_root;    /* backdrop-filter or a will-change that names a property that makes a backdrop root */
} embi_pcomp;
void embi_pb_comp_scan(const embi_style *s, embi_pcomp *c);
/* Whether the context needs a COMPOSITE group of its own (blend, clip-path, mask). */
static inline bool embi_pb_comp_group(const embi_pcomp *c)
{
    return c->blend != 0 || c->clip_path || c->clip_ref || c->mask;
}
/* Opens the COMPOSITE group of a stacking context with border box bb, whose style is s (effect properties), and emits its mask
 * layers. The filter steps [fx_first, fx_first + fx_n) and the opacity go in the same group. Returns the begin index. */
uint32_t embi_pb_comp_open(embi_pst *p, const embi_frag *f, const embi_style *s, const embi_pcomp *c, const float bb[4],
                           uint32_t fx_first, uint32_t fx_n, float opacity, uint32_t xform, uint32_t clip);
/* Wraps the items from `at` on (a stacking context's body, after its backdrop-filter items) in an isolated group, shifting the
 * indices the list holds (the markers' links and the contexts from ctx_index + 1 on). */
void embi_pb_isolate_from(embi_pst *p, uint32_t at, uint32_t ctx_index);
