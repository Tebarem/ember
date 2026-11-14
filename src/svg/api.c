/*
 * The public entry points of the SVG module (R4c): painting, hit testing and the geometry queries of the SVG DOM.
 */
#include "svg/svg_int.h"

#include "paint/dl.h"

/* The matrix to the page for the box at (x, y) under display list transform xform. */
static void base_matrix(const embi_dl *dl, uint32_t xform, float x, float y, float out[6])
{
    float t[6] = {1, 0, 0, 1, x, y};
    if (xform > 0 && xform < dl->xforms.len) {
        m_mul(dl->xforms.data[xform].m, t, out);
    } else {
        memcpy(out, t, sizeof t);
    }
}

void embi_svg_paint(const embi_svg_env *env, embi_dl *dl, const embi_element *svg, const float box[4], uint32_t xform,
                    uint32_t clip)
{
    if (dl == nullptr || svg == nullptr || !(box[2] > 0) || !(box[3] > 0)) {
        return;
    }
    svg_ctx c;
    svg_ctx_init(&c, env, SM_PAINT);
    c.dl = dl;
    c.clip0 = clip;
    c.doc = svg->base.doc;
    base_matrix(dl, xform, box[0], box[1], c.base);
    svg_walk_root(&c, (embi_element *)svg, box[2], box[3]);
    svg_ctx_free(&c);
}

const embi_element *embi_svg_hit(const embi_svg_env *env, const embi_element *svg, const float box[4], float x, float y)
{
    if (svg == nullptr) {
        return nullptr;
    }
    svg_ctx c;
    svg_ctx_init(&c, env, SM_HIT);
    c.hx = x;
    c.hy = y;
    svg_walk_root(&c, (embi_element *)svg, box[2], box[3]);
    const embi_element *r = c.hit;
    svg_ctx_free(&c);
    return r;
}

/* ------------------------------------------------------------------ geometry */

static void geom_ctx(svg_ctx *c, const embi_svg_geom *g)
{
    svg_ctx_init(c, g->env, SM_HIT);
    c->vpw = g->vw > 0 ? g->vw : 100.0f;
    c->vph = g->vh > 0 ? g->vh : 100.0f;
    c->tol = 0.05f;
}

bool embi_svg_bbox(const embi_svg_geom *g, const embi_element *el, double out[4])
{
    svg_ctx c;
    geom_ctx(&c, g);
    float b[4];
    bool ok = svg_el_bbox(&c, (embi_element *)el, (const embi_style *)el->style, b);
    svg_ctx_free(&c);
    if (ok) {
        for (int i = 0; i < 4; i++) {
            out[i] = b[i];
        }
    }
    return ok;
}

bool embi_svg_ctm(const embi_svg_geom *g, const embi_element *el, bool screen, double out[6])
{
    svg_ctx c;
    geom_ctx(&c, g);
    float m[6];
    m_identity(m);
    const embi_element *cur = el;
    bool first = true;
    while (cur != nullptr && svg_tag(cur) != TG_NONE) {
        const embi_element *par = cur->base.parent != nullptr && embi_is_element(cur->base.parent)
                                      ? (const embi_element *)cur->base.parent
                                      : nullptr;
        bool outer = par == nullptr || svg_tag(par) == TG_NONE;
        if (!outer) {
            float t[6];
            svg_el_matrix(&c, cur, (const embi_style *)cur->style, nullptr, t);
            m_mul(t, m, m);
            (void)first;
        }
        if (outer) {
            break;
        }
        if (svg_tag(par) == TG_SVG || svg_tag(par) == TG_SYMBOL) {
            float vb[4], vm[6];
            bool has;
            float w = svg_attr_px(&c, par, "width", c.vpw, c.vpw), h = svg_attr_px(&c, par, "height", c.vph, c.vph);
            svg_viewport_matrix(&c, par, w, h, vm, vb, &has);
            m_mul(vm, m, m);
            bool par_outer = par->base.parent == nullptr || !embi_is_element(par->base.parent) ||
                             svg_tag((const embi_element *)par->base.parent) == TG_NONE;
            if (!par_outer) {
                float tx[6] = {1, 0, 0, 1, svg_attr_px(&c, par, "x", c.vpw, 0), svg_attr_px(&c, par, "y", c.vph, 0)};
                m_mul(tx, m, m);
            }
            if (!screen) {
                break;
            }
        }
        cur = par;
        first = false;
    }
    svg_ctx_free(&c);
    for (int i = 0; i < 6; i++) {
        out[i] = m[i];
    }
    return true;
}

bool embi_svg_foreign_box(const embi_svg_geom *g, const embi_element *fo, float out[4])
{
    if (fo == nullptr || svg_tag(fo) != TG_FOREIGN) {
        return false;
    }
    svg_ctx c;
    geom_ctx(&c, g);
    /* percentages are of the nearest viewport: a viewBox, else the outermost svg's box or a nested svg's size */
    const embi_element *vp = nullptr;
    for (const embi_node *n = fo->base.parent; n != nullptr && embi_is_element(n); n = n->parent) {
        if (svg_tag((const embi_element *)n) == TG_SVG) {
            vp = (const embi_element *)n;
            break;
        }
    }
    if (vp != nullptr) {
        float vb[4], vm[6];
        bool has_vb;
        bool outer = vp->base.parent == nullptr || !embi_is_element(vp->base.parent) ||
                     svg_tag((const embi_element *)vp->base.parent) == TG_NONE;
        float w = outer ? c.vpw : svg_attr_px(&c, vp, "width", c.vpw, c.vpw);
        float h = outer ? c.vph : svg_attr_px(&c, vp, "height", c.vph, c.vph);
        svg_viewport_matrix(&c, vp, w, h, vm, vb, &has_vb);
        c.vpw = has_vb ? vb[2] : w;
        c.vph = has_vb ? vb[3] : h;
    }
    float x = svg_attr_px(&c, fo, "x", c.vpw, 0), y = svg_attr_px(&c, fo, "y", c.vph, 0);
    float w = svg_attr_px(&c, fo, "width", c.vpw, 0), h = svg_attr_px(&c, fo, "height", c.vph, 0);
    svg_ctx_free(&c);
    double m[6];
    if (!embi_svg_ctm(g, fo, true, m)) {
        return false;
    }
    out[0] = (float)(m[0] * x + m[2] * y + m[4]);
    out[1] = (float)(m[1] * x + m[3] * y + m[5]);
    out[2] = w;
    out[3] = h;
    return w > 0 && h > 0 && isfinite(out[0]) && isfinite(out[1]);
}

static embi_path *el_path(svg_ctx *c, const embi_element *el, const embi_style **st)
{
    uint8_t t = svg_tag(el);
    if (!(t >= TG_PATH && t <= TG_POLYGON)) {
        return nullptr;
    }
    *st = (const embi_style *)el->style;
    return svg_shape_path(c, el, *st, t, nullptr);
}

bool embi_svg_total_length(const embi_svg_geom *g, const embi_element *el, double *out)
{
    svg_ctx c;
    geom_ctx(&c, g);
    const embi_style *st;
    embi_path *p = el_path(&c, el, &st);
    svg_ctx_free(&c);
    if (p == nullptr) {
        return false;
    }
    *out = svgp_length(p);
    svgp_free(p);
    return true;
}

bool embi_svg_point_at_length(const embi_svg_geom *g, const embi_element *el, double dist, double out[2])
{
    svg_ctx c;
    geom_ctx(&c, g);
    const embi_style *st;
    embi_path *p = el_path(&c, el, &st);
    svg_ctx_free(&c);
    if (p == nullptr) {
        return false;
    }
    bool ok = svgp_point_at(p, dist, out, nullptr);
    svgp_free(p);
    return ok;
}

bool embi_svg_point_in_fill(const embi_svg_geom *g, const embi_element *el, double x, double y)
{
    svg_ctx c;
    geom_ctx(&c, g);
    const embi_style *st;
    embi_path *p = el_path(&c, el, &st);
    svg_ctx_free(&c);
    if (p == nullptr) {
        return false;
    }
    int rule = svg_kw(st, EMBI_PROP_fill_rule) == EMBI_KW_evenodd ? 1 : 0;
    bool in = svgp_contains(p, rule, x, y);
    svgp_free(p);
    return in;
}

bool embi_svg_point_in_stroke(const embi_svg_geom *g, const embi_element *el, double x, double y)
{
    svg_ctx c;
    geom_ctx(&c, g);
    const embi_style *st;
    embi_path *p = el_path(&c, el, &st);
    bool in = false;
    if (p != nullptr) {
        svg_stroke s;
        memset(&s, 0, sizeof s);
        s.width = svg_stroke_width_of(&c, st);
        s.miter_limit = (float)svg_num(st, EMBI_PROP_stroke_miterlimit, 4.0);
        switch (svg_kw(st, EMBI_PROP_stroke_linecap)) {
        case EMBI_KW_round: s.cap = 1; break;
        case EMBI_KW_square: s.cap = 2; break;
        default: break;
        }
        switch (svg_kw(st, EMBI_PROP_stroke_linejoin)) {
        case EMBI_KW_round: s.join = 1; break;
        case EMBI_KW_bevel: s.join = 2; break;
        default: break;
        }
        embi_path *sk = svgp_stroke(p, &s, 0.05f);
        if (sk != nullptr) {
            in = svgp_contains(sk, 0, x, y);
            svgp_free(sk);
        }
        svgp_free(p);
    }
    svg_ctx_free(&c);
    return in;
}
