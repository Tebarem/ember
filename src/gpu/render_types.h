#pragma once
/*
 * Renderer data layout, shared with Slang through shaders/lib/types.slang. Keep both files in
 * sync by hand: every struct here has a matching Slang struct with the same field order, and
 * the static_asserts below pin the sizes that the GPU reads.
 *
 * Colors are RGBA8, non-premultiplied, with r in the low byte. The shader premultiplies.
 */

#include <stddef.h>
#include <stdint.h>

/* Primitive kinds. Bits 0..6 of embi_prim.kind hold one of these (the paint or box kind), bit 7
 * is EMBI_PRIM_OUTLINE, and bits 8..31 hold the index of the per-kind record: the border record,
 * the gradient record, or the atlas layer of an image. */
enum {
    EMBI_PRIM_FILL = 0,
    EMBI_PRIM_BORDER = 1,
    EMBI_PRIM_IMAGE = 2,
    EMBI_PRIM_LINEAR = 3,
    EMBI_PRIM_RADIAL = 4,
    EMBI_PRIM_CONIC = 5,
    EMBI_PRIM_SHADOW = 6, /* analytic box shadow (embi_frame_shadow): its record is in the border buffer */
    /* A layer or image drawn through a projective map (R3c, 3D transforms). The prim's xform index names two
     * consecutive xform records: the first holds the affine rows of the map (X, Y), the second the w row (see
     * embi_frame_image_proj). The index bits hold the atlas layer, as for an image. */
    EMBI_PRIM_PLANE = 12,
    /* An image that needs its own record (a sub-rect uv, or nearest sampling): the index bits hold the record index in
     * the border buffer (embi_image_rec), which carries the atlas layer and the sampling state. */
    EMBI_PRIM_IMAGE_REC = 13,
};
/* Set on an outline prim (a glyph or a path, drawn by the Slug coverage in the same ubershader).
 * Its geometry is the blob's em box and its paint kind is one of the kinds above (FILL or a
 * gradient). A box prim has this bit clear. */
#define EMBI_PRIM_OUTLINE 0x80u
#define EMBI_PRIM_KIND_MASK 0x7Fu

/* Longest clip chain the fragment shader walks. Must equal MAX_CLIP_DEPTH in
 * shaders/lib/types.slang; the configure step checks it (cmake/EmberShaders.cmake). */
#define EMBI_MAX_CLIP_DEPTH 8

/* 64 bytes. One instance per drawn primitive: a box (fill, border, image, gradient) or an outline
 * (a glyph or a path). The layout is shared by both, so one instance buffer carries every prim in
 * paint order and one pipeline draws them all (shaders/box.slang).
 *
 *   box:     rect = x, y, w, h in local (pre-transform) CSS px; radii = tl, tr, br, bl (circular).
 *   outline: rect = the em box (x0, y0, x1, y1) of the blob; radii = a, b, c, d, the 2x2 part of the
 *            map from em to local CSS px (CSS order); extra holds the translation e, f (floats in
 *            trans) and, as raw 32-bit words, the blob's byte offset in the slug buffer and the
 *            band word (bits 0..4 horizontal bands, 5..9 vertical, bit 10 even-odd).
 *   box image: extra = the uv rect (x0, y0, x1, y1) in its atlas layer.
 *   image record: extra = the uv rect as above; the index names an embi_image_rec (layer, clamp, sampling).
 * color is RGBA8 non-premultiplied, r in the low byte. xform and clip index the buffers of the
 * frame. */
typedef struct embi_prim {
    float rect[4];
    float radii[4];
    uint32_t color;
    uint32_t kind;  /* EMBI_PRIM_* in the low 7 bits, EMBI_PRIM_OUTLINE, then the index */
    uint32_t xform; /* index into the transform buffer (0 = device scale only) */
    uint32_t clip;  /* index into the clip buffer (0 = none) */
    union {
        float extra[4];                                             /* box image: uv rect */
        struct {
            float trans[2];                                         /* outline: e, f */
            uint32_t data;                                          /* outline: slug byte offset */
            uint32_t bands;                                         /* outline: band word */
        } outline;
    };
} embi_prim;

/* 2x3 affine, CSS order (a, b, c, d, e, f): x' = a*x + c*y + e, y' = b*x + d*y + f. The map is
 * local CSS px to device px, device scale already multiplied in.
 *   pad[0] = |J^-T e1| and pad[1] = |J^-T e2|, the local distance that moves an edge by one
 *   device px along its normal, per axis (J = the 2x2 part). The vertex shader expands each
 *   prim's quad by these, so the 1 px anti-aliasing fringe is never clipped, even under a
 *   non-uniform scale.
 *   pad[0] == 0 marks a degenerate transform. Nothing drawn through it is visible. */
typedef struct embi_xform {
    float m[6];
    float pad[2];
} embi_xform;

/* Rounded-rect clip in its own local space. Index 0 is a sentinel, so a real clip is >= 1.
 * The fragment shader walks the parent chain, up to EMBI_MAX_CLIP_DEPTH entries. A chain that
 * would grow past that depth is folded when its child is added (see render.c). */
typedef struct embi_clip {
    float rect[4];
    float radii[4];
    uint32_t xform;
    uint32_t parent; /* 0 = none */
    uint32_t pad[2];
} embi_clip;

/* CSS border-style values, as embi_frame_border_styled takes them (per side). NONE paints
 * nothing, as hidden does. */
enum {
    EMBI_BSTYLE_SOLID = 0,
    EMBI_BSTYLE_DOTTED,
    EMBI_BSTYLE_DASHED,
    EMBI_BSTYLE_DOUBLE,
    EMBI_BSTYLE_GROOVE,
    EMBI_BSTYLE_RIDGE,
    EMBI_BSTYLE_INSET,
    EMBI_BSTYLE_OUTSET,
    EMBI_BSTYLE_NONE,
};

/* Pattern of one border side, the per-side kind the shader reads (embi_border.pattern). Solid
 * sides are drawn with the band coverage alone. */
enum {
    EMBI_BPAT_SOLID = 0,
    EMBI_BPAT_DOTTED = 1,
    EMBI_BPAT_DASHED = 2,
    EMBI_BPAT_DOUBLE = 3,
};

/* 64 bytes. One per border prim. Each side has an outer-half color and an inner-half color: they
 * differ only for groove and ridge, which split the side along its width. The pattern selects
 * the dash, dot or double coverage that the shader applies along the side. Written by render.c. */
typedef struct embi_border {
    float widths[4];     /* top, right, bottom, left */
    uint32_t colors[4];  /* per side: the outer-half color (the whole side for most styles) */
    uint32_t inner[4];   /* per side: the inner-half color (equal to colors except groove, ridge) */
    uint32_t pattern[4]; /* per side: EMBI_BPAT_* */
} embi_border;

/* 64 bytes. One per image prim that needs more than a whole image sampled linearly (EMBI_PRIM_IMAGE_REC). clamp is
 * the uv rect (x0, y0, x1, y1) inset by half a texel on each side (see embi_frame_image_ex), so a sub-rect never
 * samples the texels next to it. texel is 1 / page size, the size of one texel in uv. nearest selects nearest-texel
 * sampling. Written by render.c. */
typedef struct embi_image_rec {
    float clamp[4];
    float texel[2];
    uint32_t layer;
    uint32_t nearest;
    uint32_t pad[8];
} embi_image_rec;

/* Gradient records, one per gradient prim (its kind index). The stops of a gradient are a run in
 * the stop buffer, from first_stop, with stop_count entries. Written by render.c. */
typedef struct embi_gradient {
    float p0[2];       /* linear: start; radial: center; conic: center */
    float p1[2];       /* linear: end; radial: radii; conic: angle packed in p1[0] */
    uint32_t first_stop;
    uint32_t stop_count;
    uint32_t repeat;
    uint32_t pad;
} embi_gradient;

typedef struct embi_stop {
    float offset;
    uint32_t color;
} embi_stop;

/* Per-frame constants, stored in a one-element storage buffer read by the vertex stage. */
typedef struct embi_frame_params {
    float viewport[2]; /* device px */
    float pad[2];
} embi_frame_params;

/* One quadratic segment of an outline in em (font units) or path space: start, control and end.
 * Segments of one outline must close (each contour ends where it starts). */
typedef struct embi_qcurve {
    float x0, y0;
    float cx, cy;
    float x1, y1;
} embi_qcurve;

static_assert(sizeof(embi_prim) == 64, "embi_prim must be 64 bytes");
static_assert(offsetof(embi_prim, outline) == 48, "outline words start at byte 48");
static_assert(sizeof(embi_qcurve) == 24, "embi_qcurve must be 24 bytes");
static_assert(_Alignof(embi_prim) == 4, "embi_prim alignment");
static_assert(sizeof(embi_xform) == 32, "embi_xform must be 32 bytes");
static_assert(sizeof(embi_clip) == 48, "embi_clip must be 48 bytes");
static_assert(sizeof(embi_border) == 64, "embi_border must be 64 bytes");
static_assert(sizeof(embi_image_rec) == sizeof(embi_border), "an image record is one border record");
static_assert(sizeof(embi_gradient) == 32, "embi_gradient must be 32 bytes");
static_assert(sizeof(embi_stop) == 8, "embi_stop must be 8 bytes");
static_assert(sizeof(embi_frame_params) == 16, "embi_frame_params must be 16 bytes");
