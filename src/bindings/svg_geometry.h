#pragma once
/*
 * The SVG geometry hook (R4d). The geometry methods of the SVG DOM (getBBox, getCTM, getScreenCTM, getTotalLength,
 * getPointAtLength, isPointInFill, isPointInStroke) are answered from the render tree, which R4c builds. The bindings
 * call the table installed on their context; with no table installed (or a callback that returns false, meaning "not
 * rendered") they return the SVG 2 defaults: an empty rectangle, the identity matrix, 0, (0, 0) and false.
 *
 * The table belongs to one bind context (one view or runtime), so several views or runtimes in a process each have their
 * own: R4c calls embi_svg_geometry_install(ctx, &ops, view) once for each context it serves, and nullptr to remove it.
 * Every callback gets the `user` pointer given at install time first. Node pointers are the element nodes (type
 * EMBI_NODE_ELEMENT, extra EMBI_NS_SVG); the table must not keep them past the call.
 */
#include "bindings/bind.h"
#include "dom/dom.h"

#include <stdbool.h>

typedef struct embi_svg_geometry_ops {
    /* The bounding box in user space: out[0..3] = x, y, width, height. */
    bool (*bbox)(void *user, const embi_node *el, double out[4]);
    /* The current transformation matrix (screen == false) or the screen CTM, as a, b, c, d, e, f. */
    bool (*ctm)(void *user, const embi_node *el, bool screen, double out[6]);
    /* The length of a geometry element's path in user units. */
    bool (*total_length)(void *user, const embi_node *el, double *out);
    /* The point at a distance along the path, in user space: out[0] = x, out[1] = y. */
    bool (*point_at_length)(void *user, const embi_node *el, double distance, double out[2]);
    /* Hit tests in user space: the fill and the stroke, per the element's pointer-events and fill rule. */
    bool (*point_in_fill)(void *user, const embi_node *el, double x, double y);
    bool (*point_in_stroke)(void *user, const embi_node *el, double x, double y);
} embi_svg_geometry_ops;

/* Installs the table on a context (nullptr removes it). The table must outlive the context or be removed first. */
void embi_svg_geometry_install(embi_bind_ctx *c, const embi_svg_geometry_ops *ops, void *user);
/* The table installed on a context, or nullptr. */
const embi_svg_geometry_ops *embi_svg_geometry_current(const embi_bind_ctx *c);
