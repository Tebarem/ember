#pragma once
/*
 * Outline emboldening for synthetic bold (text/draw.c). Included by text modules only.
 */

#include "gpu/render_types.h"

#include <stdbool.h>
#include <stdint.h>

/* Grows an outline of n curves (font units, y up). starts[c] is the index of the first curve of contour c (ascending, starts[0]
 * is 0; nc contours, nc >= 1); each contour is closed by its last curve ending on its first point. Each contour's role is its
 * nesting: a contour inside an odd number of other contours is a hole. Outer contours grow and holes shrink, by xstr in x and
 * ystr in y in total (each side by half). anchor_bl keeps the left edge and the baseline where they are and grows right and up
 * (FreeType's bottom-left anchor, for hinted outlines); otherwise the outline is centred. A corner moves along its edge normals
 * with the per-axis half strengths (FreeType's EmboldenXY shift), capped at 4 half strengths (the miter limit). A hole's shift
 * is at most 0.45 times the smaller extent of its bounding box. out has room for n curves and must not alias in. The curve
 * count does not change. */
void embi_embolden(const embi_qcurve *in, uint32_t n, const uint32_t *starts, uint32_t nc, float xstr, float ystr, bool anchor_bl,
                   embi_qcurve *out);
