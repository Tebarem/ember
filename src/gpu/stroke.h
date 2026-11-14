#pragma once
/*
 * Stroke expansion: a path plus stroke parameters becomes a fill path that Slug draws with the
 * nonzero rule.
 *
 *     embi_stroke_style st = {.width = 2, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_MITER,
 *                             .miter_limit = 4};
 *     embi_path *s = embi_path_stroke(p, &st, 0);
 *     embi_frame_fill_path(f, s, EMBI_FILL_NONZERO, &paint, xform, clip);
 *     embi_path_destroy(s);
 *
 * How it works: the result is a union of closed pieces, every one with the same winding: a body
 * per segment (a rectangle for a line, a ribbon between two offset quadratics for a curve), a
 * wedge per join, a shape per cap. Under the nonzero rule overlaps do not matter, so the pieces
 * never need to be clipped against each other. Fill the result with EMBI_FILL_NONZERO only.
 *
 * Curves: a quadratic is split at t = 1/2 until the offset of each piece is a quadratic (the
 * offset of the control polygon, exact at the ends) that stays within tolerance / 2 of the true
 * offset at three sampled points, and its turning angle is small. A piece whose radius of
 * curvature is below about 1.25 half-widths (the inner offset would flip over) is flattened into
 * lines instead and drawn as rectangles with round wedges between them, which is exact up to the
 * flattening error (tolerance / 2). Curve cusps (a quadratic that doubles back) split into two
 * lines with a round join between them, as the true offset of a cusp is round.
 *
 * Hairlines: with min_device_width > 0 a stroke thinner than that many device pixels is widened to
 * it, and *coverage (embi_path_stroke_ex) says by how much its alpha must be scaled down
 * (width * scale / min_device_width, else 1). scale is device px per local px (the uniform scale
 * of the transform the caller will draw with). tolerance is in local px: pass
 * device_tolerance / scale. Non-uniform scales are not handled here: for them, transform the path
 * to device space first and stroke it there with scale 1.
 *
 * Limits: a width above 2e8 is clamped. The output stops growing at 2^21 quadratics (about
 * 50 MB); the rest of the stroke is dropped. A dash pattern that would make more than 1e5 dashes on
 * a subpath is ignored for that subpath (it is stroked solid). Runs of segments shorter than
 * tolerance / 20 are merged into chords, so the dropped length per gap stays below that. Curve
 * flattening caps at 256 chords per piece, so for extreme half-widths (about 1e8) the tolerance / 2
 * bound can be exceeded. A zero-length subpath with a dash array draws a dot only if the pattern
 * (after dash_offset) starts on a zero-length dash; one that starts in a gap or a positive dash
 * draws no dot.
 */

#include "gpu/path.h"

typedef enum embi_stroke_cap {
    EMBI_CAP_BUTT = 0,
    EMBI_CAP_ROUND,
    EMBI_CAP_SQUARE,
} embi_stroke_cap;

typedef enum embi_stroke_join {
    EMBI_JOIN_MITER = 0,
    EMBI_JOIN_ROUND,
    EMBI_JOIN_BEVEL,
} embi_stroke_join;

typedef struct embi_stroke_style {
    float width;            /* local px; not finite or <= 0 draws nothing */
    embi_stroke_cap cap;    /* ends of open subpaths and of dashes */
    embi_stroke_join join;  /* corners */
    float miter_limit;      /* miter length / width; not finite or < 1 selects 4 */
    const float *dash;      /* dash lengths (on, off, on, ...); nullptr or 0 entries: solid */
    uint32_t dash_count;    /* an odd count repeats the array. Any negative or non-finite entry, or
                               a zero sum, turns dashing off. */
    float dash_offset;      /* distance into the pattern where each subpath starts; may be negative */
    float scale;            /* device px per local px; 0 is 1 */
    float min_device_width; /* hairline floor in device px; 0 is off */
} embi_stroke_style;

/* Returns a new path (the caller destroys it), nullptr only when in or st is nullptr
 * (allocation failure aborts). tolerance: the largest offset error in local px; 0 selects 1/64. Subpaths of
 * zero length draw a dot with round caps and a square with square caps, as SVG and Canvas say. */
[[nodiscard]] embi_path *embi_path_stroke(const embi_path *in, const embi_stroke_style *st,
                                          float tolerance);
/* As embi_path_stroke; coverage (may be nullptr) receives the alpha factor of the hairline rule. */
[[nodiscard]] embi_path *embi_path_stroke_ex(const embi_path *in, const embi_stroke_style *st,
                                             float tolerance, float *coverage);

/* Strokes p and fills the result in one call. A solid paint has its alpha scaled by the hairline
 * coverage; other paints are drawn unscaled. The expanded outline's blob is cached under a hash of
 * the path and the style (width, caps, joins, miter limit, dashes, tolerance), so a stroke drawn
 * again is not expanded again. */
void embi_frame_stroke_path(embi_frame *f, const embi_path *p, const embi_stroke_style *st,
                            float tolerance, const embi_paint *paint, uint32_t xform,
                            uint32_t clip);
