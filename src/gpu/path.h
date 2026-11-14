#pragma once
/*
 * Paths: a builder for quadratic outlines in local CSS px, and filled paths on the Slug renderer.
 *
 *     embi_path *p = embi_path_create(0);       // 0: the default tolerance
 *     embi_path_move_to(p, 10, 10);
 *     embi_path_cubic_to(p, 40, 0, 60, 40, 30, 40);
 *     embi_path_close(p);
 *     embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, xform, clip);
 *     embi_path_destroy(p);
 *
 * Cubics are split into quadratics with a known error bound: one quadratic replacing a cubic is
 * off by at most (sqrt(3) / 36) |P0 - 3 P1 + 3 P2 - P3|, and each split divides that by 8. Arcs
 * become cubics of at most a fraction of a turn, which are then flattened the same way. A
 * segment's error stays within the tolerance given at creation (in local px), except where the
 * subdivision depth is capped (a cubic far larger than its tolerance).
 *
 * Commands follow SVG and Canvas. A segment with no current subpath starts one at its first point
 * (the endpoint for a line or an arc, the first control point for a quadratic or a cubic). A
 * subpath that is not closed is filled as if it were: the fill adds the closing line.
 *
 * Coordinates must be finite. A command with a non-finite argument returns false and changes
 * nothing.
 */

#include <stdbool.h>
#include <stdint.h>

#include "gpu/render.h"

typedef struct embi_path embi_path;

/* tolerance: the largest error allowed when flattening, in local px. 0 (or negative) selects the
 * default, 1/64 px. Returns nullptr on allocation failure. */
[[nodiscard]] embi_path *embi_path_create(float tolerance);
void embi_path_destroy(embi_path *p);
/* Removes every segment. The path keeps its identity, so the blob cache sees the change. */
void embi_path_clear(embi_path *p);

bool embi_path_move_to(embi_path *p, float x, float y);
bool embi_path_line_to(embi_path *p, float x, float y);
bool embi_path_quad_to(embi_path *p, float cx, float cy, float x, float y);
bool embi_path_cubic_to(embi_path *p, float c1x, float c1y, float c2x, float c2y, float x,
                        float y);
/* SVG endpoint arc from the current point to (x, y). rotation is the x axis rotation in radians.
 * Radii of zero draw a line. Radii too small for the endpoints are scaled up, as SVG says. */
bool embi_path_arc_to(embi_path *p, float rx, float ry, float rotation, bool large_arc,
                      bool sweep, float x, float y);
/* Closes the current subpath with a line back to its start. The next segment starts a new
 * subpath there. */
bool embi_path_close(embi_path *p);
bool embi_path_is_empty(const embi_path *p);

/* Internal (tests now, stroking in R4): the quadratic segments in order, without the closing lines
 * that the fill adds for open subpaths, and the number of subpaths. The pointer is valid until the
 * path changes. */
const embi_qcurve *embi_path_segments(const embi_path *p, uint32_t *count);
uint32_t embi_path_subpath_count(const embi_path *p);

/* Internal (stroking): one subpath. first/count index embi_path_segments. count is 0 for a point
 * subpath (a line to its own start, or move + close). closed: ended by embi_path_close. */
typedef struct embi_path_subpath_info {
    uint32_t first;
    uint32_t count;
    float sx, sy;
    bool closed;
} embi_path_subpath_info;
bool embi_path_subpath(const embi_path *p, uint32_t k, embi_path_subpath_info *out);

/* Fills the path with paint: its region under rule, in local space, transformed by xform. The
 * path is not const: it may rebuild its fill outline (closing its open subpaths). */
void embi_frame_fill_path(embi_frame *f, embi_path *p, embi_fill_rule rule,
                          const embi_paint *paint, uint32_t xform, uint32_t clip);
