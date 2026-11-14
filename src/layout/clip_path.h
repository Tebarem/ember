#pragma once
/*
 * clip-path basic shapes (R3b, CSS Masking 1 section 5 and CSS Shapes 1): the geometry of inset(), xywh(), rect(),
 * circle(), ellipse(), polygon() and path() against a reference box, as a rounded rect (circular radii) or an outline of
 * quadratic curves. Paint turns the result into the clip of a group; hit testing asks whether a point is inside. Everything
 * is in the page frame of the layout, in px.
 */
#include "base/base.h"
#include "css/props.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_lay_curve {
    float c[6]; /* x0, y0, control x, control y, x1, y1 */
} embi_lay_curve;
typedef EMBI_VEC(embi_lay_curve) embi_lay_curves;

enum { EMBI_LCP_EMPTY = 0, EMBI_LCP_RRECT, EMBI_LCP_PATH };

/* The resolved shape. EMPTY clips everything away. RRECT: rect (x, y, w, h) with circular corner radii (top-left,
 * top-right, bottom-right, bottom-left). PATH: the curves [first, first + n) of the vector given to the resolver, filled by
 * the rule. bounds is x0, y0, x1, y1 of the shape. */
typedef struct embi_lay_cp {
    uint8_t kind;
    bool evenodd;
    float rect[4];
    float radii[4];
    uint32_t first, n;
    float bounds[4];
} embi_lay_cp;

/* Resolves the basic shape sh against the reference box ref (x, y, w, h) into out; PATH curves are appended to cv. fs is the
 * font size of the element, vw and vh the viewport and rem the root font size. False when the shape cannot be resolved (a
 * length of another kind). */
bool embi_lay_clip_shape(const embi_shape *sh, const float ref[4], float fs, float vw, float vh, float rem, embi_lay_cp *out,
                         embi_lay_curves *cv);

/* The shape of a rounded rect with elliptical radii rx[i], ry[i] (top-left, top-right, bottom-right, bottom-left; already
 * scaled to fit): an RRECT when every corner is circular, else a PATH appended to cv. */
void embi_lay_cp_rrect(embi_lay_cp *out, embi_lay_curves *cv, const float rect[4], const float rx[4], const float ry[4]);

/* The outline of an SVG path data string (CSS px, relative to the origin ox, oy) appended to cv. False for data that does not
 * parse. */
bool embi_lay_svg_path(embi_str d, float ox, float oy, embi_lay_curves *cv);

/* Whether the point (x, y) is inside the shape; curves is the vector the shape's curves were appended to. */
bool embi_lay_cp_contains(const embi_lay_cp *cp, const embi_lay_curve *curves, float x, float y);
