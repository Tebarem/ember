#pragma once
/*
 * Vertical glyph hinting, in the manner of a light autohinter. The horizontal edges of a glyph (the crossbar of e and H, the
 * bars of z, the tops of the letters, the baseline) are moved onto pixel rows for one size, and every other point follows by
 * piecewise-linear interpolation. Only y changes unless x fitting is asked for (the sharp mode): then the vertical stems of
 * the glyph (the sides of l, H, o, n) also go onto whole device columns. Pixel-art glyphs (every segment axis-aligned on a
 * common grid) have their own detection and snapping. Included by draw.c only.
 */

#include "gpu/render_types.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    EMBI_HINT_BASELINE,
    EMBI_HINT_XHEIGHT,
    EMBI_HINT_CAP,
    EMBI_HINT_ASCENDER,
    EMBI_HINT_DESCENDER,
    EMBI_HINT_FIGURES,
    EMBI_HINT_ZONES
};

/* An alignment zone in font units: the flat edge value and the overshoot value (equal when the face has no overshoot there). */
typedef struct embi_hint_zone {
    float flat, over;
} embi_hint_zone;

typedef struct embi_hint_zones {
    embi_hint_zone z[EMBI_HINT_ZONES];
    uint32_t n_valid_mask; /* bit i: zone i is usable */
} embi_hint_zones;

/* The vertical extent (font units, curve extrema included) of the glyph of code point cp; false when the face has none. */
typedef bool (*embi_hint_extent_fn)(void *user, uint32_t cp, float *ymin, float *ymax);

/* Measures the zones of a face from the extents of x, o, H, O, d, l, b, p, g, 1, 4 and 0. os2_xheight (0 when unknown) stands
 * in for x. Zones whose glyphs are missing, or whose overshoot is 0.05 em or more, stay invalid. */
void embi_hint_zones_measure(embi_hint_zones *out, float upem, float os2_xheight, embi_hint_extent_fn fn, void *user);

/* True when the zones are good enough to hint text (an x-height or a cap height is known). */
bool embi_hint_zones_usable(const embi_hint_zones *z);

/* The vertical extent of n curves, extrema included. */
void embi_hint_extent(const embi_qcurve *c, uint32_t n, float *ymin, float *ymax);

/* Hints one glyph. in: n curves in font units (y up); s: device px per font unit vertically, with the baseline on a pixel row.
 * out has room for 2 * n curves (a curve is cut at its vertical extremum). Returns the number of curves written, or 0 when the
 * glyph has no edge to fit (the caller then draws it with the plain scale). Deterministic. */
uint32_t embi_hint_glyph(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                         embi_qcurve *out);

/* As embi_hint_glyph, and map (n + 1 entries, or nullptr) receives the index in out of the first curve that each input curve
 * became, with map[n] the number written. */
uint32_t embi_hint_glyph_map(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                             embi_qcurve *out, uint32_t *map);

/* As embi_hint_glyph_map, and with fit_x the vertical stems are fitted as well, in x. The glyph origin must be on a whole device
 * column (the caller rounds the pen x), and s is the device px per font unit in x as well as in y. A stem is a left edge paired
 * with the next right edge that it overlaps in y, at most 0.22 em away: its width goes to a whole number of px (at least 1) and
 * its left edge to the column that keeps its centre within half a px of where it was; later stems keep their order and a gap.
 * Every other x follows by interpolation between the fitted edges, and a curve is cut at its horizontal extremum. The y fitting
 * is skipped when zones is nullptr (x alone is then fitted). out has room for 4 * n curves when fit_x (2 * n otherwise), map as
 * above. Returns 0 when nothing was fitted. With fit_x false the result is that of embi_hint_glyph_map. Deterministic: the
 * curves are a function of the arguments only. */
uint32_t embi_hint_glyph_fit(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                             embi_qcurve *out, uint32_t *map, bool fit_x);

/* Pixel-art detection. True when every segment of the n curves is axis-aligned (the control point lies on the segment) and the
 * x of every end point is a whole multiple of one step u from a common x, and the y of every end point likewise (the common
 * offsets may differ between the axes: Ahem's squares run from -0.2 to 0.8 em). u is the largest such step, in font units, found
 * to a tolerance of 1/64 unit; it is at least 1 and the glyph spans at most 64 steps in either direction. *unit receives u. */
bool embi_hint_pixel_grid(const embi_qcurve *c, uint32_t n, float *unit);

/* The scale (device px per font unit) at which a pixel glyph of step unit, drawn at scale (device px per font unit), has a whole
 * number of px per step; 0 when it is not to be snapped. natural: only when unit * scale is within 0.02 of a whole number of at
 * least 1. sharp: always, at max(1, round(unit * scale)) px per step (0 when a step is under half a px: that is not a pixel
 * grid at this size). */
float embi_hint_pixel_scale(float unit, float scale, bool sharp);

/* Puts every end point of a pixel glyph on a whole device px at scale s (from embi_hint_pixel_scale): out gets the n curves
 * with x and y rounded to a multiple of 1/s, control points on their segments. The glyph origin is taken to be on a device px. */
void embi_hint_pixel_snap(const embi_qcurve *in, uint32_t n, float s, embi_qcurve *out);

/* True when the n curves are axis-aligned and every end point lies on the grid of the given step (distances from the first end
 * point are whole multiples of it, to 1/64 unit). */
bool embi_hint_pixel_on_grid(const embi_qcurve *c, uint32_t n, float unit);

/* Whether a face is pixel art, from the outlines of sample glyphs (counts[i] == 0 entries are skipped): every sample is a pixel
 * grid, all on one common step u (the common divisor of their steps), and the em (upem) spans at least 5 and at most 64 steps.
 * *unit receives u. A text face whose '.', '-' or 'I' happen to be rectangles fails on its round letters. */
bool embi_hint_pixel_face(const embi_qcurve *const *glyphs, const uint32_t *counts, uint32_t ng, float upem, float *unit);
