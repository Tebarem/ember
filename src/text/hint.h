#pragma once
/*
 * Vertical glyph hinting, in the manner of a light autohinter. The horizontal edges of a glyph (the crossbar of e and H, the
 * bars of z, the tops of the letters, the baseline) are moved onto pixel rows for one size, and every other point follows by
 * piecewise-linear interpolation. Only y changes: x stays exact. Included by draw.c only.
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
