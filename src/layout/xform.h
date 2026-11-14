#pragma once
/*
 * Box transforms as 4x4 matrices (R3c, CSS Transforms 1 and 2): the transform of a box from its transform,
 * translate, rotate, scale and transform-origin properties, the perspective that a box gives its children, and the
 * small matrix helpers that paint (display lists, plane sorting) and hit testing share. Positions are in the one
 * page frame of the layout, so a matrix already includes the move of the transform origin: M = T(o) * list * T(-o).
 *
 * The matrices are row-major (m[row * 4 + col]) and act on column vectors (x, y, z, 1). The plane z = 0 of a box
 * (where its content lies) maps through the 3x3 homography of rows and columns 0, 1 and 3.
 */
#include "layout/box.h"
#include "layout/fragment.h"
#include "style/style.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_m4 {
    double m[16];
} embi_m4;

void embi_m4_identity(embi_m4 *a);
/* out = a * b (b applies first to a point). out may alias a or b. */
void embi_m4_mul(const embi_m4 *a, const embi_m4 *b, embi_m4 *out);
/* The image (x, y, z, w) of the point (x, y, z, 1). */
void embi_m4_point(const embi_m4 *a, double x, double y, double z, double out[4]);
/* Whether the matrix can be inverted (a transform that is singular, such as scale3d(2, 2, 0), draws nothing). */
bool embi_m4_invertible(const embi_m4 *a);
/* The homography of the plane z = 0: h[0..8] row-major, mapping (x, y, 1) to (X, Y, W). */
void embi_m4_plane(const embi_m4 *a, double h[9]);
/* The inverse of a 3x3. False when singular. out may alias h. */
bool embi_h3_invert(const double h[9], double out[9]);
/* out = a * b for 3x3 matrices (row-major). out may alias. */
void embi_h3_mul(const double a[9], const double b[9], double out[9]);
double embi_h3_det(const double h[9]);

/* Whether the style has a transform, translate, rotate or scale other than none. */
bool embi_lay_has_transform(const embi_style *s);
/* The matrix of a box with the border box bb (x, y, w, h in the page frame): translate, rotate, scale, then
 * transform, about transform-origin (with its z). vw and vh are the viewport for vw/vh units and rem the root font size (rem units; 16 when not above 0). Returns false (out is
 * the identity) when the style has no transform. */
bool embi_lay_box_matrix(const embi_style *s, const float bb[4], float vw, float vh, float rem, embi_m4 *out);
/* The matrix that the perspective of a box gives to its children: T(po) * P * T(-po) with the box's perspective-origin
 * (perspective: none gives false and the identity). */
bool embi_lay_perspective_matrix(const embi_style *s, const float bb[4], float vw, float vh, float rem, embi_m4 *out);
/* A length, percentage or calc() value in px against the 100% base ref (R3b: clip-path shapes). ok is false for other kinds.
 * fs is the element's font size, vw and vh the viewport and rem the root font size (16 when not above 0). */
double embi_lay_len_px(const embi_value *v, double ref, float fs, float vw, float vh, float rem, bool *ok);
/* The font size in px of the root element (the base of rem units) of the fragment tree whose ICB is root. */
float embi_lay_root_font_px(const embi_frag *root);
/* transform-style: preserve-3d is the computed value. */
bool embi_lay_preserve3d_style(const embi_style *s);
/* The used value of transform-style for a box: preserve-3d unless a grouping property (overflow other than visible or
 * clip, opacity below 1, filter, clip-path, mask, isolation, mix-blend-mode, contain: paint) forces flat. */
bool embi_lay_preserve3d_used(const embi_style *s);
/* The element whose box is the parent of the box b for 3D purposes: the DOM parent of an element box (a block inside
 * an inline belongs to the inline, not to its containing block), the owner element of a generated box (::before and
 * the like). nullptr for an anonymous box and for the root. */
const struct embi_element *embi_lay_parent_el(const embi_box *b);
/* backface-visibility: hidden. */
bool embi_lay_backface_hidden(const embi_style *s);
