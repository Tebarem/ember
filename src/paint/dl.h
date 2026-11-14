#pragma once
/*
 * The display list types (W4c): the item kinds, items, clips, transforms, stops,
 * stacking contexts and the list itself. They live apart from paint.h, which pulls in the layout and style headers,
 * so that the damage and the renderer tests can include them in a translation unit that also includes the renderer
 * header (gpu/render.h and the CSS headers define clashing names). paint.h includes this header.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Item kinds. */
enum embi_di_kind {
    EMBI_DI_FILL = 1,   /* rect, radii, colors[0] (solid) */
    EMBI_DI_BORDER,     /* rect (the outer edge), radii, widths and colors and styles per side (top, right,
                         * bottom, left); also an outline (drawn outside its box with its own rect) */
    EMBI_DI_IMAGE,      /* rect (destination), aux: URL index, f[0..3]: uv rect (x0, y0, x1, y1),
                         * colors[0]: tint, flags: EMBI_DF_NEAREST for pixelated sampling. With EMBI_DF_TILE the
                         * item is a tiled image instead: rect is the area the tiles cover (the clip of the layer
                         * is applied by the item's clip), f[0], f[1] the origin of the first tile, f[2], f[3] the
                         * tile size and f[4], f[5] the distance from one tile origin to the next (the tile size
                         * for repeat, the tile size plus the gap for space, the rounded size for round). The
                         * tiles are the origins first + (i, j) * step that meet rect, each drawn whole with the
                         * full uv rect (0, 0, 1, 1), or, when widths[2] > widths[0] and widths[3] > widths[1], with
                         * the uv rect widths[0..3] (x0, y0, x1, y1) and each tile clipped to rect (a border image's
                         * tile is a slice of its source and may overhang the area). One item covers any number of tiles. */
    EMBI_DI_GRADIENT,   /* rect (the tile), radii, sub: EMBI_GRAD_*, f: geometry (linear x0 y0 x1 y1,
                         * radial cx cy rx ry, conic cx cy from-radians), aux, aux2: stops in dl->stops,
                         * flags: EMBI_DF_REPEAT. The stops are sorted by offset (non-decreasing) and carry the
                         * interpolated curve of the CSS color hints, the premultiplied blend and the
                         * non-sRGB interpolation spaces as extra stops (background.c). EMBI_DF_REPEAT with a
                         * linear gradient whose stops run from 0 to 1 repeats the gradient along its line (a
                         * background tile repeated along the line, one item for the whole repeated area). */
    EMBI_DI_TEXT,       /* rect (the text fragment), aux: run index, aux2: end byte, aux3: begin byte,
                         * f[0], f[1]: the baseline origin (x, y), colors[0] */
    EMBI_DI_DECORATION, /* rect (the line), sub: EMBI_DECO_*, colors[0] */
    EMBI_DI_LAYER_BEGIN, /* f[0]: opacity; aux: the index of the matching LAYER_END. With sub == EMBI_LS_PLANE the group
                          * is a plane of a 3D transform (R3c): its items are in the plane's own local space (no
                          * transform of the plane in them), aux2 indexes dl->planes, clip is the clip of the
                          * composite (in the space the group is composed into) and the composite applies the
                          * plane's homography. It always renders into a layer. */
    EMBI_DI_LAYER_END,   /* aux: the index of the matching LAYER_BEGIN */
    EMBI_DI_TRIANGLE,    /* rect (the box of the triangle), sub: EMBI_TRI_*, colors[0]: the disclosure marker */
    EMBI_DI_CHECK,       /* W5e: a check mark filling rect (the polygon of a checkbox's tick), colors[0] */
    EMBI_DI_SHADOW,      /* a box shadow (R3a): rect (the area it covers: the shadow grown by 3.5 sigma, or the padding
                          * box of an inset shadow), sub: EMBI_SHADOW_*, colors[0], aux: the index in dl->shadows */
    EMBI_DI_BACKDROP,    /* backdrop-filter (R3a): rect and radii (the border box), aux2, aux3: the filter ops
                          * [aux2, aux2 + aux3) in dl->fx. Replay copies what is under the box, filters the copy and
                          * draws it clipped to the box */
    EMBI_DI_PATH,        /* R4c: a path filled with a paint (SVG shapes and strokes). aux: the index in dl->paths (the path is
                          * in the item's own space, under xform), rect: the bounds of the path in that space, sub: the fill
                          * rule (0 nonzero, 1 evenodd), styles[0]: the paint, 0 for the solid colors[0] or
                          * EMBI_GRAD_LINEAR_DL and EMBI_GRAD_RADIAL_DL for a gradient whose stops are dl->stops
                          * [aux2, aux2 + aux3) and whose geometry is f[0..3] (the gradient geometry in the path's space;
                          * flags: EMBI_DF_REPEAT repeats the stops over 0..1). */
};

/* LAYER_BEGIN sub value: the group is filtered (R3a). aux2, aux3: the filter ops [aux2, aux2 + aux3) in dl->fx,
 * applied in order to the rendered group before its opacity (f[0]); xform: the transform of the filtered box, whose
 * scale turns CSS lengths of the filter into layer px. Such a group is always an intermediate target. */
#define EMBI_LAYER_FILTERED 1u

/* Shadow modes of an EMBI_DI_SHADOW item (sub). */
enum {
    EMBI_SHADOW_OUTER = 0,
    EMBI_SHADOW_INSET = 1,
};

/* Directions of a triangle (the disclosure markers): the way it points. */
enum {
    EMBI_TRI_RIGHT = 0,
    EMBI_TRI_DOWN,
    EMBI_TRI_LEFT,
    EMBI_TRI_UP,
};

/* LAYER_BEGIN sub values. */
#define EMBI_LS_PLANE 2u /* distinct from EMBI_LAYER_FILTERED (R3a) */
/* R4c: a mask layer. Its items render into a layer of their own, which is multiplied into the enclosing group with the
 * destination-in blend (only the alpha of the layer counts: a luminance mask has its colors converted to alpha when the
 * list is built). It sits last in an EMBI_LS_ISO group. Not the CSS mask layer EMBI_LS_MASK (R3b). */
#define EMBI_LS_SVGMASK 7u
/* R4c: an isolated group that always renders into a layer (f[0]: opacity), so that a mask group inside it applies to its
 * content only. */
#define EMBI_LS_ISO 8u

/* LAYER_BEGIN sub value EMBI_LS_COMPOSITE (R3b): a group with a blend mode, a clip-path, a mask or an isolation. Replay
 * renders the group into a layer, applies the filter steps [aux2, aux2 + aux3) of dl->fx (as EMBI_LAYER_FILTERED),
 * then the clip-path, the mask, the opacity f[0] and blends the result onto what the target holds. Fields:
 *   colors[0]  the blend mode, EMBI_BM_*
 *   colors[1]  1 + the index of the clip-path in dl->cpaths, or 0
 *   colors[2]  the number of mask layers: the group's first colors[2] children are EMBI_LS_MASK groups, topmost mask
 *              layer first; the rest is the content
 *   flags      EMBI_DF_ISOLATE: a plain isolated group (isolation, a stacking context with a blending descendant, a
 *              backdrop root) that has none of the above
 *   xform, clip as for EMBI_LAYER_FILTERED: the transform of the box (the clip-path is in its space) and the clip the
 *              composite applies. */
#define EMBI_LS_COMPOSITE 3u
/* LAYER_BEGIN sub value EMBI_LS_MASK (R3b): one layer of a mask-image list, a child of a COMPOSITE group. Its items are
 * the mask image (tiles and gradients, clipped to the mask-clip area). colors[0]: the mask-mode, EMBI_MM_*; colors[1]:
 * the mask-composite operator with the layers below (EMBI_MC_*). An empty layer still counts (it is transparent). */
#define EMBI_LS_MASK 4u

/* Blend modes (colors[0] of a COMPOSITE group), numbered as gpu/fx.h embi_fx_blend. */
enum {
    EMBI_BM_NORMAL = 0,
    EMBI_BM_MULTIPLY,
    EMBI_BM_SCREEN,
    EMBI_BM_OVERLAY,
    EMBI_BM_DARKEN,
    EMBI_BM_LIGHTEN,
    EMBI_BM_COLOR_DODGE,
    EMBI_BM_COLOR_BURN,
    EMBI_BM_HARD_LIGHT,
    EMBI_BM_SOFT_LIGHT,
    EMBI_BM_DIFFERENCE,
    EMBI_BM_EXCLUSION,
    EMBI_BM_HUE,
    EMBI_BM_SATURATION,
    EMBI_BM_COLOR,
    EMBI_BM_LUMINOSITY,
    EMBI_BM_PLUS_LIGHTER,
    EMBI_BM_PLUS_DARKER, /* a separable approximation (see fx.h), not the exact plus-darker */
};
/* Mask modes and mask-composite operators of a mask layer (match-source is alpha: there are no SVG masks). */
enum { EMBI_MM_ALPHA = 0, EMBI_MM_LUMINANCE = 1 };
enum { EMBI_MC_ADD = 0, EMBI_MC_SUBTRACT, EMBI_MC_INTERSECT, EMBI_MC_EXCLUDE };

/* Item flags. */
#define EMBI_DF_REPEAT 0x01u  /* gradient: repeating */
#define EMBI_DF_NEAREST 0x02u /* image: nearest sampling (image-rendering: pixelated, crisp-edges) */
#define EMBI_DF_TILE 0x04u    /* image: a tiled image, see EMBI_DI_IMAGE */
#define EMBI_DF_NOFOLD 0x10u  /* text: its glyphs overlap (negative letter-spacing), so an opacity layer may not fold into its color */
#define EMBI_DF_ISOLATE 0x08u /* LAYER_BEGIN, EMBI_LS_COMPOSITE: a plain isolated group */
/* PATH (R4c): the path is a center line that the renderer strokes when it draws (a stroke costs the list its center line,
 * not the expanded outline): f[0] the width, f[1] the miter limit, styles[1] the cap (0 butt, 1 round, 2 square), styles[2]
 * the join (0 miter, 1 round, 2 bevel). The paint is the solid colors[0]. */
#define EMBI_DF_STROKE 0x08u

/* Decoration styles (text-decoration-style, as in CSS Text Decoration 3). */
enum {
    EMBI_DECO_SOLID = 0,
    EMBI_DECO_DOUBLE,
    EMBI_DECO_DOTTED,
    EMBI_DECO_DASHED,
    EMBI_DECO_WAVY,
};

/* Border styles of a BORDER item, per side. They have the values of gpu/render_types.h EMBI_BSTYLE_* (the
 * replay maps them one to one; that header cannot be included here, since its struct names clash with the CSS
 * ones in one translation unit). */
enum {
    EMBI_DS_SOLID = 0,
    EMBI_DS_DOTTED,
    EMBI_DS_DASHED,
    EMBI_DS_DOUBLE,
    EMBI_DS_GROOVE,
    EMBI_DS_RIDGE,
    EMBI_DS_INSET,
    EMBI_DS_OUTSET,
    EMBI_DS_NONE,
};

/* Gradient kinds (embi_di_kind EMBI_DI_GRADIENT, sub). */
enum {
    EMBI_GRAD_LINEAR_DL = 1,
    EMBI_GRAD_RADIAL_DL,
    EMBI_GRAD_CONIC_DL,
};

/* One item. Fields a kind does not use are zero. The record is 124 bytes, with no padding (damage.c memcmps items). */
typedef struct embi_ditem {
    uint8_t kind;       /* EMBI_DI_* */
    uint8_t sub;        /* gradient kind, decoration style */
    uint8_t flags;      /* EMBI_DF_* */
    uint8_t styles[4];  /* border: per side (top, right, bottom, left) EMBI_DS_* */
    uint32_t xform;     /* index into xforms (0 = identity) */
    uint32_t clip;      /* index into clips (0 = none) */
    float rect[4];      /* x, y, w, h in page px */
    float radii[4];     /* top-left, top-right, bottom-right, bottom-left (circular, px) */
    float widths[4];    /* border: top, right, bottom, left */
    uint32_t colors[4]; /* RGBA8 non-premultiplied, r in the low byte */
    uint32_t aux, aux2, aux3;
    float f[6];
    /* EMBI_DI_TEXT: embi_tshim_content_hash of the run's bytes [aux3, aux2), taken when the item is made. The run is a
     * layout pointer that dies with the next layout, while a list outlives it as the previous list the damage compares
     * against, so the item hash (embi_layer_item_hash) must not read the run. 0 for the other kinds. Two halves, so the
     * record keeps 4-byte alignment and no padding (embi_ditem_chash, embi_ditem_set_chash). */
    uint32_t chash[2];
} embi_ditem;
static_assert(sizeof(embi_ditem) == 124, "embi_ditem has no padding after xform: damage.c memcmps the items");

static inline uint64_t embi_ditem_chash(const embi_ditem *it)
{
    return (uint64_t)it->chash[0] | ((uint64_t)it->chash[1] << 32);
}

static inline void embi_ditem_set_chash(embi_ditem *it, uint64_t h)
{
    it->chash[0] = (uint32_t)h;
    it->chash[1] = (uint32_t)(h >> 32);
}

/* A clip rounded rect in page px, with the transform it is in and its parent clip (0 = none). Index 0 is a
 * sentinel and never used by an item. */
typedef struct embi_dclip {
    float rect[4];
    float radii[4];
    uint32_t xform;
    uint32_t parent;
} embi_dclip;

/* A 2D affine transform (CSS order a b c d e f): x' = a x + c y + e, y' = b x + d y + f. Index 0 is the
 * identity. */
typedef struct embi_dxform {
    float m[6];
    uint32_t scroll; /* 1 + the index in dl->scrolls of the innermost scroll container whose content the transform is in, or 0 */
} embi_dxform;

/* A scroll container with an offset: the scroll offset is a translation, the first
 * transform of the container's content (xform), and every transform made inside the container (its descendants' own, an
 * inline SVG's) carries the container in its scroll field. A scroll that changes nothing else then moves the content by
 * patching those transforms (embi_dl_scroll_retarget) instead of building the list again. */
typedef struct embi_dscroll {
    uint64_t key;     /* the fragment of the container, as an integer */
    uint32_t xform;   /* the container's content transform: the outer transform moved by minus the offset */
    uint32_t outer;   /* the transform the container's box is in (0: none) */
    uint32_t parent;  /* 1 + the enclosing scroll container's index, or 0 */
    float sx, sy;     /* the offset (CSS px, rounded to device px) the transforms are made for */
    float lin[4];     /* the linear part (a, b, c, d) of the outer transform */
    float rect[4];    /* the container's box (x, y, w, h) in the space of its outer transform: what a scroll of it redraws */
    bool root;        /* the root scroller (the viewport) */
} embi_dscroll;

/* A plane of a 3D transform (R3c): the homography from the plane's local px (the space of its items) to the px of the
 * space it is composed into, row-major as X = h0 x + h1 y + h2, Y = h3 x + h4 y + h5, W = h6 x + h7 y + h8 (the point
 * is (X / W, Y / W)), and the bounds of the plane's content in its local px (x0, y0, x1, y1). */
typedef struct embi_dplane {
    float h[9];
    float bounds[4];
} embi_dplane;

/* A color stop of a gradient: the offset is in the gradient's own units (see EMBI_DI_GRADIENT). */
typedef struct embi_dstop {
    float offset;
    uint32_t color;
} embi_dstop;

/* The two rounded rects and the blur of a box shadow (gpu/render.h embi_frame_shadow): the shape that is blurred and
 * the sharp rect that clips (outer: cut out of the shadow, inset: the shadow is clipped to it). Page px. */
typedef struct embi_dshadow {
    float shape[4];
    float shape_radii[4];
    float clip[4];
    float clip_radii[4];
    float sigma; /* the Gaussian standard deviation, px (half the CSS blur radius) */
} embi_dshadow;

/* One step of a filter list (R3a). Color functions are folded into a matrix at paint time. */
enum {
    EMBI_FX_BLUR = 1,   /* sigma: the standard deviation in CSS px */
    EMBI_FX_MATRIX,     /* m: the 4x5 color matrix on unpremultiplied colors, in feColorMatrix order */
    EMBI_FX_SHADOW,     /* drop-shadow: dx, dy, sigma in CSS px, color */
};
typedef struct embi_dfx {
    uint8_t kind;
    float sigma;
    float dx, dy;
    uint32_t color; /* RGBA8 non-premultiplied */
    float m[20];
} embi_dfx;

/* A quadratic Bezier curve of a clip-path outline (gpu/render.h embi_qcurve has the same layout). */
typedef struct embi_dcurve {
    float x0, y0, cx, cy, x1, y1;
} embi_dcurve;

/* A clip-path (R3b), in the space of the COMPOSITE group's transform. EMBI_CP_EMPTY clips everything away. EMBI_CP_RRECT is
 * the rounded rect with circular radii (rect, radii); EMBI_CP_PATH the outline of curves [first, first + n) in
 * dl->curves, filled by the fill rule. bounds is the box (x0, y0, x1, y1) of the shape. */
enum { EMBI_CP_EMPTY = 0, EMBI_CP_RRECT, EMBI_CP_PATH };
typedef struct embi_dcpath {
    uint8_t kind;
    uint8_t evenodd;
    float rect[4];
    float radii[4];
    uint32_t first, n;
    float bounds[4];
} embi_dcpath;

/* A stacking context: its item range in the list and its parent context (0 is the root). */
/* A path of an EMBI_DI_PATH item: the list owns it. hash identifies its content (equal hashes mean equal paths), so that
 * two lists are compared and damage is computed without comparing segments. */
struct embi_path;
typedef struct embi_pathref embi_pathref; /* the reference count shared by the lists that hold the same path */
typedef struct embi_dpath {
    struct embi_path *path;
    uint64_t hash;
    embi_pathref *ref;
} embi_dpath;

typedef struct embi_dctx {
    uint32_t first, end;
    uint32_t parent;
} embi_dctx;

typedef struct embi_dl {
    EMBI_VEC(embi_ditem) items;
    EMBI_VEC(embi_dclip) clips;
    EMBI_VEC(embi_dxform) xforms;
    EMBI_VEC(embi_dscroll) scrolls; /* the scroll containers with an offset that the list was built for */
    uint32_t cur_scroll;            /* build state: the scroll container whose content is being painted (index + 1, or 0) */
    bool scroll_rigid;              /* build result: something depends on the offsets other than through the transforms
                                     * (sticky boxes, fixed backgrounds, local backgrounds), so a scroll rebuilds the list */
    EMBI_VEC(embi_dstop) stops;
    EMBI_VEC(embi_dctx) ctxs;
    EMBI_VEC(embi_dshadow) shadows; /* EMBI_DI_SHADOW aux */
    EMBI_VEC(embi_dfx) fx;          /* filter steps of LAYER_BEGIN (EMBI_LAYER_FILTERED) and EMBI_DI_BACKDROP */
    uint32_t fx_items;              /* filtered groups and backdrop items in the list (damage redraws fully when > 0) */
    EMBI_VEC(embi_dplane) planes;  /* the 3D planes (LAYER_BEGIN with sub EMBI_LS_PLANE) */
    EMBI_VEC(embi_dcpath) cpaths;  /* the clip-paths of COMPOSITE groups (R3b) */
    EMBI_VEC(embi_dcurve) curves;  /* the outlines of the path clip-paths */
    EMBI_VEC(embi_dpath) paths;    /* R4c: the paths of EMBI_DI_PATH items. Paths are reference counted: a list holds one
                                    * reference to each and the incremental cache's copy of a list shares them. */
    EMBI_VEC(const void *) runs;   /* shaped text runs (layout), indexed by EMBI_DI_TEXT aux */
    EMBI_VEC(void *) own_runs;     /* W5e: runs shaped by the painter (form control text); the list owns and frees them */
    EMBI_VEC(uint32_t) urls;       /* pairs: offset and length into urltext, per URL index */
    EMBI_VEC(char) urltext;
} embi_dl;

/* What a retarget changed on screen: the boxes (x0, y0, x1, y1 in page px) of the containers that moved, or full when the
 * viewport scrolled or there are too many. */
#define EMBI_SCROLL_DAMAGE_MAX 16u
typedef struct embi_scroll_damage {
    uint32_t n;
    bool full;
    float r[EMBI_SCROLL_DAMAGE_MAX][4];
} embi_scroll_damage;

/* The scroll offsets of the list's containers, moved to the offsets in off (pairs of x, y per container, in order of
 * dl->scrolls, rounded to device px by the caller) by patching the transforms. False (nothing changed) when the list cannot
 * be moved this way: it holds 3D planes, filters, or something that depends on the offsets in another way. */
bool embi_dl_scroll_retarget(embi_dl *d, const float *off, embi_scroll_damage *dmg);

/* Adds a path to the list's table (the list takes it) and returns its index; embi_dl_copy_paths shares the paths of one list
 * with another. */
uint32_t embi_dl_add_path(embi_dl *d, struct embi_path *path, uint64_t hash);
void embi_dl_copy_paths(embi_dl *dst, const embi_dl *src);
/* Another table entry for the path at index pi (one more reference); returns the new index. */
uint32_t embi_dl_share_path(embi_dl *d, uint32_t pi);

void embi_dl_init(embi_dl *d);
/* Empties the list and keeps its arrays. */
void embi_dl_reset(embi_dl *d);
void embi_dl_free(embi_dl *d);
/* Bytes held by the list's arrays (the bench reports it per box). */
size_t embi_dl_bytes(const embi_dl *d);
/* The URL of index i (empty when out of range). */
embi_str embi_dl_url(const embi_dl *d, uint32_t i);
