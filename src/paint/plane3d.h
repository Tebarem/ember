#pragma once
/*
 * Planes of 3D transforms and 3D rendering contexts (R3c, CSS Transforms 2 "3D rendering contexts").
 *
 * A box whose transform is not an affine map of the plane (a perspective, a rotation about x or y, a matrix3d), a box
 * that preserves 3D, and every stacking context inside a preserve-3d box paint as a plane: a LAYER_BEGIN with sub
 * EMBI_LS_PLANE, the box's items in its own space, and a LAYER_END (dl.h). The plane's homography into the space it is
 * composed into is in dl->planes.
 *
 * A 3D rendering context is the plane of a preserve-3d box together with the planes of the stacking contexts that are
 * its children, and, recursively, the children of those that preserve 3D. The planes of one context are painted in
 * the order of the tree, one after the other (a child's rendering is deferred until its parent plane is complete), and
 * then ordered: each plane is a polygon in the space the context is composed into, and a BSP tree orders them back to
 * front and splits a plane that another one cuts. A piece of a split plane repeats the plane's items and composes
 * under a clip that is the half plane (a rotated rectangle) of the cut.
 */
#include "paint/paint_int.h"

/* True when f is a child, in the layout tree, of the plane that is being painted in a 3D rendering context and that
 * plane preserves 3D: f is then a plane of the same context. */
bool embi_p3_participant(const embi_pst *p, const embi_frag *f);
/* Defers the participant f (its border box at kx, ky) until its parent plane is complete. rel is its matrix relative to
 * the space of the parent plane: the parent's perspective, when it has one, times the box's own matrix. */
void embi_p3_defer(embi_pst *p, const embi_frag *f, float kx, float ky, const embi_m4 *rel, uint32_t parent_ctx);
/* Paints the box f (at kx, ky) as a plane of a context of its own: a flat plane when it does not preserve 3D, the root
 * of a 3D rendering context when it does. rel is the matrix of the box relative to the space it is composed into
 * (the parent's perspective times the box's own matrix), xf_parent and clip_parent the affine transform and the clip of
 * that space. */
void embi_p3_paint(embi_pst *p, const embi_frag *f, float kx, float ky, const embi_m4 *rel, uint32_t xf_parent,
                   uint32_t clip_parent, uint32_t parent_ctx);
