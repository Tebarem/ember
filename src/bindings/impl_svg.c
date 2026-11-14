/*
 * The SVG DOM (R4d): the interface of each SVG element (createElementNS, the parser and innerHTML use the SVG tag table of
 * idlgen), the members of SVGElement, SVGGraphicsElement, SVGGeometryElement and SVGSVGElement, the getters of the
 * animated values and lists (the wrappers are in impl_svg_list.c, the value types in impl_svg_val.c), and the geometry
 * methods, which go to the hook in svg_geometry.h (R4c installs it).
 */
#include "bindings/svg_int.h"
#include "bindings/svg_geometry.h"
#include "style/style.h"
#include "bindings/gen/html.h"
#include "bindings/gen/svg.h"
#include "bindings/gen/svgelement.h"

#include "num/num.h"

#include <stdint.h>

/* ------------------------------------------------------------------ the geometry hook */

void embi_svg_geometry_install(embi_bind_ctx *c, const embi_svg_geometry_ops *ops, void *user)
{
    c->svg_geom = ops;
    c->svg_geom_user = user;
}

const embi_svg_geometry_ops *embi_svg_geometry_current(const embi_bind_ctx *c)
{
    return c->svg_geom;
}

/* ------------------------------------------------------------------ helpers */

static embi_str cstr(const char *s)
{
    return (embi_str){s, (uint32_t)SDL_strlen(s)};
}

static bool is_svg_el(const embi_node *n, const char *tag)
{
    return n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_SVG && embi_str_eq(embi_atom_str(n->name), cstr(tag));
}

/* The nearest svg (or symbol) ancestor of an element, or nullptr. */
static embi_node *ancestor_svg(embi_node *n, bool symbol_too)
{
    for (embi_node *p = n->parent; p != nullptr; p = p->parent) {
        if (p->type != EMBI_NODE_ELEMENT || p->extra != EMBI_NS_SVG) {
            return nullptr; /* an HTML element (inside a foreignObject) ends the SVG tree */
        }
        if (is_svg_el(p, "svg") || (symbol_too && is_svg_el(p, "symbol"))) {
            return p;
        }
        if (is_svg_el(p, "foreignObject")) {
            return nullptr; /* its children start a new (HTML) context */
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ SVGElement */

embi_js_value embi_impl_SVGElement_get_className(embi_bind_ctx *c, embi_node *self)
{
    return embi_svg_anim_view(c, self, EMBI_IF_SVGAnimatedString, SA_class);
}

embi_node *embi_impl_SVGElement_get_ownerSVGElement(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ancestor_svg(self, false);
}

embi_node *embi_impl_SVGElement_get_viewportElement(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ancestor_svg(self, true);
}

/* ------------------------------------------------------------------ SVGGraphicsElement and SVGGeometryElement */

embi_js_value embi_impl_SVGGraphicsElement_getBBox(embi_bind_ctx *c, embi_node *self, embi_dict_SVGBoundingBoxOptions a0)
{
    (void)a0; /* fill, stroke, markers and clipped are R4c's business */
    double b[4] = {0, 0, 0, 0};
    const embi_svg_geometry_ops *g = c->svg_geom;
    if (!(g && g->bbox && g->bbox(c->svg_geom_user, self, b))) {
        b[0] = b[1] = b[2] = b[3] = 0;
    }
    return embi_impl_rect_new(c, b[0], b[1], b[2], b[3]);
}

/* W6e: the client rectangle of an SVG graphics element (it has no CSS box): the bounding box of getBBox mapped through the
 * screen CTM. r is x0, y0, x1, y1. False for other nodes and when there is no geometry. */
/* Whether n takes part in rendering: no ancestor (or n) is a container whose children are only resources (mask, clipPath,
 * marker, pattern, gradients, symbol, defs, filter ...) and none has display: none. */
static bool svg_rendered(embi_bind_ctx *c, embi_node *n)
{
    static const char *const res[] = {"mask", "clipPath", "marker", "pattern", "linearGradient", "radialGradient", "symbol",
                                      "defs", "filter", "title", "desc", "metadata", "style", "script"};
    embi_style_engine *se = embi_bind_style(c);
    for (embi_node *p = n; p != nullptr && p->type == EMBI_NODE_ELEMENT && p->extra == EMBI_NS_SVG; p = p->parent) {
        for (size_t i = 0; i < sizeof res / sizeof res[0]; i++) {
            if (p != n && is_svg_el(p, res[i])) {
                return false;
            }
        }
        if (se != nullptr) {
            embi_style_resolve_document(se, p->doc);
            const embi_style *st = embi_style_get(se, (embi_element *)p, EMBI_PE_NONE);
            embi_sb sb = {0};
            bool none = st != nullptr && embi_style_value_text(st, EMBI_PROP_display, &sb) && sb.len == 4 &&
                        SDL_memcmp(sb.data, "none", 4) == 0;
            embi_sb_free(&sb);
            if (none) {
                return false;
            }
        }
    }
    return true;
}

bool embi_impl_svg_client_bbox(embi_bind_ctx *c, embi_node *n, double r[4])
{
    const embi_svg_geometry_ops *g = c->svg_geom;
    double b[4];
    double m[6] = {1, 0, 0, 1, 0, 0};
    if (n->extra != EMBI_NS_SVG || !g || !g->bbox || !(n->flags & EMBI_NF_CONNECTED) || !svg_rendered(c, n) || !g->bbox(c->svg_geom_user, n, b)) {
        return false;
    }
    if (g->ctm && !g->ctm(c->svg_geom_user, n, true, m)) {
        return false;
    }
    double xs[4] = {b[0], b[0] + b[2], b[0] + b[2], b[0]};
    double ys[4] = {b[1], b[1], b[1] + b[3], b[1] + b[3]};
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (int i = 0; i < 4; i++) {
        double x = m[0] * xs[i] + m[2] * ys[i] + m[4];
        double y = m[1] * xs[i] + m[3] * ys[i] + m[5];
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
    }
    r[0] = x0;
    r[1] = y0;
    r[2] = x1;
    r[3] = y1;
    return true;
}

static embi_js_value ctm_value(embi_bind_ctx *c, embi_node *self, bool screen)
{
    double m[6] = {1, 0, 0, 1, 0, 0};
    const embi_svg_geometry_ops *g = c->svg_geom;
    if (!(g && g->ctm && g->ctm(c->svg_geom_user, self, screen, m))) {
        const double id[6] = {1, 0, 0, 1, 0, 0};
        SDL_memcpy(m, id, sizeof m);
    }
    return embi_svg_dommatrix_new(c, m);
}

embi_js_value embi_impl_SVGGraphicsElement_getCTM(embi_bind_ctx *c, embi_node *self)
{
    return ctm_value(c, self, false);
}

embi_js_value embi_impl_SVGGraphicsElement_getScreenCTM(embi_bind_ctx *c, embi_node *self)
{
    return ctm_value(c, self, true);
}

double embi_impl_SVGGeometryElement_getTotalLength(embi_bind_ctx *c, embi_node *self)
{
    double len = 0;
    const embi_svg_geometry_ops *g = c->svg_geom;
    if (!(g && g->total_length && g->total_length(c->svg_geom_user, self, &len))) {
        return 0;
    }
    return len;
}

embi_js_value embi_impl_SVGGeometryElement_getPointAtLength(embi_bind_ctx *c, embi_node *self, double a0)
{
    double p[2] = {0, 0};
    const embi_svg_geometry_ops *g = c->svg_geom;
    if (!(g && g->point_at_length && g->point_at_length(c->svg_geom_user, self, a0, p))) {
        p[0] = p[1] = 0;
    }
    return embi_svg_dompoint_new(c, p[0], p[1], 0, 1);
}

bool embi_impl_SVGGeometryElement_isPointInFill(embi_bind_ctx *c, embi_node *self, embi_dict_DOMPointInit a0)
{
    const embi_svg_geometry_ops *g = c->svg_geom;
    return g && g->point_in_fill && g->point_in_fill(c->svg_geom_user, self, a0.x, a0.y);
}

bool embi_impl_SVGGeometryElement_isPointInStroke(embi_bind_ctx *c, embi_node *self, embi_dict_DOMPointInit a0)
{
    const embi_svg_geometry_ops *g = c->svg_geom;
    return g && g->point_in_stroke && g->point_in_stroke(c->svg_geom_user, self, a0.x, a0.y);
}

/* ------------------------------------------------------------------ SVGSVGElement */

/* getElementById: the first element with the id in the subtree of the svg element, in tree order. */
embi_node *embi_impl_SVGSVGElement_getElementById(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_str want = embi_bind_utf8(c, a0);
    if (want.len == 0) {
        return nullptr;
    }
    embi_node *n = self->first_child;
    while (n != nullptr) {
        if (n->type == EMBI_NODE_ELEMENT) {
            const embi_attr *a = embi_el_get_attribute((embi_element *)n, EMBI_STR("id"));
            if (a != nullptr && embi_str_eq(embi_bind_utf8(c, a->value), want)) {
                return n;
            }
        }
        if (n->first_child != nullptr) {
            n = n->first_child;
            continue;
        }
        while (n != nullptr && n != self && n->next == nullptr) {
            n = n->parent;
        }
        n = (n == nullptr || n == self) ? nullptr : n->next;
    }
    return nullptr;
}

/* currentScale lives in the aux of the element record (the bits of the float, xor the bits of 1.0f). The record of an element
 * lives as long as the element can be reached (the wrappers of connected nodes are roots, a detached tree is freed with its
 * last wrapper), so the value is never lost while it can be observed. */
double embi_impl_SVGSVGElement_get_currentScale(embi_bind_ctx *c, embi_node *self)
{
    uint32_t bits = embi_impl_node_rec(c, self)->aux ^ 0x3f800000u;
    float f;
    SDL_memcpy(&f, &bits, sizeof f);
    return f;
}

void embi_impl_SVGSVGElement_set_currentScale(embi_bind_ctx *c, embi_node *self, double a0)
{
    float f = (float)a0;
    uint32_t bits;
    SDL_memcpy(&bits, &f, sizeof bits);
    embi_impl_node_rec(c, self)->aux = bits ^ 0x3f800000u;
}

/* currentTranslate is [SameObject]: the generated getter keeps the first value in the record of the element, so writes
 * to the point stay (they only change the value: the panning itself is the render tree's business). */
embi_js_value embi_impl_SVGSVGElement_get_currentTranslate(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_svgpoint_new(c, 0, 0);
}

embi_dom_err embi_impl_SVGSVGElement_get_zoomAndPan(embi_bind_ctx *c, embi_node *self, uint16_t *out)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)self, EMBI_STR("zoomAndPan"));
    *out = (a != nullptr && embi_str_eq(embi_bind_utf8(c, a->value), EMBI_STR("disable"))) ? 1 : 2;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGSVGElement_set_zoomAndPan(embi_bind_ctx *c, embi_node *self, uint16_t a0)
{
    if (a0 != 1 && a0 != 2) {
        (void)embi_js_throw_type_error(embi_bind_runtime(c), "The zoomAndPan value is not valid.");
        return EMBI_BIND_THREW;
    }
    (void)embi_el_set_attribute((embi_element *)self, EMBI_STR("zoomAndPan"),
                                embi_bind_tmp_utf8(c, a0 == 1 ? EMBI_STR("disable") : EMBI_STR("magnify")));
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_SVGSVGElement_createSVGNumber(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGNumber, SV_NUM);
}

embi_js_value embi_impl_SVGSVGElement_createSVGLength(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGLength, SV_LEN);
}

embi_js_value embi_impl_SVGSVGElement_createSVGAngle(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGAngle, SV_ANGLE);
}

embi_js_value embi_impl_SVGSVGElement_createSVGRect(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGRect, SV_RECT);
}

embi_js_value embi_impl_SVGSVGElement_createSVGMatrix(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGMatrix, SV_MX);
}

embi_js_value embi_impl_SVGSVGElement_createSVGPoint(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_svg_new_value(c, EMBI_IF_SVGPoint, SV_PT);
}

embi_js_value embi_impl_SVGSVGElement_createSVGTransform(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    embi_js_value v;
    sv *t = embi_sv_new(c, EMBI_IF_SVGTransform, SV_TF, &v);
    t->u.tf.type = SVG_TF_MATRIX; /* an identity matrix transform */
    return v;
}

embi_js_value embi_impl_SVGSVGElement_createSVGTransformFromMatrix(embi_bind_ctx *c, embi_node *self,
                                                                    embi_dict_DOMMatrix2DInit a0)
{
    (void)self;
    return embi_svg_new_transform_from_init(c, &a0);
}

/* ------------------------------------------------------------------ SVGMarkerElement */

void embi_impl_SVGMarkerElement_setOrientToAuto(embi_bind_ctx *c, embi_node *self)
{
    embi_sv_set_attr(c, self, SA_orient, EMBI_STR("auto"));
}

embi_dom_err embi_impl_SVGMarkerElement_setOrientToAngle(embi_bind_ctx *c, embi_node *self, void *a0)
{
    sv *ang = a0;
    embi_sv_load(c, ang);
    embi_sb sb = {0};
    char buf[64];
    size_t k = embi_num_to_str(ang->u.num, buf);
    embi_sb_append(&sb, (embi_str){buf, (uint32_t)k});
    embi_sb_append(&sb, cstr(ang->unit == SVG_AT_DEG    ? "deg"
                             : ang->unit == SVG_AT_RAD  ? "rad"
                             : ang->unit == SVG_AT_GRAD ? "grad"
                             : ang->unit == SVG_AT_TURN ? "turn"
                                                        : ""));
    embi_sv_set_attr(c, self, SA_orient, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ the getters of the animated values and lists */

#define ANIM(OWNER, NAME, IFACE, IDX)                                                                  \
    embi_js_value embi_impl_##OWNER##_get_##NAME(embi_bind_ctx *c, embi_node *self)                    \
    {                                                                                                  \
        return embi_svg_anim_view(c, self, EMBI_IF_##IFACE, SA_##IDX);                                 \
    }

#define LISTV(OWNER, NAME, IFACE, IDX, RO)                                                                 \
    embi_js_value embi_impl_##OWNER##_get_##NAME(embi_bind_ctx *c, embi_node *self)                    \
    {                                                                                                  \
        return embi_svg_list_view(c, self, EMBI_IF_##IFACE, SA_##IDX, RO);                             \
    }

ANIM(SVGGraphicsElement, transform, SVGAnimatedTransformList, transform)
LISTV(SVGGraphicsElement, requiredExtensions, SVGStringList, requiredExtensions, false)
LISTV(SVGGraphicsElement, systemLanguage, SVGStringList, systemLanguage, false)
ANIM(SVGGeometryElement, pathLength, SVGAnimatedNumber, pathLength)
ANIM(SVGSVGElement, x, SVGAnimatedLength, x)
ANIM(SVGSVGElement, y, SVGAnimatedLength, y)
ANIM(SVGSVGElement, width, SVGAnimatedLength, width)
ANIM(SVGSVGElement, height, SVGAnimatedLength, height)
ANIM(SVGSVGElement, viewBox, SVGAnimatedRect, viewBox)
ANIM(SVGSVGElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
ANIM(SVGSymbolElement, viewBox, SVGAnimatedRect, viewBox)
ANIM(SVGSymbolElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
ANIM(SVGViewElement, viewBox, SVGAnimatedRect, viewBox)
ANIM(SVGViewElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
ANIM(SVGForeignObjectElement, x, SVGAnimatedLength, x)
ANIM(SVGForeignObjectElement, y, SVGAnimatedLength, y)
ANIM(SVGForeignObjectElement, width, SVGAnimatedLength, width)
ANIM(SVGForeignObjectElement, height, SVGAnimatedLength, height)
ANIM(SVGClipPathElement, clipPathUnits, SVGAnimatedEnumeration, clipPathUnits)
ANIM(SVGClipPathElement, transform, SVGAnimatedTransformList, transform)
ANIM(SVGAElement, href, SVGAnimatedString, href)
ANIM(SVGAElement, target, SVGAnimatedString, target)
ANIM(SVGUseElement, x, SVGAnimatedLength, x)
ANIM(SVGUseElement, y, SVGAnimatedLength, y)
ANIM(SVGUseElement, width, SVGAnimatedLength, width)
ANIM(SVGUseElement, height, SVGAnimatedLength, height)
ANIM(SVGUseElement, href, SVGAnimatedString, href)
ANIM(SVGImageElement, x, SVGAnimatedLength, x)
ANIM(SVGImageElement, y, SVGAnimatedLength, y)
ANIM(SVGImageElement, width, SVGAnimatedLength, width)
ANIM(SVGImageElement, height, SVGAnimatedLength, height)
ANIM(SVGImageElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
ANIM(SVGImageElement, href, SVGAnimatedString, href)
ANIM(SVGTextContentElement, textLength, SVGAnimatedLength, textLength)
ANIM(SVGTextContentElement, lengthAdjust, SVGAnimatedEnumeration, lengthAdjust)
ANIM(SVGTextPositioningElement, x, SVGAnimatedLengthList, x)
ANIM(SVGTextPositioningElement, y, SVGAnimatedLengthList, y)
ANIM(SVGTextPositioningElement, dx, SVGAnimatedLengthList, dx)
ANIM(SVGTextPositioningElement, dy, SVGAnimatedLengthList, dy)
ANIM(SVGTextPositioningElement, rotate, SVGAnimatedNumberList, rotate)
ANIM(SVGTextPathElement, href, SVGAnimatedString, href)
ANIM(SVGTextPathElement, startOffset, SVGAnimatedLength, startOffset)
ANIM(SVGTextPathElement, method, SVGAnimatedEnumeration, method)
ANIM(SVGTextPathElement, spacing, SVGAnimatedEnumeration, spacing)
ANIM(SVGTextPathElement, side, SVGAnimatedEnumeration, side)
ANIM(SVGRectElement, x, SVGAnimatedLength, x)
ANIM(SVGRectElement, y, SVGAnimatedLength, y)
ANIM(SVGRectElement, width, SVGAnimatedLength, width)
ANIM(SVGRectElement, height, SVGAnimatedLength, height)
ANIM(SVGRectElement, rx, SVGAnimatedLength, rx)
ANIM(SVGRectElement, ry, SVGAnimatedLength, ry)
ANIM(SVGCircleElement, cx, SVGAnimatedLength, cx)
ANIM(SVGCircleElement, cy, SVGAnimatedLength, cy)
ANIM(SVGCircleElement, r, SVGAnimatedLength, r)
ANIM(SVGEllipseElement, cx, SVGAnimatedLength, cx)
ANIM(SVGEllipseElement, cy, SVGAnimatedLength, cy)
ANIM(SVGEllipseElement, rx, SVGAnimatedLength, rx)
ANIM(SVGEllipseElement, ry, SVGAnimatedLength, ry)
ANIM(SVGLineElement, x1, SVGAnimatedLength, x1)
ANIM(SVGLineElement, y1, SVGAnimatedLength, y1)
ANIM(SVGLineElement, x2, SVGAnimatedLength, x2)
ANIM(SVGLineElement, y2, SVGAnimatedLength, y2)
LISTV(SVGPolylineElement, points, SVGPointList, points, false)
LISTV(SVGPolylineElement, animatedPoints, SVGPointList, animatedPoints, true)
LISTV(SVGPolygonElement, points, SVGPointList, points, false)
LISTV(SVGPolygonElement, animatedPoints, SVGPointList, animatedPoints, true)
ANIM(SVGGradientElement, gradientUnits, SVGAnimatedEnumeration, gradientUnits)
ANIM(SVGGradientElement, gradientTransform, SVGAnimatedTransformList, gradientTransform)
ANIM(SVGGradientElement, spreadMethod, SVGAnimatedEnumeration, spreadMethod)
ANIM(SVGGradientElement, href, SVGAnimatedString, href)
ANIM(SVGLinearGradientElement, x1, SVGAnimatedLength, x1)
ANIM(SVGLinearGradientElement, y1, SVGAnimatedLength, y1)
ANIM(SVGLinearGradientElement, x2, SVGAnimatedLength, x2)
ANIM(SVGLinearGradientElement, y2, SVGAnimatedLength, y2)
ANIM(SVGRadialGradientElement, cx, SVGAnimatedLength, cx)
ANIM(SVGRadialGradientElement, cy, SVGAnimatedLength, cy)
ANIM(SVGRadialGradientElement, r, SVGAnimatedLength, r)
ANIM(SVGRadialGradientElement, fx, SVGAnimatedLength, fx)
ANIM(SVGRadialGradientElement, fy, SVGAnimatedLength, fy)
ANIM(SVGRadialGradientElement, fr, SVGAnimatedLength, fr)
ANIM(SVGStopElement, offset, SVGAnimatedNumber, offset)
ANIM(SVGPatternElement, patternUnits, SVGAnimatedEnumeration, patternUnits)
ANIM(SVGPatternElement, patternContentUnits, SVGAnimatedEnumeration, patternContentUnits)
ANIM(SVGPatternElement, patternTransform, SVGAnimatedTransformList, patternTransform)
ANIM(SVGPatternElement, x, SVGAnimatedLength, x)
ANIM(SVGPatternElement, y, SVGAnimatedLength, y)
ANIM(SVGPatternElement, width, SVGAnimatedLength, width)
ANIM(SVGPatternElement, height, SVGAnimatedLength, height)
ANIM(SVGPatternElement, viewBox, SVGAnimatedRect, viewBox)
ANIM(SVGPatternElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
ANIM(SVGPatternElement, href, SVGAnimatedString, href)
ANIM(SVGMaskElement, maskUnits, SVGAnimatedEnumeration, maskUnits)
ANIM(SVGMaskElement, maskContentUnits, SVGAnimatedEnumeration, maskContentUnits)
ANIM(SVGMaskElement, x, SVGAnimatedLength, x)
ANIM(SVGMaskElement, y, SVGAnimatedLength, y)
ANIM(SVGMaskElement, width, SVGAnimatedLength, width)
ANIM(SVGMaskElement, height, SVGAnimatedLength, height)
ANIM(SVGMarkerElement, refX, SVGAnimatedLength, refX)
ANIM(SVGMarkerElement, refY, SVGAnimatedLength, refY)
ANIM(SVGMarkerElement, markerUnits, SVGAnimatedEnumeration, markerUnits)
ANIM(SVGMarkerElement, markerWidth, SVGAnimatedLength, markerWidth)
ANIM(SVGMarkerElement, markerHeight, SVGAnimatedLength, markerHeight)
ANIM(SVGMarkerElement, orientType, SVGAnimatedEnumeration, orientType)
ANIM(SVGMarkerElement, orientAngle, SVGAnimatedAngle, orient)
ANIM(SVGMarkerElement, viewBox, SVGAnimatedRect, viewBox)
ANIM(SVGMarkerElement, preserveAspectRatio, SVGAnimatedPreserveAspectRatio, preserveAspectRatio)
