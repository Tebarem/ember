#pragma once
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * SVG rendering (R4c): the render tree of an <svg> element, built from the DOM and the computed styles, and
 * what is done with it: the display list items that paint it, the hit test, the geometry queries of the SVG DOM, and SVG
 * documents used as images (<img src=x.svg>, CSS url(x.svg)).
 *
 * Nothing persists between calls. The render tree is built into an arena for one paint, hit test or query and freed at its
 * end, so a page with SVG pays for the list items it paints (a 24x24 path icon is one EMBI_DI_PATH item and the path's
 * quadratics) and a page without SVG pays nothing.
 *
 * Painting. embi_svg_paint appends to a display list: EMBI_DI_PATH items for fills and strokes (the strokes are expanded by
 * embi_path_stroke, R4a), EMBI_DI_TEXT items for text, EMBI_DI_IMAGE items for <image>, clips for the viewport of nested
 * svg, symbol, image, marker, pattern and foreignObject, groups (EMBI_DI_LAYER_BEGIN) for opacity, and EMBI_LS_ISO and
 * EMBI_LS_SVGMASK groups for clip-path and mask. Paint servers become gradient paints of the path items, or repeated content
 * (pattern).
 *
 * Styles. The presentation attributes reach the cascade as author level 0 declarations (style/hints.c). The instance of a
 * `use` element is styled with embi_style_compute_under, so it inherits from the use element.
 *
 * Out of scope (labelled not-yet): SMIL animation, SVG fonts, filter elements, textPath (text on a path is laid out as plain
 * text), the focal point of a radial gradient, context-fill and context-stroke outside markers. Hit testing ignores clip-path and
 * mask (a point in a clipped-away part of a shape still hits it).
 *
 * foreignObject. Layout gives each foreignObject of an inline svg a block box (a formatting context, a stacking context and a
 * containing block) of its width and height, a child of the svg root's replaced box (embi_layout_set_svg_fo). The walk paints
 * that fragment tree through env->paint_foreign where it meets the element, under the element's user space and clipped to its
 * viewport, and hit tests it through embi_layout_hit_svg_fo. Not drawn: inside use instances, markers, patterns, masks and
 * clip paths (one fragment tree cannot be painted at several places), and in SVG images (they have no layout). The box is laid
 * out in user units: a scaled or rotated foreignObject scales or rotates its laid-out content (text is not re-laid out at the
 * device scale).
 *
 * R3 effects on SVG content (R4e). mix-blend-mode, isolation and a CSS clip-path basic shape or box on an element are one
 * EMBI_LS_COMPOSITE group around it (a clip-path shape is resolved against the element's bounding box into dl->cpaths; an
 * isolated group exists only when a descendant blends); a CSS mask-image of images or gradients is the layers of the html mask
 * machinery (env->mask_layers); url(#id) masks and clip paths are the EMBI_LS_ISO and EMBI_LS_SVGMASK groups of R4c. A mask
 * or clip-path url(#id) on an html box (including the outer svg element) is emitted by embi_svg_ref_content as a mask layer of
 * the box's composite group. text-shadow on SVG text is a blurred, filtered group (or a plain offset copy) of the text's fill
 * and stroke in the shadow color, and SVG text is stroked through the glyph outlines (embi_tshim_outline). A foreignObject takes
 * its blend mode, basic-shape clip-path and image masks from its box (paint.c) and its url(#id) references from the walk.
 *
 * Patterns. The walk draws the content of one tile; the other tiles are copies of its items moved by the tile offset. A pattern
 * over more than 16384 tiles of the shape's box, or whose copies would exceed 262144 items, paints nothing.
 */
#include "base/base.h"
#include "dom/dom.h"
#include "layout/text_shim.h"
#include "paint/dl.h"
#include "style/style.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_svg_image embi_svg_image;

/* What the renderer needs from its host. Every callback may be nullptr. */
typedef struct embi_svg_env {
    embi_style_engine *se;   /* styles of use instances; nullptr: a use element draws like its target in place */
    embi_text *text;         /* the text system, for <text>; nullptr: text is not drawn */
    float dev_scale;         /* device px per CSS px (the tolerance of curves and strokes); 0 is 1 */
    float vw, vh;            /* the viewport in CSS px (vw and vh units); 0 is 1 */
    void *user;
    /* The natural size of an image URL as written in the document (raster images). */
    bool (*image_size)(void *user, embi_str url, float *w, float *h, bool *has_ratio);
    /* The SVG image behind a URL as written in the document, or nullptr when it is not an SVG image (or is not loaded). */
    const embi_svg_image *(*svg_image)(void *user, embi_str url);
    /* foreignObject (the svg's foreignObject boxes are laid out by layout, see embi_layout_set_svg_fo). The painter paints
     * the content of one with its border box at the origin of the display list transform xf, under clip; host is its first
     * argument. nullptr: foreignObject content is not painted. */
    void *host;
    void (*paint_foreign)(void *host, const embi_element *fo, uint32_t xf, uint32_t clip);
    /* R4e: the layers of a CSS mask-image (images and gradients; url(#id) is the walk's own) of an SVG element el with computed
     * style st. The host appends one EMBI_LS_MASK group per layer to the display list (the walk has just opened the COMPOSITE
     * group that owns them) for the reference box bb (x, y, w, h: the stroke box of the element, in its user space, which the
     * transform index xform maps to the page) and returns how many it appended. nullptr: such masks are not applied. */
    uint32_t (*mask_layers)(void *host, const embi_element *el, const embi_style *st, const float bb[4], uint32_t xform,
                            uint32_t clip);
    /* The layout engine whose boxes the hit test of foreignObject content asks (embi_layout_hit_svg_fo); nullptr: the
     * content is not hit. */
    const struct embi_layout_engine *layout;
} embi_svg_env;

#if !EMBER_SVG
/* EMBER_SVG=OFF (SZ3b): src/svg is not built. The painter's three entry points exist as empty stubs, so that
 * paint/ needs no #if: no element is ever an svg box (layout makes none) and no SVG image is ever found, so they are not reached. */
enum { EMBI_SVGFX_REF_NONE = 0, EMBI_SVGFX_REF_OK, EMBI_SVGFX_REF_OFF };
static inline void embi_svg_paint(const embi_svg_env *env, embi_dl *dl, const embi_element *svg, const float box[4],
                                  uint32_t xform, uint32_t clip)
{
    (void)env;
    (void)dl;
    (void)svg;
    (void)box;
    (void)xform;
    (void)clip;
}
static inline int embi_svg_ref_content(const embi_svg_env *env, embi_dl *dl, const embi_document *doc, embi_str url,
                                       bool clip_path, const float bbox[4], uint32_t xform, uint32_t clip)
{
    (void)env;
    (void)dl;
    (void)doc;
    (void)url;
    (void)clip_path;
    (void)bbox;
    (void)xform;
    (void)clip;
    return EMBI_SVGFX_REF_OFF; /* url(#id) masks and clip paths are not applied */
}
static inline void embi_svg_paint_image(const embi_svg_env *env, embi_dl *dl, const embi_svg_image *img, const float dest[4],
                                        const float uv[4], uint32_t tint, uint32_t xform, uint32_t clip)
{
    (void)env;
    (void)dl;
    (void)img;
    (void)dest;
    (void)uv;
    (void)tint;
    (void)xform;
    (void)clip;
}
#else
/* ------------------------------------------------------------------ painting */

/* Paints the outer svg element into dl. box is the content box of the svg's replaced box (x, y, w, h) in the space that
 * the display list transform index xform maps to the page, and clip is the list clip to start from. */
void embi_svg_paint(const embi_svg_env *env, embi_dl *dl, const embi_element *svg, const float box[4], uint32_t xform,
                    uint32_t clip);

/* R4e: a CSS mask-image or clip-path of an html box (or of any box with a layout box) that names an SVG mask or clipPath
 * of the document (url(#id)). Appends to dl the items of the referenced element for the box with border box bbox (x, y, w,
 * h in the space the display list transform xform maps to the page; the user space of the reference starts at its top left
 * corner, and bbox is also the object bounding box). The items are clipped to clip. A mask gives items whose alpha is the
 * mask value (luminance already applied), a clipPath solid black ones: the caller puts them in a mask layer. Result:
 * EMBI_SVGFX_REF_OK (appended, possibly nothing: then the box is masked away), EMBI_SVGFX_REF_NONE (url does not name a mask
 * or clipPath: a missing mask masks everything, a missing clip-path clips nothing) or EMBI_SVGFX_REF_OFF (the element is in
 * a tree that is not rendered: it is not applied). */
enum { EMBI_SVGFX_REF_NONE = 0, EMBI_SVGFX_REF_OK, EMBI_SVGFX_REF_OFF };
int embi_svg_ref_content(const embi_svg_env *env, embi_dl *dl, const embi_document *doc, embi_str url, bool clip_path,
                         const float bbox[4], uint32_t xform, uint32_t clip);

/* ------------------------------------------------------------------ hit test and geometry */

/* The element of the deepest rendered shape, image or text that contains the point (x, y) of the content box (the point is
 * relative to the box's origin), honoring pointer-events; nullptr when the point hits nothing of the svg. A use instance is
 * reported as the use element. */
const embi_element *embi_svg_hit(const embi_svg_env *env, const embi_element *svg, const float box[4], float x, float y);

/* Geometry of an element, in its own user space (the SVG DOM queries). The svg ancestor's viewport is vw x vh CSS px. */
typedef struct embi_svg_geom {
    const embi_svg_env *env;
    float vw, vh;
} embi_svg_geom;
bool embi_svg_bbox(const embi_svg_geom *g, const embi_element *el, double out[4]);
/* The matrix from the user space of el to the user space of its nearest svg viewport (screen == false) or, with screen, the
 * matrix to the viewport of the outermost svg (the caller composes the layout transform on top). */
bool embi_svg_ctm(const embi_svg_geom *g, const embi_element *el, bool screen, double out[6]);
bool embi_svg_total_length(const embi_svg_geom *g, const embi_element *el, double *out);
bool embi_svg_point_at_length(const embi_svg_geom *g, const embi_element *el, double dist, double out[2]);
bool embi_svg_point_in_fill(const embi_svg_geom *g, const embi_element *el, double x, double y);
bool embi_svg_point_in_stroke(const embi_svg_geom *g, const embi_element *el, double x, double y);
/* The box of a foreignObject for layout (embi_layout_svg_fo_fn): out[0], out[1] where its point (x, y) lands in the content
 * box of its outermost svg (g->vw x g->vh CSS px, its viewBox applied), out[2], out[3] its width and height in its user units.
 * False when it has no positive size. */
bool embi_svg_foreign_box(const embi_svg_geom *g, const embi_element *fo, float out[4]);

/* ------------------------------------------------------------------ SVG as an image */

/* True when the bytes look like an SVG document (an <svg> start tag after an optional BOM, XML declaration, doctype,
 * comments and whitespace). */
bool embi_svg_sniff(const uint8_t *bytes, size_t len);

/* Parses an SVG document (scripts stay off: the document is a separate DOM that never reaches the page's script) and styles
 * it with the engine. nullptr when it has no svg root. */
embi_svg_image *embi_svg_image_load(embi_style_engine *se, const uint8_t *bytes, size_t len);
void embi_svg_image_free(embi_svg_image *img);
/* The image is opened at a fragment (the part of the URL after #): the id of a <view> element, whose viewBox and
 * preserveAspectRatio then replace the root's (SVG 2 linking, section 6.4). Other fragments are ignored. */
void embi_svg_image_set_fragment(embi_svg_image *img, embi_str frag);
/* The natural size in CSS px and whether there is an intrinsic aspect ratio (SVG 2 section 8, image size negotiation:
 * width and height when both are absolute lengths, else the viewBox size, else 300x150 without a ratio). */
void embi_svg_image_size(const embi_svg_image *img, float *w, float *h, bool *has_ratio);
/* Paints the image into dl so that the part uv (u0, v0, u1, v1; 0..1 of the image) fills dest (x, y, w, h) in the space of
 * the transform xform, clipped to dest. tint multiplies the alpha (RGBA8, white is the identity). */
void embi_svg_paint_image(const embi_svg_env *env, embi_dl *dl, const embi_svg_image *img, const float dest[4],
                          const float uv[4], uint32_t tint, uint32_t xform, uint32_t clip);
/* Bytes held by the parsed image (its document and styles are shared with the engine; this counts the document only). */
size_t embi_svg_image_bytes(const embi_svg_image *img);
#endif /* EMBER_SVG */
