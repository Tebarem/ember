/*
 * Path2D (canvas.webidl, R4b) and the rounded-rect radii of roundRect. A Path2D wrapper's ptr is the embi_cpath (user
 * space, no transform applied); the wrapper owns one reference through a small box.
 */
#include "bindings/impl_canvas.h"

#include <math.h>
#include <string.h>

typedef struct pbox {
    embi_cpath *p;
} pbox;

static void pbox_release(void *owned)
{
    embi_cpath_release(((pbox *)owned)->p);
}

static embi_js_value path_wrap(embi_bind_ctx *c, embi_cpath *p)
{
    pbox *b = embi_alloc(sizeof *b);
    b->p = p;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_Path2D, p, b, sizeof *b);
    embi_bind_obj_of(v)->release = pbox_release;
    return v;
}

embi_js_value embi_impl_Path2D_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_cpath *p = embi_cpath_new();
    embi_bind_obj *o = embi_js_is_object(a0) ? embi_bind_obj_of(a0) : nullptr;
    if (o != nullptr && o->iface == EMBI_IF_Path2D) {
        embi_cpath_add(p, o->ptr, nullptr);
    } else if (!embi_js_is_undefined(a0)) {
        embi_dstr d;
        if (!embi_bind_conv_str(c, a0, "Failed to construct 'Path2D'", false, &d)) {
            embi_cpath_release(p);
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        embi_cpath_parse_svg(p, embi_bind_utf8(c, d));
    }
    embi_js_value v = path_wrap(c, p);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_dom_err embi_impl_Path2D_addPath(embi_bind_ctx *c, embi_bind_obj *self, void *a0, embi_dict_DOMMatrix2DInit a1)
{
    double m16[16];
    if (!embi_svg_matrix_from_init(c, &a1, m16)) {
        return EMBI_BIND_THREW;
    }
    const double m[6] = {m16[0], m16[1], m16[4], m16[5], m16[12], m16[13]};
    if (!isfinite(m[0]) || !isfinite(m[1]) || !isfinite(m[2]) || !isfinite(m[3]) || !isfinite(m[4]) || !isfinite(m[5])) {
        return EMBI_DOM_OK;
    }
    if (a0 == self->ptr) {
        embi_cpath *copy = embi_cpath_clone(a0);
        embi_cpath_add(self->ptr, copy, m);
        embi_cpath_release(copy);
    } else {
        embi_cpath_add(self->ptr, a0, m);
    }
    return EMBI_DOM_OK;
}

void embi_impl_Path2D_closePath(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_cpath_close(self->ptr);
}

void embi_impl_Path2D_moveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    embi_cpath_move_to(self->ptr, nullptr, a0, a1);
}

void embi_impl_Path2D_lineTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    embi_cpath_line_to(self->ptr, nullptr, a0, a1);
}

void embi_impl_Path2D_quadraticCurveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3)
{
    (void)c;
    embi_cpath_quad_to(self->ptr, nullptr, a0, a1, a2, a3);
}

void embi_impl_Path2D_bezierCurveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3,
                                    double a4, double a5)
{
    (void)c;
    embi_cpath_cubic_to(self->ptr, nullptr, a0, a1, a2, a3, a4, a5);
}

embi_dom_err embi_impl_Path2D_arcTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3,
                                    double a4)
{
    return cv_throw(c, embi_cpath_arc_to(self->ptr, nullptr, a0, a1, a2, a3, a4), "The radius provided is negative.");
}

void embi_impl_Path2D_rect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3)
{
    (void)c;
    embi_cpath_rect(self->ptr, nullptr, a0, a1, a2, a3);
}

embi_dom_err embi_impl_Path2D_roundRect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3,
                                        embi_js_value a4)
{
    return cv_round_rect(c, self->ptr, nullptr, a0, a1, a2, a3, a4);
}

embi_dom_err embi_impl_Path2D_arc(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3,
                                  double a4, bool a5)
{
    return cv_throw(c, embi_cpath_arc(self->ptr, nullptr, a0, a1, a2, a3, a4, a5), "The radius provided is negative.");
}

embi_dom_err embi_impl_Path2D_ellipse(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2, double a3,
                                      double a4, double a5, double a6, bool a7)
{
    return cv_throw(c, embi_cpath_ellipse(self->ptr, nullptr, a0, a1, a2, a3, a4, a5, a6, a7),
                    "The radius provided is negative.");
}

/* ------------------------------------------------------------------ roundRect radii */

/* One corner radius from a number or a DOMPointInit. 1: read; 0: a non-finite value (the call does nothing); -1: an exception
 * is pending. */
static int corner(embi_bind_ctx *c, embi_js_value v, double *rx, double *ry)
{
    if (embi_js_is_object(v)) {
        embi_js_value xv, yv;
        if (!embi_bind_dict_get(c, v, "x", &xv) || !embi_bind_dict_get(c, v, "y", &yv)) {
            return -1;
        }
        double x = 0.0, y = 0.0;
        if (!embi_js_is_undefined(xv) && !embi_bind_conv_double(c, xv, true, "roundRect", &x)) {
            return -1;
        }
        if (!embi_js_is_undefined(yv) && !embi_bind_conv_double(c, yv, true, "roundRect", &y)) {
            return -1;
        }
        *rx = x;
        *ry = y;
    } else {
        double r;
        if (!embi_bind_conv_double(c, v, true, "roundRect", &r)) {
            return -1;
        }
        *rx = *ry = r;
    }
    return isfinite(*rx) && isfinite(*ry) ? 1 : 0;
}

embi_dom_err cv_round_rect(embi_bind_ctx *c, embi_cpath *p, const double *m, double x, double y, double w, double h,
                           embi_js_value radii)
{
    double list[4][2];
    uint32_t n = 0;
    bool finite = true;
    if (embi_js_is_object(radii) && embi_js_is_array(radii)) {
        embi_js_value *items;
        if (!embi_bind_seq_open(c, radii, "roundRect", &items, &n)) {
            return EMBI_BIND_THREW;
        }
        if (n < 1 || n > 4) {
            (void)embi_js_throw_range_error(c->rt, "%u radii provided, but only 1 to 4 are allowed.", n);
            return EMBI_BIND_THREW;
        }
        for (uint32_t i = 0; i < n; i++) {
            int r = corner(c, items[i], &list[i][0], &list[i][1]);
            if (r < 0) {
                return EMBI_BIND_THREW;
            }
            finite = finite && r == 1;
        }
    } else if (embi_js_is_undefined(radii)) {
        n = 1;
        list[0][0] = list[0][1] = 0.0;
    } else {
        n = 1;
        int r = corner(c, radii, &list[0][0], &list[0][1]);
        if (r < 0) {
            return EMBI_BIND_THREW;
        }
        finite = r == 1;
    }
    if (!finite || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return EMBI_DOM_OK;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (list[i][0] < 0.0 || list[i][1] < 0.0) {
            (void)embi_js_throw_range_error(c->rt, "Radius value %g is negative.", list[i][0] < 0.0 ? list[i][0] : list[i][1]);
            return EMBI_BIND_THREW;
        }
    }
    /* TL, TR, BR, BL from the CSS-like shorthand. */
    uint32_t idx[4];
    switch (n) {
    case 1: idx[0] = idx[1] = idx[2] = idx[3] = 0; break;
    case 2: idx[0] = 0; idx[1] = 1; idx[2] = 0; idx[3] = 1; break;
    case 3: idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 1; break;
    default: idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3; break;
    }
    double r8[8];
    for (int i = 0; i < 4; i++) {
        r8[2 * i] = list[idx[i]][0];
        r8[2 * i + 1] = list[idx[i]][1];
    }
    embi_cpath_round_rect(p, m, x, y, w, h, r8);
    return EMBI_DOM_OK;
}
