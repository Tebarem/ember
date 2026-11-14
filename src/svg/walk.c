/*
 * The SVG walker (R4c): one recursive pass over the SVG subtree of an element that paints it (SM_PAINT) or finds the element
 * under a point (SM_HIT). Nothing is kept between calls: styles of use instances are computed on the way and released at the
 * end of the call, paths go straight into the display list.
 */
#include "svg/svg_int.h"

#include "dom/dstr.h"
#include "layout/clip_path.h"
#include "layout/layout.h"
#include "layout/xform.h"
#include "paint/dl.h"

#include <SDL3/SDL_stdinc.h>

#define SVG_MAX_NODES 40000
#define SVG_MAX_BBOX_NODES 400000
#define SVG_MAX_DEPTH 64

/* ------------------------------------------------------------------ elements and attributes */

static const struct {
    const char *name;
    uint8_t tag;
} k_tags[] = {
    {"svg", TG_SVG}, {"g", TG_G}, {"defs", TG_DEFS}, {"symbol", TG_SYMBOL}, {"use", TG_USE}, {"switch", TG_SWITCH},
    {"a", TG_A}, {"path", TG_PATH}, {"rect", TG_RECT}, {"circle", TG_CIRCLE}, {"ellipse", TG_ELLIPSE}, {"line", TG_LINE},
    {"polyline", TG_POLYLINE}, {"polygon", TG_POLYGON}, {"image", TG_IMAGE}, {"text", TG_TEXT}, {"foreignObject", TG_FOREIGN},
    {"linearGradient", TG_LGRAD}, {"radialGradient", TG_RGRAD}, {"pattern", TG_PATTERN}, {"clipPath", TG_CLIPPATH},
    {"mask", TG_MASK}, {"marker", TG_MARKER}, {"view", TG_VIEW},
};

uint8_t svg_tag(const embi_element *el)
{
    if (el == nullptr || el->base.extra != EMBI_NS_SVG) {
        return TG_NONE;
    }
    embi_str n = embi_atom_str(el->base.name);
    for (size_t i = 0; i < sizeof k_tags / sizeof k_tags[0]; i++) {
        if (strlen(k_tags[i].name) == n.len && memcmp(k_tags[i].name, n.ptr, n.len) == 0) {
            return k_tags[i].tag;
        }
    }
    return TG_OTHER;
}

bool svg_attr_sb(const embi_element *el, const char *name, embi_sb *sb)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(sb, a->value);
    return true;
}

bool svg_href_sb(const embi_element *el, embi_sb *sb)
{
    return svg_attr_sb(el, "href", sb) || svg_attr_sb(el, "xlink:href", sb);
}

bool svg_attr_len_raw(const embi_element *el, const char *name, svg_len *out)
{
    embi_sb sb = {0};
    bool ok = svg_attr_sb(el, name, &sb) && svg_parse_length(sb.data != nullptr ? sb.data : "", sb.len, out);
    embi_sb_free(&sb);
    return ok;
}

float svg_font_px(const embi_style *st)
{
    const embi_value *v = st != nullptr ? embi_style_value(st, EMBI_PROP_font_size) : nullptr;
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->num > 0) {
        return (float)v->num;
    }
    return 16.0f;
}

/* The style that em and ex units of el's attributes use: inside a use instance, the instance's. */
static const embi_style *font_style(const svg_ctx *c, const embi_element *el)
{
    return c->cur_el == el && c->cur_st != nullptr ? c->cur_st : (const embi_style *)el->style;
}

float svg_attr_px(const svg_ctx *c, const embi_element *el, const char *name, float pct_base, float fallback)
{
    svg_len l;
    if (!svg_attr_len_raw(el, name, &l)) {
        return fallback;
    }
    return svg_len_px(l, svg_font_px(font_style(c, el)), pct_base);
}

float svg_attr_num(const embi_element *el, const char *name, float fallback)
{
    embi_sb sb = {0};
    float r = fallback;
    if (svg_attr_sb(el, name, &sb) && sb.len > 0) {
        const char *p = svg_skip_ws(sb.data, sb.data + sb.len);
        double v;
        if (svg_number(&p, sb.data + sb.len, &v)) {
            r = (float)v;
        }
    }
    embi_sb_free(&sb);
    return r;
}

bool svg_ref_id(embi_str t, embi_str *id)
{
    if (t.ptr == nullptr || t.len == 0) {
        return false;
    }
    const char *p = t.ptr, *end = t.ptr + t.len;
    p = svg_skip_ws(p, end);
    if (end - p >= 4 && memcmp(p, "url(", 4) == 0) {
        p = svg_skip_ws(p + 4, end);
        if (p < end && (*p == '"' || *p == '\'')) {
            p++;
        }
    }
    if (p >= end || *p != '#') {
        return false;
    }
    p++;
    const char *s = p;
    while (p < end && *p != ')' && *p != '"' && *p != '\'' && *p != ' ') {
        p++;
    }
    if (p == s) {
        return false;
    }
    *id = (embi_str){s, (uint32_t)(p - s)};
    return true;
}

embi_element *svg_ref_element(const embi_document *doc, embi_str ref)
{
    embi_str id;
    if (doc == nullptr || !svg_ref_id(ref, &id)) {
        return nullptr;
    }
    return embi_get_element_by_id((embi_document *)doc, id);
}

uint16_t svg_kw(const embi_style *st, embi_prop_id p)
{
    const embi_value *v = st != nullptr ? embi_style_value(st, p) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

double svg_num(const embi_style *st, embi_prop_id p, double fallback)
{
    const embi_value *v = st != nullptr ? embi_style_value(st, p) : nullptr;
    if (v == nullptr || (v->kind != EMBI_V_NUMBER && v->kind != EMBI_V_INTEGER && v->kind != EMBI_V_LENGTH)) {
        return fallback;
    }
    return v->num;
}

bool svg_color(const embi_style *st, const embi_value *v, uint32_t *rgba)
{
    if (v == nullptr) {
        return false;
    }
    const embi_color *c = nullptr;
    if (v->kind == EMBI_V_COLOR) {
        c = &v->color;
    } else if (v->kind == EMBI_V_LIGHT_DARK && v->pair != nullptr && v->pair[0].kind == EMBI_V_COLOR) {
        c = &v->pair[0].color;
    } else {
        return false;
    }
    embi_color out;
    if (c->space == EMBI_CS_CURRENT) {
        const embi_value *cv = embi_style_value(st, EMBI_PROP_color);
        if (cv == nullptr || cv->kind != EMBI_V_COLOR || cv->color.space == EMBI_CS_CURRENT) {
            return false;
        }
        out = cv->color;
    } else if (c->space == EMBI_CS_DEFERRED) {
        const embi_value *cv = embi_style_value(st, EMBI_PROP_color);
        if (cv == nullptr || cv->kind != EMBI_V_COLOR || !embi_color_resolve_deferred(c, &cv->color, &out)) {
            return false;
        }
    } else {
        out = *c;
    }
    uint8_t px[4];
    if (!embi_color_to_srgb8(&out, px)) {
        return false;
    }
    *rgba = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
    return true;
}

/* ------------------------------------------------------------------ context */

void svg_ctx_init(svg_ctx *c, const embi_svg_env *env, int mode)
{
    memset(c, 0, sizeof *c);
    c->env = env;
    c->mode = mode;
    c->dev = env != nullptr && env->dev_scale > 0 ? env->dev_scale : 1.0f;
    c->rvw = env != nullptr && env->vw > 0 ? env->vw : 1.0f;
    c->rvh = env != nullptr && env->vh > 0 ? env->vh : 1.0f;
    m_identity(c->base);
}

void svg_ctx_free(svg_ctx *c)
{
    if (c->env != nullptr && c->env->se != nullptr) {
        for (uint32_t i = 0; i < c->owned.len; i++) {
            embi_style_release(c->env->se, c->owned.data[i]);
        }
    }
    embi_vec_free(&c->owned);
    embi_vec_free(&c->stack);
}

const embi_style *svg_style_of(svg_ctx *c, embi_element *el, const embi_style *parent_for_use)
{
    if (parent_for_use != nullptr && c->env != nullptr && c->env->se != nullptr) {
        const embi_style *s = embi_style_compute_under(c->env->se, el, parent_for_use);
        if (s != nullptr) {
            embi_vec_push(&c->owned, s);
            return s;
        }
    }
    return (const embi_style *)el->style;
}

/* ------------------------------------------------------------------ geometry of shapes */

static float pct_x(const svg_ctx *c) { return c->vpw; }
static float pct_y(const svg_ctx *c) { return c->vph; }

static float len_attr(const svg_ctx *c, const embi_element *el, const char *name, int axis, float fb)
{
    svg_len l;
    if (!svg_attr_len_raw(el, name, &l)) {
        return fb;
    }
    float base = axis == 0 ? pct_x(c) : axis == 1 ? pct_y(c) : sqrtf((c->vpw * c->vpw + c->vph * c->vph) * 0.5f);
    return svg_len_px(l, svg_font_px(font_style(c, el)), base);
}

static bool len_present(const embi_element *el, const char *name, svg_len *l)
{
    return svg_attr_len_raw(el, name, l);
}

static void ellipse_path(embi_path *p, float cx, float cy, float rx, float ry)
{
    (void)svgp_move(p, cx + rx, cy);
    (void)svgp_arc(p, rx, ry, 0, false, true, cx, cy + ry);
    (void)svgp_arc(p, rx, ry, 0, false, true, cx - rx, cy);
    (void)svgp_arc(p, rx, ry, 0, false, true, cx, cy - ry);
    (void)svgp_arc(p, rx, ry, 0, false, true, cx + rx, cy);
    (void)svgp_close(p);
}

embi_path *svg_shape_path(const svg_ctx *c, const embi_element *el, const embi_style *st, uint8_t tag, svg_verts *verts)
{
    (void)st;
    float tol = c->tol > 0 ? c->tol : 0.0f;
    switch (tag) {
    case TG_PATH: {
        embi_sb sb = {0};
        embi_path *p = nullptr;
        if (svg_attr_sb(el, "d", &sb) && sb.len > 0) {
            bool err;
            p = svg_parse_path(sb.data, sb.len, tol, verts, &err);
            if (svgp_empty(p) && (verts == nullptr || verts->n == 0)) { /* markers still sit on the vertices of an empty path */
                svgp_free(p);
                p = nullptr;
            }
        }
        embi_sb_free(&sb);
        return p;
    }
    case TG_RECT: {
        float w = len_attr(c, el, "width", 0, 0), h = len_attr(c, el, "height", 1, 0);
        if (!(w > 0) || !(h > 0)) {
            return nullptr;
        }
        float x = len_attr(c, el, "x", 0, 0), y = len_attr(c, el, "y", 1, 0);
        svg_len lx, ly;
        bool hx = len_present(el, "rx", &lx) && lx.v >= 0, hy = len_present(el, "ry", &ly) && ly.v >= 0;
        float rx = hx ? len_attr(c, el, "rx", 0, 0) : 0, ry = hy ? len_attr(c, el, "ry", 1, 0) : 0;
        if (hx && !hy) {
            ry = rx;
        } else if (hy && !hx) {
            rx = ry;
        }
        rx = fminf(rx, w * 0.5f);
        ry = fminf(ry, h * 0.5f);
        embi_path *p = svgp_new(tol);
        if (rx > 0 && ry > 0) {
            (void)svgp_move(p, x + rx, y);
            (void)svgp_line(p, x + w - rx, y);
            (void)svgp_arc(p, rx, ry, 0, false, true, x + w, y + ry);
            (void)svgp_line(p, x + w, y + h - ry);
            (void)svgp_arc(p, rx, ry, 0, false, true, x + w - rx, y + h);
            (void)svgp_line(p, x + rx, y + h);
            (void)svgp_arc(p, rx, ry, 0, false, true, x, y + h - ry);
            (void)svgp_line(p, x, y + ry);
            (void)svgp_arc(p, rx, ry, 0, false, true, x + rx, y);
        } else {
            (void)svgp_move(p, x, y);
            (void)svgp_line(p, x + w, y);
            (void)svgp_line(p, x + w, y + h);
            (void)svgp_line(p, x, y + h);
        }
        (void)svgp_close(p);
        return p;
    }
    case TG_CIRCLE: {
        float r = len_attr(c, el, "r", 2, 0);
        if (!(r > 0)) {
            return nullptr;
        }
        embi_path *p = svgp_new(tol);
        ellipse_path(p, len_attr(c, el, "cx", 0, 0), len_attr(c, el, "cy", 1, 0), r, r);
        return p;
    }
    case TG_ELLIPSE: {
        svg_len lx, ly;
        bool hx = len_present(el, "rx", &lx) && lx.v >= 0, hy = len_present(el, "ry", &ly) && ly.v >= 0;
        float rx = hx ? len_attr(c, el, "rx", 0, 0) : 0, ry = hy ? len_attr(c, el, "ry", 1, 0) : 0;
        if (hx && !hy) {
            ry = rx;
        } else if (hy && !hx) {
            rx = ry;
        }
        if (!(rx > 0) || !(ry > 0)) {
            return nullptr;
        }
        embi_path *p = svgp_new(tol);
        ellipse_path(p, len_attr(c, el, "cx", 0, 0), len_attr(c, el, "cy", 1, 0), rx, ry);
        return p;
    }
    case TG_LINE: {
        float x1 = len_attr(c, el, "x1", 0, 0), y1 = len_attr(c, el, "y1", 1, 0);
        float x2 = len_attr(c, el, "x2", 0, 0), y2 = len_attr(c, el, "y2", 1, 0);
        embi_path *p = svgp_new(tol);
        (void)svgp_move(p, x1, y1);
        (void)svgp_line(p, x2, y2);
        if (verts != nullptr) {
            double xy[4] = {x1, y1, x2, y2};
            svg_verts_from_points(verts, xy, 2, false);
        }
        return p;
    }
    case TG_POLYLINE:
    case TG_POLYGON: {
        embi_sb sb = {0};
        embi_path *p = nullptr;
        if (svg_attr_sb(el, "points", &sb) && sb.len > 0) {
            /* a number takes at least one character and a separator, so len/2 + 1 bounds the count */
            uint32_t CAP = sb.len / 2 + 2;
            double *v = embi_alloc(CAP * sizeof *v);
            bool ok;
            uint32_t n = svg_number_list(sb.data, sb.len, v, CAP, &ok);
            n &= ~1u; /* an odd trailing coordinate is dropped */
            if (n >= 4) {
                p = svgp_new(tol);
                (void)svgp_move(p, (float)v[0], (float)v[1]);
                for (uint32_t i = 2; i < n; i += 2) {
                    (void)svgp_line(p, (float)v[i], (float)v[i + 1]);
                }
                if (tag == TG_POLYGON) {
                    (void)svgp_close(p);
                }
                if (verts != nullptr) {
                    svg_verts_from_points(verts, v, n / 2, tag == TG_POLYGON);
                }
            }
            embi_free(v, CAP * sizeof *v);
        }
        embi_sb_free(&sb);
        return p;
    }
    default:
        return nullptr;
    }
}

/* ------------------------------------------------------------------ matrices */

static bool is_default_origin(const embi_style *st)
{
    const embi_value *o = embi_style_value(st, EMBI_PROP_transform_origin);
    if (o == nullptr || o->kind != EMBI_V_POSITION || o->pos == nullptr) {
        return true;
    }
    const embi_value *x = &o->pos->x.off, *y = &o->pos->y.off;
    /* the initial value is written "50% 50% 0": an explicit two-value 50% 50% has no z and means the viewport centre */
    return x->kind == EMBI_V_PERCENT && x->num == 50.0 && y->kind == EMBI_V_PERCENT && y->num == 50.0 &&
           o->pos->x.anchor == 0 && o->pos->y.anchor == 0 && o->pos->z.kind != EMBI_V_NONE;
}

void svg_el_matrix(const svg_ctx *c, const embi_element *el, const embi_style *st, const float bbox[4], float m[6])
{
    m_identity(m);
    if (st != nullptr && embi_lay_has_transform(st)) {
        float bb[4] = {0, 0, 0, 0};
        if (!is_default_origin(st)) {
            uint16_t box = svg_kw(st, EMBI_PROP_transform_box);
            if ((box == EMBI_KW_fill_box || box == EMBI_KW_stroke_box) && bbox != nullptr) {
                memcpy(bb, bbox, sizeof bb);
            } else {
                bb[2] = c->vpw;
                bb[3] = c->vph;
            }
        }
        embi_m4 m4;
        if (embi_lay_box_matrix(st, bb, c->rvw, c->rvh, 16.0f, &m4)) {
            double p0[4], p1[4], p2[4];
            embi_m4_point(&m4, 0, 0, 0, p0);
            embi_m4_point(&m4, 1, 0, 0, p1);
            embi_m4_point(&m4, 0, 1, 0, p2);
            m[0] = (float)(p1[0] - p0[0]);
            m[1] = (float)(p1[1] - p0[1]);
            m[2] = (float)(p2[0] - p0[0]);
            m[3] = (float)(p2[1] - p0[1]);
            m[4] = (float)p0[0];
            m[5] = (float)p0[1];
        }
        return;
    }
    embi_sb sb = {0};
    if (svg_attr_sb(el, "transform", &sb) && sb.len > 0) {
        float t[6];
        if (svg_parse_transform(sb.data, sb.len, t)) {
            memcpy(m, t, sizeof t);
        }
    }
    embi_sb_free(&sb);
}

void svg_viewport_matrix(const svg_ctx *c, const embi_element *el, float w, float h, float m[6], float vb[4], bool *has_vb)
{
    m_identity(m);
    *has_vb = false;
    embi_sb sb = {0};
    const embi_element *pe = el; /* where the viewBox and preserveAspectRatio are read */
    bool got = false;
    if (c != nullptr && c->view != nullptr && el == c->view_root && svg_attr_sb(c->view, "viewBox", &sb) && sb.len > 0) {
        pe = c->view;
        got = true;
    } else {
        sb.len = 0;
    }
    if (!got) {
        got = svg_attr_sb(el, "viewBox", &sb) && sb.len > 0;
    }
    if (got) {
        double v[4];
        bool ok;
        uint32_t n = svg_number_list(sb.data, sb.len, v, 4, &ok);
        if (n == 4 && ok && v[2] > 0 && v[3] > 0) {
            for (int i = 0; i < 4; i++) {
                vb[i] = (float)v[i];
            }
            svg_par par = {5, false};
            embi_sb pb = {0};
            if (svg_attr_sb(pe, "preserveAspectRatio", &pb)) {
                par = svg_parse_par(pb.data != nullptr ? pb.data : "", pb.len);
            }
            embi_sb_free(&pb);
            svg_viewbox_matrix(vb, w, h, par, m);
            *has_vb = true;
        }
    }
    embi_sb_free(&sb);
}

/* ------------------------------------------------------------------ display list helpers */

uint32_t svg_em_xform(svg_ctx *c, const float m[6])
{
    embi_dl *d = c->dl;
    embi_dxform x;
    m_mul(c->base, m, x.m);
    x.scroll = d->cur_scroll;
    if (d->xforms.len > 0) {
        const embi_dxform *l = &d->xforms.data[d->xforms.len - 1];
        if (memcmp(l->m, x.m, sizeof x.m) == 0 && l->scroll == x.scroll) {
            return d->xforms.len - 1;
        }
    }
    embi_vec_push(&d->xforms, x);
    return d->xforms.len - 1;
}

uint32_t svg_em_clip(svg_ctx *c, const float rect[4], const float m[6], uint32_t parent)
{
    embi_dclip k;
    memset(&k, 0, sizeof k);
    memcpy(k.rect, rect, sizeof k.rect);
    k.xform = svg_em_xform(c, m);
    k.parent = parent;
    embi_vec_push(&c->dl->clips, k);
    return c->dl->clips.len - 1;
}

uint32_t svg_em_group_begin(svg_ctx *c, uint8_t sub, float opacity, uint32_t clip)
{
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_LAYER_BEGIN;
    it.sub = sub;
    it.f[0] = opacity;
    it.clip = clip;
    embi_vec_push(&c->dl->items, it);
    return c->dl->items.len - 1;
}

void svg_em_group_end(svg_ctx *c, uint32_t begin)
{
    embi_dl *d = c->dl;
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = begin;
    embi_vec_push(&d->items, end);
    d->items.data[begin].aux = d->items.len - 1;
    if (d->items.len - begin - 2 == 0) {
        d->items.len = begin; /* an empty group */
    }
}

/* ------------------------------------------------------------------ bounding boxes */

static bool uses_ok(svg_ctx *c, const embi_element *t)
{
    for (uint32_t i = 0; i < c->stack.len; i++) {
        if (c->stack.data[i] == t) {
            return false;
        }
    }
    return c->stack.len < SVG_MAX_DEPTH;
}

static void union_box(float acc[4], bool *have, const float b[4])
{
    if (!*have) {
        memcpy(acc, b, 4 * sizeof(float));
        *have = true;
        return;
    }
    float x0 = fminf(acc[0], b[0]), y0 = fminf(acc[1], b[1]);
    float x1 = fmaxf(acc[0] + acc[2], b[0] + b[2]), y1 = fmaxf(acc[1] + acc[3], b[1] + b[3]);
    acc[0] = x0;
    acc[1] = y0;
    acc[2] = x1 - x0;
    acc[3] = y1 - y0;
}

static bool el_rendered(const embi_element *el, const embi_style *st)
{
    (void)el;
    return st != nullptr && svg_kw(st, EMBI_PROP_display) != EMBI_KW_none;
}

/* The box of el in its own user space (without its own transform). */
bool svg_el_bbox(svg_ctx *c, embi_element *el, const embi_style *st, float out[4])
{
    uint8_t tag = svg_tag(el);
    if (c->depth > SVG_MAX_DEPTH || ++c->bbox_nodes > SVG_MAX_BBOX_NODES) {
        return false;
    }
    switch (tag) {
    case TG_PATH: case TG_RECT: case TG_CIRCLE: case TG_ELLIPSE: case TG_LINE: case TG_POLYLINE: case TG_POLYGON: {
        embi_path *p = svg_shape_path(c, el, st, tag, nullptr);
        if (p == nullptr) {
            return false;
        }
        bool ok = svgp_bbox(p, out);
        svgp_free(p);
        return ok;
    }
    case TG_TEXT:
        return svg_text_bbox(c, el, out);
    case TG_IMAGE:
    case TG_FOREIGN: {
        float w = len_attr(c, el, "width", 0, 0), h = len_attr(c, el, "height", 1, 0);
        if (!(w > 0) || !(h > 0)) {
            return false;
        }
        out[0] = len_attr(c, el, "x", 0, 0);
        out[1] = len_attr(c, el, "y", 1, 0);
        out[2] = w;
        out[3] = h;
        return true;
    }
    case TG_G: case TG_A: case TG_SVG: case TG_SWITCH: case TG_SYMBOL: case TG_USE: case TG_MARKER: {
        float acc[4] = {0, 0, 0, 0};
        bool have = false;
        c->depth++;
        if (tag == TG_USE) {
            embi_sb sb = {0};
            embi_element *t = nullptr;
            if (svg_href_sb(el, &sb)) {
                t = svg_ref_element(el->base.doc, (embi_str){sb.data, sb.len});
            }
            embi_sb_free(&sb);
            if (t != nullptr && uses_ok(c, t) && t != el) {
                embi_vec_push(&c->stack, (const embi_element *)t);
                const embi_style *ts = (const embi_style *)t->style;
                float b[4], m[6];
                if (el_rendered(t, ts) || svg_tag(t) == TG_SYMBOL) {
                    if (svg_el_bbox(c, t, ts, b)) {
                        svg_el_matrix(c, t, ts, b, m);
                        float tx = len_attr(c, el, "x", 0, 0), ty = len_attr(c, el, "y", 1, 0);
                        float tm[6] = {1, 0, 0, 1, tx, ty};
                        m_mul(tm, m, m);
                        float o[4];
                        m_map_rect(m, b, o);
                        union_box(acc, &have, o);
                    }
                }
                c->stack.len--;
            }
        } else {
            for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
                if (!embi_is_element(n)) {
                    continue;
                }
                embi_element *ch = embi_as_element(n);
                uint8_t ct = svg_tag(ch);
                if (ct == TG_NONE || ct >= TG_LGRAD) {
                    continue;
                }
                const embi_style *cs = (const embi_style *)ch->style;
                if (ct != TG_DEFS && ct != TG_SYMBOL && !el_rendered(ch, cs)) {
                    continue;
                }
                if (ct == TG_DEFS || ct == TG_SYMBOL) {
                    continue;
                }
                float b[4], m[6], o[4];
                if (svg_el_bbox(c, ch, cs, b)) {
                    svg_el_matrix(c, ch, cs, b, m);
                    m_map_rect(m, b, o);
                    union_box(acc, &have, o);
                }
            }
        }
        c->depth--;
        if (have) {
            memcpy(out, acc, sizeof acc);
        }
        return have;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ painting of shapes */

static bool pointer_hits(const embi_style *st, bool *fill, bool *stroke, bool fill_painted, bool stroke_painted)
{
    bool visible = svg_kw(st, EMBI_PROP_visibility) != EMBI_KW_hidden && svg_kw(st, EMBI_PROP_visibility) != EMBI_KW_collapse;
    *fill = *stroke = false;
    switch (svg_kw(st, EMBI_PROP_pointer_events)) {
    case EMBI_KW_none:
        return false;
    case EMBI_KW_visiblefill:
        *fill = visible;
        break;
    case EMBI_KW_visiblestroke:
        *stroke = visible;
        break;
    case EMBI_KW_visible:
        *fill = *stroke = visible;
        break;
    case EMBI_KW_painted:
        *fill = fill_painted;
        *stroke = stroke_painted;
        break;
    case EMBI_KW_fill:
        *fill = true;
        break;
    case EMBI_KW_stroke:
        *stroke = true;
        break;
    case EMBI_KW_all:
        *fill = *stroke = true;
        break;
    default: /* auto, visiblepainted */
        *fill = visible && fill_painted;
        *stroke = visible && stroke_painted;
        break;
    }
    return *fill || *stroke;
}

typedef struct stroke_props {
    float width;
    svg_stroke s;
    float dash[64];
} stroke_props;

float svg_stroke_width_of(const svg_ctx *c, const embi_style *st)
{
    const embi_value *v = embi_style_value(st, EMBI_PROP_stroke_width);
    if (v == nullptr) {
        return 1.0f;
    }
    if (v->kind == EMBI_V_PERCENT) {
        return (float)(v->num * sqrt(((double)c->vpw * c->vpw + (double)c->vph * c->vph) / 2.0) / 100.0);
    }
    if (v->kind == EMBI_V_LENGTH || v->kind == EMBI_V_NUMBER || v->kind == EMBI_V_INTEGER) {
        return (float)v->num;
    }
    return 1.0f;
}

static void load_stroke(const svg_ctx *c, const embi_style *st, stroke_props *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->s.width = svg_stroke_width_of(c, st);
    switch (svg_kw(st, EMBI_PROP_stroke_linecap)) {
    case EMBI_KW_round: sp->s.cap = 1; break;
    case EMBI_KW_square: sp->s.cap = 2; break;
    default: sp->s.cap = 0; break;
    }
    switch (svg_kw(st, EMBI_PROP_stroke_linejoin)) {
    case EMBI_KW_round: sp->s.join = 1; break;
    case EMBI_KW_bevel: sp->s.join = 2; break;
    default: sp->s.join = 0; break;
    }
    sp->s.miter_limit = (float)svg_num(st, EMBI_PROP_stroke_miterlimit, 4.0);
    if (sp->s.miter_limit < 1.0f) {
        sp->s.miter_limit = 1.0f;
    }
    {
        const embi_value *dov = embi_style_value(st, EMBI_PROP_stroke_dashoffset);
        if (dov != nullptr && dov->kind == EMBI_V_PERCENT) {
            sp->s.dash_offset = (float)(dov->num * sqrt(((double)c->vpw * c->vpw + (double)c->vph * c->vph) / 2.0) / 100.0);
        } else {
            sp->s.dash_offset = (float)svg_num(st, EMBI_PROP_stroke_dashoffset, 0.0);
        }
    }
    const embi_value *d = embi_style_value(st, EMBI_PROP_stroke_dasharray);
    if (d != nullptr && d->kind == EMBI_V_LIST && d->list.n > 0) {
        uint32_t n = 0;
        float sum = 0;
        for (uint32_t i = 0; i < d->list.n && n < 32; i++) {
            const embi_value *it = &d->list.items[i];
            float v = 0;
            if (it->kind == EMBI_V_PERCENT) {
                v = (float)(it->num * sqrt(((double)c->vpw * c->vpw + (double)c->vph * c->vph) / 2.0) / 100.0);
            } else if (it->kind == EMBI_V_LENGTH || it->kind == EMBI_V_NUMBER || it->kind == EMBI_V_INTEGER) {
                v = (float)it->num;
            }
            sp->dash[n++] = v;
            sum += v;
        }
        if (n > 0 && sum > 0) {
            if (n & 1u) {
                for (uint32_t i = 0; i < n; i++) {
                    sp->dash[n + i] = sp->dash[i];
                }
                n *= 2;
            }
            sp->s.dash = sp->dash;
            sp->s.ndash = n;
        }
    }
}

void svg_emit_stroke_of(svg_ctx *c, const embi_style *st, embi_path *path, const float m[6], uint32_t clip, uint32_t rgba)
{
    stroke_props props;
    load_stroke(c, st, &props);
    float sc = m_scale(m) * c->dev;
    c->tol = 0.2f / (sc > 1e-6f ? sc : 1e-6f);
    if (c->tol > 1e5f) {
        c->tol = 1e5f;
    }
    if (props.s.ndash == 0 && c->luma == 0) {
        svg_emit_stroke_lazy(c, path, &props.s, rgba, m, clip);
        return;
    }
    embi_path *sk = svgp_stroke(path, &props.s, c->tol);
    svgp_free(path);
    float bbox[4];
    if (sk == nullptr) {
        return;
    }
    if (!svgp_bbox(sk, bbox)) {
        svgp_free(sk);
        return;
    }
    svg_paint sp = {SP_COLOR, rgba, nullptr, 0, false};
    svg_emit_path(c, sk, 0, &sp, m, bbox, 1.0f, clip);
}

static float opacity_of(const embi_style *st, embi_prop_id p)
{
    double v = svg_num(st, p, 1.0);
    return v < 0 ? 0.0f : v > 1 ? 1.0f : (float)v;
}

/* The point mapped back into the shape's space. */
static bool unmap(const float m[6], float x, float y, double out[2])
{
    float inv[6];
    if (!m_invert(m, inv)) {
        return false;
    }
    out[0] = (double)inv[0] * x + (double)inv[2] * y + inv[4];
    out[1] = (double)inv[1] * x + (double)inv[3] * y + inv[5];
    return true;
}

typedef struct nctx {
    const embi_element *use; /* the outermost use element, or nullptr */
    bool inst;               /* styles are computed under their parents (use instances) */
} nctx;

static void walk_node(svg_ctx *c, embi_element *el, const embi_style *pst, nctx nc, const float M[6], uint32_t clip,
                      bool force_display);

static const float *idm_of(float m[6])
{
    m_identity(m);
    return m;
}

static embi_element *marker_ref(svg_ctx *c, const embi_style *st, embi_prop_id prop, const embi_element *el)
{
    const embi_value *v = embi_style_value(st, prop);
    if (v == nullptr || v->kind != EMBI_V_URL) {
        return nullptr;
    }
    embi_element *m = svg_ref_element(el->base.doc, v->str);
    if (m == nullptr || svg_tag(m) != TG_MARKER || !uses_ok(c, m)) {
        return nullptr;
    }
    return m;
}

static void emit_markers(svg_ctx *c, const embi_style *st, const svg_verts *vs, embi_element *mk[3], const float M[6], uint32_t clip);

static void do_shape(svg_ctx *c, embi_element *el, const embi_style *st, uint8_t tag, nctx nc, const float M[6], uint32_t clip)
{
    float sc = m_scale(M) * c->dev;
    c->tol = 0.2f / (sc > 1e-6f ? sc : 1e-6f);
    if (c->tol > 1e5f) {
        c->tol = 1e5f;
    }
    embi_element *mk[3] = {nullptr, nullptr, nullptr};
    svg_verts verts = {0};
    bool markers = false;
    if (c->mode == SM_PAINT && c->luma == 0 && (tag == TG_PATH || tag == TG_LINE || tag == TG_POLYLINE || tag == TG_POLYGON)) {
        mk[0] = marker_ref(c, st, EMBI_PROP_marker_start, el);
        mk[1] = marker_ref(c, st, EMBI_PROP_marker_mid, el);
        mk[2] = marker_ref(c, st, EMBI_PROP_marker_end, el);
        markers = mk[0] != nullptr || mk[1] != nullptr || mk[2] != nullptr;
    }
    embi_path *path = svg_shape_path(c, el, st, tag, markers ? &verts : nullptr);
    if (path == nullptr) {
        svg_verts_free(&verts);
        return;
    }
    float bbox[4];
    bool visible = svg_kw(st, EMBI_PROP_visibility) != EMBI_KW_hidden && svg_kw(st, EMBI_PROP_visibility) != EMBI_KW_collapse;
    if (!svgp_bbox(path, bbox)) {
        /* nothing to fill or stroke (a lone moveto, closed subpaths of one point): markers still go on its vertices */
        svgp_free(path);
        if (markers && visible) {
            emit_markers(c, st, &verts, mk, M, clip);
        }
        svg_verts_free(&verts);
        return;
    }
    svg_paint fp = svg_style_paint(c, el, st, false), sp = svg_style_paint(c, el, st, true);
    stroke_props props;
    load_stroke(c, st, &props);
    bool nss = svg_kw(st, EMBI_PROP_vector_effect) == EMBI_KW_non_scaling_stroke;
    bool has_stroke = sp.kind != SP_NONE && props.s.width > 0;
    int rule = svg_kw(st, c->luma == 2 ? EMBI_PROP_clip_rule : EMBI_PROP_fill_rule) == EMBI_KW_evenodd ? 1 : 0;

    if (c->mode == SM_HIT) {
        bool hf, hs;
        if (!pointer_hits(st, &hf, &hs, fp.kind != SP_NONE, has_stroke)) {
            svgp_free(path);
            return;
        }
        bool hit = false;
        double q[2];
        if (hf && unmap(M, c->hx, c->hy, q)) {
            hit = svgp_contains(path, rule, q[0], q[1]);
        }
        if (!hit && hs && props.s.width > 0) {
            embi_path *sk = nullptr;
            double q2[2];
            if (nss) {
                /* the stroke is a width in root units: test in root space */
                float id[6];
                m_identity(id);
                embi_path *rp = svgp_xform(path, M, c->tol);
                sk = svgp_stroke(rp, &props.s, c->tol);
                svgp_free(rp);
                if (sk != nullptr) {
                    hit = svgp_contains(sk, 0, c->hx, c->hy);
                }
            } else if (unmap(M, c->hx, c->hy, q2)) {
                sk = svgp_stroke(path, &props.s, c->tol);
                if (sk != nullptr) {
                    hit = svgp_contains(sk, 0, q2[0], q2[1]);
                }
            }
            if (sk != nullptr) {
                svgp_free(sk);
            }
        }
        if (hit) {
            c->hit = nc.use != nullptr ? nc.use : el;
        }
        svgp_free(path);
        return;
    }

    if (!visible && c->luma != 2) {
        svgp_free(path);
        svg_verts_free(&verts);
        return;
    }
    embi_path *spath = nullptr;
    const float *sm = M;
    float idm[6];
    /* a plain solid stroke is kept as its center line and expanded by the renderer: it costs the list a few quadratics
     * instead of the outline */
    bool lazy = has_stroke && !nss && c->luma == 0 && props.s.ndash == 0 && sp.kind == SP_COLOR;
    if (has_stroke && c->luma != 2 && !lazy) {
        if (nss) {
            embi_path *rp = svgp_xform(path, M, c->tol);
            spath = svgp_stroke(rp, &props.s, c->tol);
            svgp_free(rp);
            m_identity(idm);
            sm = idm;
        } else {
            spath = svgp_stroke(path, &props.s, c->tol);
        }
    }
    float fop = opacity_of(st, EMBI_PROP_fill_opacity), sop = opacity_of(st, EMBI_PROP_stroke_opacity);
    uint16_t po0 = 0;
    const embi_value *po = embi_style_value(st, EMBI_PROP_paint_order);
    if (po != nullptr && po->kind == EMBI_V_LIST && po->list.n > 0 && po->list.items[0].kind == EMBI_V_KEYWORD) {
        po0 = po->list.items[0].kw;
    }
    bool stroke_first = po0 == EMBI_KW_stroke;
    if (c->luma == 2) {
        svg_paint blk = {SP_COLOR, 0xFF000000u, nullptr, 0, false};
        svg_emit_path(c, path, rule, &blk, M, bbox, 1.0f, clip);
        return;
    }
    uint32_t lazy_rgba = 0;
    if (lazy) {
        float a = (float)(sp.rgba >> 24) * sop;
        lazy_rgba = (sp.rgba & 0x00FFFFFFu) | ((a <= 0 ? 0u : a >= 255 ? 255u : (uint32_t)(a + 0.5f)) << 24);
    }
    if (lazy && fp.kind == SP_COLOR) {
        /* a solid fill and a plain stroke: the two list items share one path */
        svg_emit_fill_stroke_shared(c, path, rule, fp.rgba, fop, &props.s, lazy_rgba, stroke_first, M, clip);
        lazy = false;
        path = nullptr;
    } else {
        if (stroke_first) {
            if (lazy) {
                svg_emit_stroke_lazy(c, svgp_xform(path, idm_of(idm), 0.0f), &props.s, lazy_rgba, M, clip);
                lazy = false;
            } else if (spath != nullptr) {
                svg_emit_path(c, spath, 0, &sp, sm, bbox, sop, clip);
                spath = nullptr;
            }
        }
        if (fp.kind != SP_NONE) {
            if (lazy) {
                svg_emit_path(c, svgp_xform(path, idm_of(idm), 0.0f), rule, &fp, M, bbox, fop, clip);
            } else {
                svg_emit_path(c, path, rule, &fp, M, bbox, fop, clip);
            }
        } else if (!lazy) {
            svgp_free(path);
        }
        if (lazy) {
            svg_emit_stroke_lazy(c, path, &props.s, lazy_rgba, M, clip);
        } else if (spath != nullptr) {
            svg_emit_path(c, spath, 0, &sp, sm, bbox, sop, clip);
        }
    }
    if (markers && visible) {
        emit_markers(c, st, &verts, mk, M, clip);
    }
    svg_verts_free(&verts);
}

/* ------------------------------------------------------------------ images */

static void do_image(svg_ctx *c, embi_element *el, const embi_style *st, nctx nc, const float M[6], uint32_t clip)
{
    float x = len_attr(c, el, "x", 0, 0), y = len_attr(c, el, "y", 1, 0);
    float w = len_attr(c, el, "width", 0, 0), h = len_attr(c, el, "height", 1, 0);
    if (c->env == nullptr) {
        return;
    }
    embi_sb sb = {0};
    if (!svg_href_sb(el, &sb) || sb.len == 0) {
        embi_sb_free(&sb);
        return;
    }
    embi_str url = {sb.data, sb.len};
    float iw = 0, ih = 0;
    bool ratio = false;
    const embi_svg_image *simg = c->env->svg_image != nullptr ? c->env->svg_image(c->env->user, url) : nullptr;
    if (simg != nullptr) {
        embi_svg_image_size(simg, &iw, &ih, &ratio);
    } else if (c->env->image_size == nullptr || !c->env->image_size(c->env->user, url, &iw, &ih, &ratio)) {
        embi_sb_free(&sb);
        return;
    }
    if (!(iw > 0) || !(ih > 0)) {
        embi_sb_free(&sb);
        return;
    }
    /* an absent or zero size is auto: the natural size, keeping the ratio when only one is given */
    if (!(w > 0) && !(h > 0)) {
        w = iw;
        h = ih;
    } else if (!(w > 0)) {
        w = h * iw / ih;
    } else if (!(h > 0)) {
        h = w * ih / iw;
    }
    if (c->mode == SM_HIT) {
        double q[2];
        bool visible = svg_kw(st, EMBI_PROP_visibility) != EMBI_KW_hidden;
        if (visible && svg_kw(st, EMBI_PROP_pointer_events) != EMBI_KW_none && unmap(M, c->hx, c->hy, q) && q[0] >= x &&
            q[0] <= x + w && q[1] >= y && q[1] <= y + h) {
            c->hit = nc.use != nullptr ? nc.use : el;
        }
        embi_sb_free(&sb);
        return;
    }
    if (svg_kw(st, EMBI_PROP_visibility) == EMBI_KW_hidden) {
        embi_sb_free(&sb);
        return;
    }
    svg_par par = {5, false};
    embi_sb pb = {0};
    if (svg_attr_sb(el, "preserveAspectRatio", &pb)) {
        par = svg_parse_par(pb.data != nullptr ? pb.data : "", pb.len);
    }
    embi_sb_free(&pb);
    float dx = x, dy = y, dw = w, dh = h;
    uint32_t lclip = clip;
    if (par.align != 0) {
        float s = par.slice ? fmaxf(w / iw, h / ih) : fminf(w / iw, h / ih);
        dw = iw * s;
        dh = ih * s;
        int xi = (par.align - 1) % 3, yi = (par.align - 1) / 3;
        dx = x + (w - dw) * 0.5f * (float)xi;
        dy = y + (h - dh) * 0.5f * (float)yi;
        if (par.slice) {
            float r[4] = {x, y, w, h};
            lclip = svg_em_clip(c, r, M, clip);
        }
    }
    uint32_t xf = svg_em_xform(c, M);
    float dest[4] = {dx, dy, dw, dh}, uv[4] = {0, 0, 1, 1};
    if (simg != nullptr) {
        embi_svg_paint_image(c->env, c->dl, simg, dest, uv, 0xFFFFFFFFu, xf, lclip);
    } else {
        embi_dl *d = c->dl;
        uint32_t n = d->urls.len / 2, off = d->urltext.len;
        for (uint32_t i = 0; i < url.len; i++) {
            embi_vec_push(&d->urltext, url.ptr[i]);
        }
        embi_vec_push(&d->urls, off);
        embi_vec_push(&d->urls, url.len);
        embi_ditem it;
        memset(&it, 0, sizeof it);
        it.kind = EMBI_DI_IMAGE;
        it.xform = xf;
        it.clip = lclip;
        memcpy(it.rect, dest, sizeof dest);
        memcpy(it.f, uv, sizeof uv);
        it.colors[0] = 0xFFFFFFFFu;
        it.aux = n;
        embi_vec_push(&d->items, it);
    }
    embi_sb_free(&sb);
}

/* ------------------------------------------------------------------ foreignObject */

/* The viewport of a foreignObject: x, y, width and height in its user space (SVG 2 section 23.2). Its HTML content was laid
 * out by layout in a box of that size (embi_layout_set_svg_fo), which the host paints and hit tests through the env. The
 * content is drawn where the walk of the svg itself meets the element: not in use instances, markers, patterns, masks and
 * clip paths (the stack holds the element they were reached through), whose copies of the box would paint one fragment
 * tree several times. */
static void do_foreign(svg_ctx *c, embi_element *el, const embi_style *st, nctx nc, const float M[6], uint32_t clip)
{
    if (c->env == nullptr || nc.inst || c->luma != 0 || c->stack.len != 0) {
        return;
    }
    float x = len_attr(c, el, "x", 0, 0), y = len_attr(c, el, "y", 1, 0);
    float w = len_attr(c, el, "width", 0, 0), h = len_attr(c, el, "height", 1, 0);
    if (!(w > 0) || !(h > 0)) {
        return;
    }
    float tm[6] = {1, 0, 0, 1, x, y}, m2[6];
    m_mul(M, tm, m2);
    bool clipped = svg_kw(st, EMBI_PROP_overflow_x) != EMBI_KW_visible;
    if (c->mode == SM_HIT) {
        double q[2];
        if (c->env->layout == nullptr || !unmap(m2, c->hx, c->hy, q)) {
            return;
        }
        if (clipped && (q[0] < 0 || q[1] < 0 || q[0] > w || q[1] > h)) {
            return;
        }
        const embi_element *r = embi_layout_hit_svg_fo(c->env->layout, el, (float)q[0], (float)q[1]);
        if (r != nullptr) {
            c->hit = r;
        }
        return;
    }
    if (c->env->paint_foreign == nullptr || c->dl == nullptr) {
        return;
    }
    uint32_t vclip = clip;
    if (clipped) {
        float r[4] = {0, 0, w, h};
        vclip = svg_em_clip(c, r, m2, clip);
    }
    c->env->paint_foreign(c->env->host, el, svg_em_xform(c, m2), vclip);
}

/* ------------------------------------------------------------------ clip-path and mask */

static bool attr_is(const embi_element *el, const char *name, const char *val)
{
    embi_sb sb = {0};
    size_t n = strlen(val);
    bool r = svg_attr_sb(el, name, &sb) && sb.len == n && memcmp(sb.data, val, n) == 0;
    embi_sb_free(&sb);
    return r;
}

/* The element a clip-path or mask reference names: from the computed property (so that a stylesheet rule or a style attribute
 * counts as well as the presentation attribute, which reaches the property through the cascade), else from the attribute. */
static bool ref_prop(const embi_element *el, const embi_style *st, embi_prop_id prop, const char *name, embi_sb *sb,
                     embi_element **target)
{
    if (st != nullptr && embi_style_value_text(st, prop, sb) && sb->len > 0 && sb->data != nullptr && sb->len >= 5 &&
        memcmp(sb->data, "url(", 4) == 0) {
        *target = svg_ref_element(el->base.doc, (embi_str){sb->data, sb->len});
        if (*target != nullptr) {
            return true;
        }
    }
    embi_sb_free(sb);
    memset(sb, 0, sizeof *sb);
    if (!svg_attr_sb(el, name, sb) || sb->len == 0) {
        return false;
    }
    *target = svg_ref_element(el->base.doc, (embi_str){sb->data, sb->len});
    return *target != nullptr;
}

static void walk_children(svg_ctx *c, embi_element *parent, const embi_style *pst, nctx nc, const float M[6], uint32_t clip);

/* Emits the clipPath cp for the element (matrix M, box bbox) as a mask group. */
static bool clip_needs_bbox(const embi_element *cp)
{
    return attr_is(cp, "clipPathUnits", "objectBoundingBox");
}

static bool mask_units(const embi_element *mk, bool *region_obb, bool *content_obb)
{
    *region_obb = !attr_is(mk, "maskUnits", "userSpaceOnUse");
    *content_obb = attr_is(mk, "maskContentUnits", "objectBoundingBox");
    return *region_obb || *content_obb;
}

/* The shapes of the clipPath cp as solid black items (the clip path of an element with matrix M and box bbox). */
static bool emit_clip_content(svg_ctx *c, embi_element *cp, const float M[6], const float bbox[4], bool have_bbox, uint32_t clip)
{
    const embi_style *cst = (const embi_style *)cp->style;
    bool obb = clip_needs_bbox(cp);
    float m[6];
    memcpy(m, M, sizeof m);
    if (obb) {
        if (!have_bbox) {
            return false;
        }
        float bm[6] = {bbox[2], 0, 0, bbox[3], bbox[0], bbox[1]};
        m_mul(m, bm, m);
    }
    float cm[6];
    svg_el_matrix(c, cp, cst, nullptr, cm);
    m_mul(m, cm, m);
    int saved = c->luma;
    c->luma = 2;
    for (embi_node *n = cp->base.first_child; n != nullptr; n = n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *ch = embi_as_element(n);
        uint8_t t = svg_tag(ch);
        if (t == TG_USE || (t >= TG_PATH && t <= TG_POLYGON) || t == TG_TEXT) {
            walk_node(c, ch, cst, (nctx){nullptr, false}, m, clip, false);
        }
    }
    c->luma = saved;
    return true;
}

/* Returns false when nothing was emitted: a clipPath without drawable content clips everything away. */
static bool emit_clip_path(svg_ctx *c, embi_element *cp, const float M[6], const float bbox[4], bool have_bbox, uint32_t clip)
{
    if (clip_needs_bbox(cp) && !have_bbox) {
        return false;
    }
    uint32_t g = svg_em_group_begin(c, EMBI_LS_SVGMASK, 1.0f, clip);
    emit_clip_content(c, cp, M, bbox, have_bbox, clip);
    svg_em_group_end(c, g);
    return c->dl->items.len > g;
}

/* The content of the mask mk (an element with matrix M and box bbox) as items whose alpha is the mask value, inside the
 * mask region. False when the mask cannot apply (its units need a box the element has not). */
static bool emit_mask_content(svg_ctx *c, embi_element *mk, const float M[6], const float bbox[4], bool have_bbox, uint32_t clip)
{
    const embi_style *mst = (const embi_style *)mk->style;
    bool region_obb, content_obb;
    mask_units(mk, &region_obb, &content_obb);
    if ((region_obb || content_obb) && !have_bbox) {
        return false;
    }
    float rx, ry, rw, rh;
    svg_len l;
    if (region_obb) {
        rx = bbox[0] + bbox[2] * (svg_attr_len_raw(mk, "x", &l) ? (l.unit == SVGL_PCT ? l.v / 100.0f : l.v) : -0.1f);
        ry = bbox[1] + bbox[3] * (svg_attr_len_raw(mk, "y", &l) ? (l.unit == SVGL_PCT ? l.v / 100.0f : l.v) : -0.1f);
        rw = bbox[2] * (svg_attr_len_raw(mk, "width", &l) ? (l.unit == SVGL_PCT ? l.v / 100.0f : l.v) : 1.2f);
        rh = bbox[3] * (svg_attr_len_raw(mk, "height", &l) ? (l.unit == SVGL_PCT ? l.v / 100.0f : l.v) : 1.2f);
    } else {
        rx = svg_attr_len_raw(mk, "x", &l) ? svg_len_px(l, 16, c->vpw) : -0.1f * c->vpw;
        ry = svg_attr_len_raw(mk, "y", &l) ? svg_len_px(l, 16, c->vph) : -0.1f * c->vph;
        rw = svg_attr_len_raw(mk, "width", &l) ? svg_len_px(l, 16, c->vpw) : 1.2f * c->vpw;
        rh = svg_attr_len_raw(mk, "height", &l) ? svg_len_px(l, 16, c->vph) : 1.2f * c->vph;
    }
    if (!(rw > 0) || !(rh > 0)) {
        return false;
    }
    float region[4] = {rx, ry, rw, rh};
    uint32_t rclip = svg_em_clip(c, region, M, clip);
    float m[6];
    memcpy(m, M, sizeof m);
    if (content_obb) {
        float bm[6] = {bbox[2], 0, 0, bbox[3], bbox[0], bbox[1]};
        m_mul(m, bm, m);
    }
    int saved = c->luma;
    c->luma = svg_kw(mst, EMBI_PROP_mask_type) == EMBI_KW_alpha ? 3 : 1;
    walk_children(c, mk, mst, (nctx){nullptr, false}, m, rclip);
    c->luma = saved;
    return true;
}

/* Returns false when the mask has no content (the element is then not rendered). */
static bool emit_mask(svg_ctx *c, embi_element *mk, const float M[6], const float bbox[4], bool have_bbox, uint32_t clip)
{
    uint32_t g = svg_em_group_begin(c, EMBI_LS_SVGMASK, 1.0f, clip);
    if (!emit_mask_content(c, mk, M, bbox, have_bbox, clip)) {
        c->dl->items.len = g;
        return false;
    }
    svg_em_group_end(c, g);
    return c->dl->items.len > g;
}

/* ------------------------------------------------------------------ R4e: blend modes, isolation, CSS clip-path shapes */

/* The EMBI_BM_* of a mix-blend-mode keyword (the SVG module cannot call the painter's embi_pb_blend_kw). */
static uint32_t svgfx_blend_kw(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_multiply: return EMBI_BM_MULTIPLY;
    case EMBI_KW_screen: return EMBI_BM_SCREEN;
    case EMBI_KW_overlay: return EMBI_BM_OVERLAY;
    case EMBI_KW_darken: return EMBI_BM_DARKEN;
    case EMBI_KW_lighten: return EMBI_BM_LIGHTEN;
    case EMBI_KW_color_dodge: return EMBI_BM_COLOR_DODGE;
    case EMBI_KW_color_burn: return EMBI_BM_COLOR_BURN;
    case EMBI_KW_hard_light: return EMBI_BM_HARD_LIGHT;
    case EMBI_KW_soft_light: return EMBI_BM_SOFT_LIGHT;
    case EMBI_KW_difference: return EMBI_BM_DIFFERENCE;
    case EMBI_KW_exclusion: return EMBI_BM_EXCLUSION;
    case EMBI_KW_hue: return EMBI_BM_HUE;
    case EMBI_KW_saturation: return EMBI_BM_SATURATION;
    case EMBI_KW_color: return EMBI_BM_COLOR;
    case EMBI_KW_luminosity: return EMBI_BM_LUMINOSITY;
    case EMBI_KW_plus_lighter: return EMBI_BM_PLUS_LIGHTER;
    case EMBI_KW_plus_darker: return EMBI_BM_PLUS_DARKER;
    default: return EMBI_BM_NORMAL;
    }
}

static uint32_t svgfx_blend(const embi_style *st)
{
    const embi_value *v = st != nullptr ? embi_style_value(st, EMBI_PROP_mix_blend_mode) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? svgfx_blend_kw(v->kw) : EMBI_BM_NORMAL;
}

/* Whether a rendered descendant of el (through use elements too) has a blend mode: an isolated group pays for a layer only
 * then. budget bounds the visits. */
static bool svgfx_blend_below(const embi_element *el, int depth, uint32_t *budget)
{
    if (depth > 24) {
        return false;
    }
    for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *ch = embi_as_element(n);
        uint8_t t = svg_tag(ch);
        if (t == TG_NONE || t >= TG_LGRAD || t == TG_DEFS || t == TG_SYMBOL || *budget == 0) {
            continue;
        }
        (*budget)--;
        if (svgfx_blend((const embi_style *)ch->style) != EMBI_BM_NORMAL) {
            return true;
        }
        if (t == TG_USE) {
            embi_sb sb = {0};
            embi_element *tg = svg_href_sb(ch, &sb) ? svg_ref_element(ch->base.doc, (embi_str){sb.data, sb.len}) : nullptr;
            embi_sb_free(&sb);
            if (tg != nullptr && tg != ch && (svgfx_blend((const embi_style *)tg->style) != EMBI_BM_NORMAL ||
                                              svgfx_blend_below(tg, depth + 1, budget))) {
                return true;
            }
        } else if (svgfx_blend_below(ch, depth + 1, budget)) {
            return true;
        }
    }
    return false;
}

/* Opens a COMPOSITE group (dl.h): a blend mode, the clip-path with index cp1 - 1 in dl->cpaths (in the space of xform) and the
 * opacity. Closed by svgfx_group_end. */
static uint32_t svgfx_group_begin(svg_ctx *c, uint32_t blend, float opacity, uint32_t cp1, uint32_t xform, uint32_t clip)
{
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_LAYER_BEGIN;
    it.sub = EMBI_LS_COMPOSITE;
    it.flags = blend == EMBI_BM_NORMAL && cp1 == 0 ? EMBI_DF_ISOLATE : 0;
    it.f[0] = opacity;
    it.xform = xform;
    it.clip = clip;
    it.colors[0] = blend;
    it.colors[1] = cp1;
    embi_vec_push(&c->dl->items, it);
    return c->dl->items.len - 1;
}

static void svgfx_group_end(svg_ctx *c, uint32_t begin)
{
    embi_dl *d = c->dl;
    svg_em_group_end(c, begin);
    if (d->items.len > begin) {
        d->fx_items++;
    }
}

/* The reference box of a CSS clip-path of an SVG element without a layout box (CSS Masking 1 section 5.1): content-box and
 * padding-box are the fill box, border-box and margin-box the stroke box, view-box the nearest viewport. bb is the object
 * bounding box of the element. */
static bool svgfx_ref_box(const svg_ctx *c, const embi_style *st, uint16_t kw, const float *bb, float out[4])
{
    if (kw == EMBI_KW_view_box) {
        out[0] = 0;
        out[1] = 0;
        out[2] = c->vpw;
        out[3] = c->vph;
        return true;
    }
    if (bb == nullptr) {
        return false;
    }
    memcpy(out, bb, 4 * sizeof(float));
    bool stroke_box = kw == 0 || kw == EMBI_KW_border_box || kw == EMBI_KW_margin_box || kw == EMBI_KW_stroke_box;
    if (stroke_box) {
        const embi_value *sv = embi_style_value(st, EMBI_PROP_stroke);
        float w = svg_stroke_width_of(c, st);
        if (sv != nullptr && !(sv->kind == EMBI_V_KEYWORD && sv->kw == EMBI_KW_none) && w > 0) {
            out[0] -= w * 0.5f;
            out[1] -= w * 0.5f;
            out[2] += w;
            out[3] += w;
        }
    }
    return true;
}

/* Resolves the clip-path shape or box v of an element into dl->cpaths; returns 1 + its index, or 0 when there is none to
 * apply: *empty is then true when the clip leaves nothing (the element paints nothing), false when the shape is ignored. */
static uint32_t svgfx_clip_record(svg_ctx *c, const embi_style *st, const embi_value *v, const float *bb, bool *empty)
{
    *empty = false;
    float ref[4];
    if (!svgfx_ref_box(c, st, v->kw, bb, ref)) {
        *empty = true;
        return 0;
    }
    embi_dl *d = c->dl;
    embi_dcpath rec;
    memset(&rec, 0, sizeof rec);
    if (v->shape == nullptr) {
        if (!(ref[2] > 0.0f) || !(ref[3] > 0.0f)) {
            *empty = true;
            return 0;
        }
        rec.kind = EMBI_CP_RRECT;
        memcpy(rec.rect, ref, sizeof rec.rect);
        rec.bounds[0] = ref[0];
        rec.bounds[1] = ref[1];
        rec.bounds[2] = ref[0] + ref[2];
        rec.bounds[3] = ref[1] + ref[3];
    } else {
        embi_lay_curves cv;
        memset(&cv, 0, sizeof cv);
        embi_lay_cp cp;
        if (!embi_lay_clip_shape(v->shape, ref, svg_font_px(st), c->rvw, c->rvh, 16.0f, &cp, &cv)) {
            embi_vec_free(&cv);
            return 0; /* a shape that cannot be resolved is ignored, as for an html box */
        }
        rec.kind = cp.kind;
        rec.evenodd = cp.evenodd;
        memcpy(rec.rect, cp.rect, sizeof rec.rect);
        memcpy(rec.radii, cp.radii, sizeof rec.radii);
        memcpy(rec.bounds, cp.bounds, sizeof rec.bounds);
        if (cp.kind == EMBI_LCP_PATH) {
            rec.first = d->curves.len;
            rec.n = cp.n;
            static_assert(sizeof(embi_lay_curve) == sizeof(embi_dcurve), "the curve layouts match");
            for (uint32_t i = 0; i < cp.n; i++) {
                embi_dcurve cc;
                memcpy(&cc, cv.data[cp.first + i].c, sizeof cc);
                embi_vec_push(&d->curves, cc);
            }
        }
        embi_vec_free(&cv);
    }
    uint32_t idx = d->cpaths.len;
    embi_vec_push(&d->cpaths, rec);
    return idx + 1;
}

/* True when el sits in a tree that is not rendered: an ancestor with display: none, other than the elements whose UA style
 * is display: none (defs, mask, clipPath, ...). A mask or clip path in such a tree is disabled (CSS Masking 1 section 6). */
static bool svgfx_hidden_tree(const embi_element *el)
{
    for (const embi_node *n = el->base.parent; n != nullptr && embi_is_element(n); n = n->parent) {
        const embi_element *e = (const embi_element *)n;
        uint8_t t = svg_tag(e);
        if (e->base.extra == EMBI_NS_SVG && (t == TG_DEFS || t == TG_SYMBOL || t == TG_MASK || t == TG_CLIPPATH || t == TG_MARKER ||
                                             t == TG_PATTERN || t == TG_LGRAD || t == TG_RGRAD)) {
            continue;
        }
        const embi_style *st = (const embi_style *)e->style;
        if (st != nullptr && svg_kw(st, EMBI_PROP_display) == EMBI_KW_none) {
            return true;
        }
    }
    return false;
}

/* Whether the property prop of st names a fragment reference, url(#...): the mask of an element that names one that does not
 * resolve is transparent black (CSS Masking 1 section 6.1) and the element is not rendered. */
static bool svgfx_names_ref(const embi_style *st, embi_prop_id prop)
{
    embi_sb sb = {0};
    bool r = false;
    if (st != nullptr && embi_style_value_text(st, prop, &sb) && sb.len >= 6 && sb.data != nullptr &&
        memcmp(sb.data, "url(", 4) == 0) {
        embi_str id;
        r = svg_ref_id((embi_str){sb.data, sb.len}, &id);
    }
    embi_sb_free(&sb);
    return r;
}

/* ------------------------------------------------------------------ nodes */

static bool switch_ok(const embi_element *el)
{
    if (embi_el_has_attribute((embi_element *)el, EMBI_STR("requiredExtensions"))) {
        return false;
    }
    embi_sb sb = {0};
    bool ok = true;
    if (svg_attr_sb(el, "systemLanguage", &sb)) {
        ok = false;
        const char *p = sb.data != nullptr ? sb.data : "", *end = p + sb.len;
        while (p < end) {
            p = svg_skip_wsp_comma(p, end);
            const char *s = p;
            while (p < end && *p != ',' && *p != ' ') {
                p++;
            }
            size_t n = (size_t)(p - s);
            if (n >= 2 && SDL_strncasecmp(s, "en", 2) == 0 && (n == 2 || s[2] == '-')) {
                ok = true;
            }
        }
    }
    embi_sb_free(&sb);
    return ok;
}

static void walk_children(svg_ctx *c, embi_element *parent, const embi_style *pst, nctx nc, const float M[6], uint32_t clip)
{
    bool sw = svg_tag(parent) == TG_SWITCH;
    for (embi_node *n = parent->base.first_child; n != nullptr; n = n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *ch = embi_as_element(n);
        uint8_t t = svg_tag(ch);
        if (t == TG_NONE || t >= TG_LGRAD) {
            continue;
        }
        if (sw && !switch_ok(ch)) {
            continue;
        }
        walk_node(c, ch, pst, nc, M, clip, false);
        if (sw) {
            break;
        }
    }
}

/* Draws the markers of a shape: at the first vertex, every other one, and the last (SVG 2 section 11.6.2). */
/* True when everything below el is plain unstroked shapes and groups: the geometry box is then the whole painted area. */
static bool content_unstroked(svg_ctx *c, embi_element *el, int depth)
{
    if (depth > 8) {
        return false;
    }
    for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *ch = embi_as_element(n);
        uint8_t t = svg_tag(ch);
        if (t == TG_NONE || t >= TG_LGRAD) {
            continue;
        }
        const embi_style *cs = (const embi_style *)ch->style;
        if (cs == nullptr) {
            return false;
        }
        if (t == TG_G) {
            if (!content_unstroked(c, ch, depth + 1)) {
                return false;
            }
        } else if (t >= TG_PATH && t <= TG_POLYGON) {
            if (svg_style_paint(c, ch, cs, true).kind != SP_NONE) {
                return false;
            }
        } else if (t != TG_DEFS) {
            return false;
        }
    }
    return true;
}

/* The marker's content lies inside its viewport (0, 0, mw, mh): no clip is needed. vm maps the content to the viewport. */
static bool marker_fits(svg_ctx *c, embi_element *m, const float vm[6], float mw, float mh)
{
    float b[4], inv[6];
    if (!content_unstroked(c, m, 0)) {
        return false;
    }
    if (!svg_el_bbox(c, m, (const embi_style *)m->style, b)) {
        return true; /* nothing drawn */
    }
    if (!m_invert(vm, inv)) {
        return false;
    }
    float vp[4] = {0, 0, mw, mh}, r[4];
    m_map_rect(inv, vp, r);
    float eps = 1e-4f * fmaxf(fmaxf(r[2], r[3]), 1e-3f);
    return b[0] >= r[0] - eps && b[1] >= r[1] - eps && b[0] + b[2] <= r[0] + r[2] + eps && b[1] + b[3] <= r[1] + r[3] + eps;
}

bool svg_parse_angle(const char *s, uint32_t len, double *rad)
{
    const char *p = svg_skip_ws(s, s + len), *end = s + len;
    double d;
    if (!svg_number(&p, end, &d)) {
        return false;
    }
    const char *u = p;
    while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
        p++;
    }
    size_t n = (size_t)(p - u);
    if (svg_skip_ws(p, end) != end) {
        return false;
    }
    const double pi = 3.14159265358979323846;
    if (n == 0 || (n == 3 && SDL_strncasecmp(u, "deg", 3) == 0)) {
        *rad = d * pi / 180.0;
    } else if (n == 4 && SDL_strncasecmp(u, "grad", 4) == 0) {
        *rad = d * pi / 200.0;
    } else if (n == 3 && SDL_strncasecmp(u, "rad", 3) == 0) {
        *rad = d;
    } else if (n == 4 && SDL_strncasecmp(u, "turn", 4) == 0) {
        *rad = d * 2.0 * pi;
    } else {
        return false;
    }
    return isfinite(*rad);
}

static void emit_markers(svg_ctx *c, const embi_style *st, const svg_verts *vs, embi_element *mk[3], const float M[6], uint32_t clip)
{
    float sw = svg_stroke_width_of(c, st);
    for (uint32_t i = 0; i < vs->n; i++) {
        const svg_mvert *v = &vs->v[i];
        embi_element *m = v->seg == 0 ? mk[0] : v->seg == 2 ? mk[2] : mk[1];
        if (m == nullptr || c->depth > SVG_MAX_DEPTH) {
            continue;
        }
        const embi_style *mst = (const embi_style *)m->style;
        if (mst == nullptr) {
            continue;
        }
        /* orient */
        float ang = 0;
        embi_sb ob = {0};
        bool auto_orient = false, reverse = false;
        if (svg_attr_sb(m, "orient", &ob) && ob.len > 0) {
            if (ob.len == 4 && memcmp(ob.data, "auto", 4) == 0) {
                auto_orient = true;
            } else if (ob.len == 18 && memcmp(ob.data, "auto-start-reverse", 18) == 0) {
                auto_orient = true;
                reverse = true;
            } else {
                double d;
                if (svg_parse_angle(ob.data, ob.len, &d)) {
                    ang = (float)d;
                }
            }
        }
        embi_sb_free(&ob);
        if (auto_orient) {
            if (v->has_in && v->has_out) {
                ang = atan2f(sinf(v->ain) + sinf(v->aout), cosf(v->ain) + cosf(v->aout));
            } else if (v->has_in) {
                ang = v->ain;
            } else if (v->has_out) {
                ang = v->aout;
            }
            if (reverse && v->seg == 0) {
                ang += 3.14159265358979323846f;
            }
        }
        float mw = svg_attr_px(c, m, "markerWidth", c->vpw, 3.0f), mh = svg_attr_px(c, m, "markerHeight", c->vph, 3.0f);
        if (!(mw > 0) || !(mh > 0)) {
            continue;
        }
        float scale = 1.0f;
        embi_sb ub = {0};
        if (svg_attr_sb(m, "markerUnits", &ub) && ub.len == 14 && memcmp(ub.data, "userSpaceOnUse", 14) == 0) {
            scale = 1.0f;
        } else {
            scale = sw;
        }
        embi_sb_free(&ub);
        float cs = cosf(ang), sn = sinf(ang);
        float T[6] = {cs * scale, sn * scale, -sn * scale, cs * scale, v->x, v->y};
        float vb[4], vm[6];
        bool has_vb;
        svg_viewport_matrix(c, m, mw, mh, vm, vb, &has_vb);
        float rx = svg_attr_px(c, m, "refX", has_vb ? vb[2] : mw, 0.0f), ry = svg_attr_px(c, m, "refY", has_vb ? vb[3] : mh, 0.0f);
        float rpx, rpy;
        m_apply(vm, rx, ry, &rpx, &rpy);
        float tr[6] = {1, 0, 0, 1, -rpx, -rpy};
        float vp[6], mc[6], tm[6];
        m_mul(M, T, tm);
        m_mul(tm, tr, vp); /* the marker's viewport space on the page */
        m_mul(vp, vm, mc);
        bool oflow = svg_kw(mst, EMBI_PROP_overflow_x) == EMBI_KW_visible || svg_kw(mst, EMBI_PROP_overflow_x) == EMBI_KW_auto;
        /* the content is clipped to the marker viewport. A mask group clips with the same coverage as a clip-path does, so
         * content that ends exactly on the viewport edge (and is therefore not clipped at all) is not darkened twice. */
        bool use_mask = !oflow && !marker_fits(c, m, vm, mw, mh);
        float ovw = c->vpw, ovh = c->vph;
        c->vpw = has_vb ? vb[2] : mw;
        c->vph = has_vb ? vb[3] : mh;
        embi_vec_push(&c->stack, (const embi_element *)m);
        c->depth++;
        uint32_t gb = 0;
        if (use_mask) {
            gb = svg_em_group_begin(c, EMBI_LS_ISO, 1.0f, clip);
        }
        walk_children(c, m, mst, (nctx){nullptr, false}, mc, clip);
        if (use_mask) {
            uint32_t g2 = svg_em_group_begin(c, EMBI_LS_SVGMASK, 1.0f, clip);
            embi_path *rp = svgp_new(0.0f);
            (void)svgp_move(rp, 0, 0);
            (void)svgp_line(rp, mw, 0);
            (void)svgp_line(rp, mw, mh);
            (void)svgp_line(rp, 0, mh);
            (void)svgp_close(rp);
            svg_paint blk = {SP_COLOR, 0xFF000000u, nullptr, 0, false};
            float rb[4] = {0, 0, mw, mh};
            svg_emit_path(c, rp, 0, &blk, vp, rb, 1.0f, clip);
            svg_em_group_end(c, g2);
            if (c->dl->items.len > g2) {
                svg_em_group_end(c, gb);
            } else {
                c->dl->items.len = gb; /* an empty viewport clips everything away */
            }
        }
        c->depth--;
        c->stack.len--;
        c->vpw = ovw;
        c->vph = ovh;
    }
}

/* A viewport element (nested svg, symbol): the matrix of its content and the clip. */
static void viewport_children(svg_ctx *c, embi_element *el, const embi_style *st, nctx nc, const float M[6], uint32_t clip,
                              float x, float y, float w, float h)
{
    float vb[4], mc[6];
    bool has_vb;
    svg_viewport_matrix(c, el, w, h, mc, vb, &has_vb);
    float tm[6] = {1, 0, 0, 1, x, y};
    float m2[6];
    m_mul(M, tm, m2);
    uint32_t vclip = clip;
    if (c->mode == SM_PAINT && svg_kw(st, EMBI_PROP_overflow_x) != EMBI_KW_visible) {
        float r[4] = {0, 0, w, h};
        vclip = svg_em_clip(c, r, m2, clip);
    } else if (c->mode == SM_HIT && svg_kw(st, EMBI_PROP_overflow_x) != EMBI_KW_visible) {
        double q[2];
        if (!unmap(m2, c->hx, c->hy, q) || q[0] < 0 || q[1] < 0 || q[0] > w || q[1] > h) {
            return;
        }
    }
    m_mul(m2, mc, m2);
    float ovw = c->vpw, ovh = c->vph;
    c->vpw = has_vb ? vb[2] : w;
    c->vph = has_vb ? vb[3] : h;
    walk_children(c, el, st, nc, m2, vclip);
    c->vpw = ovw;
    c->vph = ovh;
}

static void do_use(svg_ctx *c, embi_element *el, const embi_style *st, nctx nc, const float M[6], uint32_t clip)
{
    embi_sb sb = {0};
    embi_element *t = nullptr;
    if (svg_href_sb(el, &sb)) {
        t = svg_ref_element(el->base.doc, (embi_str){sb.data, sb.len});
    }
    embi_sb_free(&sb);
    if (t == nullptr || t == el || !uses_ok(c, t) || embi_contains(&t->base, &el->base)) {
        return;
    }
    uint8_t tt = svg_tag(t);
    if (tt == TG_NONE || tt >= TG_LGRAD || tt == TG_DEFS || tt == TG_MARKER) {
        return;
    }
    float x = len_attr(c, el, "x", 0, 0), y = len_attr(c, el, "y", 1, 0);
    float m[6], tm[6] = {1, 0, 0, 1, x, y};
    m_mul(M, tm, m);
    embi_vec_push(&c->stack, (const embi_element *)t);
    nctx inner = {nc.use != nullptr ? nc.use : el, true};
    if (tt == TG_SYMBOL || tt == TG_SVG) {
        const embi_style *ts = svg_style_of(c, t, st);
        float w = len_attr(c, el, "width", 0, -1), h = len_attr(c, el, "height", 1, -1);
        svg_len l;
        if (w < 0) {
            w = svg_attr_len_raw(t, "width", &l) ? svg_len_px(l, 16, c->vpw) : c->vpw; /* SVG 2: a symbol has a size too */
        }
        if (h < 0) {
            h = svg_attr_len_raw(t, "height", &l) ? svg_len_px(l, 16, c->vph) : c->vph;
        }
        if (w > 0 && h > 0 && ts != nullptr) {
            float tmx[6];
            svg_el_matrix(c, t, ts, nullptr, tmx);
            float m3[6];
            m_mul(m, tmx, m3);
            float ex = 0, ey = 0;
            if (tt == TG_SVG) {
                ex = len_attr(c, t, "x", 0, 0);
                ey = len_attr(c, t, "y", 1, 0);
            }
            viewport_children(c, t, ts, inner, m3, clip, ex, ey, w, h);
        }
    } else {
        walk_node(c, t, st, inner, m, clip, true);
    }
    c->stack.len--;
}

static void walk_node(svg_ctx *c, embi_element *el, const embi_style *pst, nctx nc, const float M[6], uint32_t clip,
                      bool force_display)
{
    if (++c->nodes > SVG_MAX_NODES || c->depth > SVG_MAX_DEPTH) {
        return;
    }
    uint8_t tag = svg_tag(el);
    if (tag == TG_NONE || tag >= TG_LGRAD) {
        return;
    }
    if (tag == TG_DEFS || tag == TG_SYMBOL) {
        return; /* symbol is drawn by use only */
    }
    const embi_style *st = svg_style_of(c, el, nc.inst ? pst : nullptr);
    if (st == nullptr) {
        return;
    }
    if (svg_kw(st, EMBI_PROP_display) == EMBI_KW_none) {
        return; /* display applies to the instance as well */
    }
    (void)force_display;
    float bb[4];
    bool have_bb = false;
    bool shape = tag >= TG_PATH && tag <= TG_POLYGON;
    /* the transform */
    float lm[6];
    if (shape || tag == TG_TEXT || tag == TG_IMAGE || tag == TG_G || tag == TG_A || tag == TG_USE || tag == TG_SWITCH || tag == TG_SVG ||
        tag == TG_FOREIGN) {
        if (embi_lay_has_transform(st) && !is_default_origin(st)) {
            have_bb = svg_el_bbox(c, el, st, bb);
        }
        svg_el_matrix(c, el, st, have_bb ? bb : nullptr, lm);
    } else {
        m_identity(lm);
    }
    float M2[6];
    m_mul(M, lm, M2);

    /* group effects: opacity, clip-path, mask */
    float opacity = opacity_of(st, EMBI_PROP_opacity);
    embi_sb csb = {0}, msb = {0};
    embi_element *cpe = nullptr, *mke = nullptr;
    bool mask_missing = false;
    if (c->mode == SM_PAINT && c->luma != 2) {
        if (ref_prop(el, st, EMBI_PROP_clip_path, "clip-path", &csb, &cpe) && (svg_tag(cpe) != TG_CLIPPATH || !uses_ok(c, cpe))) {
            cpe = nullptr;
        }
        if (ref_prop(el, st, EMBI_PROP_mask_image, "mask", &msb, &mke)) {
            if (svg_tag(mke) != TG_MASK) {
                mke = nullptr;
                mask_missing = true; /* it names something that is not a mask: a mask of transparent black */
            } else if (!uses_ok(c, mke)) {
                mke = nullptr; /* a cycle: no mask */
            } else if (svgfx_hidden_tree(mke)) {
                mke = nullptr; /* a mask in a tree that is not rendered is disabled */
            }
        } else {
            mask_missing = svgfx_names_ref(st, EMBI_PROP_mask_image);
        }
    }
    embi_sb_free(&csb);
    embi_sb_free(&msb);
    if (mask_missing) {
        return; /* a mask reference that does not resolve masks everything away */
    }
    /* R4e: mix-blend-mode, isolation and a clip-path shape or box (CSS Masking 1) are one composite group around the
     * element (a foreignObject gets them from its box, paint.c). The element is an isolated group when it says so and a
     * descendant blends; a blend mode needs a group of its own. */
    uint32_t blend = EMBI_BM_NORMAL, cp1 = 0;
    bool iso = false, css_mask = false;
    float mbox[4];
    if (c->mode == SM_PAINT && c->luma == 0 && tag != TG_FOREIGN) {
        blend = svgfx_blend(st);
        if (blend == EMBI_BM_NORMAL && svg_kw(st, EMBI_PROP_isolation) == EMBI_KW_isolate &&
            (tag == TG_G || tag == TG_A || tag == TG_SWITCH || tag == TG_SVG || tag == TG_USE)) {
            uint32_t budget = 2000;
            iso = svgfx_blend_below(el, 0, &budget);
        }
        const embi_value *cv = embi_style_value(st, EMBI_PROP_clip_path);
        if (cv != nullptr && cv->kind == EMBI_V_SHAPE) {
            float b2[4];
            bool hb = have_bb;
            if (!hb && cv->kw != EMBI_KW_view_box) {
                hb = svg_el_bbox(c, el, st, b2);
            }
            bool empty = false;
            cp1 = svgfx_clip_record(c, st, cv, hb ? (have_bb ? bb : b2) : nullptr, &empty);
            if (empty) {
                return; /* a clip with no area clips everything away */
            }
        }
        /* a mask-image of images or gradients: the layers of the html machinery, over the stroke box */
        const embi_value *mv = embi_style_value(st, EMBI_PROP_mask_image);
        if (c->env != nullptr && c->env->mask_layers != nullptr && mv != nullptr && !(mv->kind == EMBI_V_KEYWORD && mv->kw == EMBI_KW_none) &&
            !svgfx_names_ref(st, EMBI_PROP_mask_image)) {
            float b3[4];
            bool hb = have_bb || svg_el_bbox(c, el, st, b3);
            css_mask = hb && svgfx_ref_box(c, st, 0, have_bb ? bb : b3, mbox);
        }
    }
    if (opacity <= 0.0f && cpe == nullptr && mke == nullptr && c->mode == SM_PAINT) {
        return;
    }
    uint32_t wbegin = 0;
    bool wrapped = false;
    if (c->mode == SM_PAINT && (blend != EMBI_BM_NORMAL || iso || cp1 != 0 || css_mask)) {
        bool fold = cpe == nullptr && mke == nullptr; /* the opacity goes into the composite group */
        uint32_t wxf = svg_em_xform(c, M2);
        wbegin = svgfx_group_begin(c, blend, fold ? opacity : 1.0f, cp1, wxf, clip);
        wrapped = true;
        if (css_mask) {
            c->dl->items.data[wbegin].colors[2] = c->env->mask_layers(c->env->host, el, st, mbox, wxf, clip);
        }
        if (fold) {
            opacity = 1.0f;
        }
    }
    uint32_t gbegin = 0;
    bool grouped = false;
    if (c->mode == SM_PAINT && (opacity < 1.0f || cpe != nullptr || mke != nullptr)) {
        gbegin = svg_em_group_begin(c, cpe != nullptr || mke != nullptr ? EMBI_LS_ISO : 0, opacity, clip);
        grouped = true;
    }

    const embi_element *saved_el = c->cur_el;
    const embi_style *saved_st = c->cur_st;
    c->cur_el = el;
    c->cur_st = st;
    c->depth++;
    switch (tag) {
    case TG_G: case TG_A: case TG_SWITCH:
        walk_children(c, el, st, nc, M2, clip);
        break;
    case TG_SVG: {
        float w = len_attr(c, el, "width", 0, c->vpw), h = len_attr(c, el, "height", 1, c->vph);
        float x = len_attr(c, el, "x", 0, 0), y = len_attr(c, el, "y", 1, 0);
        if (w > 0 && h > 0) {
            viewport_children(c, el, st, nc, M2, clip, x, y, w, h);
        }
        break;
    }
    case TG_USE:
        do_use(c, el, st, nc, M2, clip);
        break;
    case TG_IMAGE:
        do_image(c, el, st, nc, M2, clip);
        break;
    case TG_FOREIGN:
        do_foreign(c, el, st, nc, M2, clip);
        break;
    case TG_TEXT: {
        double q[2] = {0, 0};
        bool hit = false;
        if (c->mode == SM_HIT && (svg_kw(st, EMBI_PROP_pointer_events) == EMBI_KW_none || !unmap(M2, c->hx, c->hy, q))) {
            break;
        }
        svg_emit_text(c, el, st, M2, clip, &hit, q);
        if (hit) {
            c->hit = nc.use != nullptr ? nc.use : el;
        }
        break;
    }
    default:
        if (shape) {
            do_shape(c, el, st, tag, nc, M2, clip);
        }
        break;
    }
    c->depth--;
    c->cur_el = saved_el;
    c->cur_st = saved_st;

    if (grouped) {
        bool dropped = false;
        if (cpe != nullptr || mke != nullptr) {
            float b2[4];
            bool need = (cpe != nullptr && clip_needs_bbox(cpe));
            if (mke != nullptr) {
                bool ro, co;
                need = mask_units(mke, &ro, &co) || need;
            }
            bool hb = have_bb;
            if (!hb && need) {
                hb = svg_el_bbox(c, el, st, b2);
            }
            const float *bp = have_bb ? bb : b2;
            if (cpe != nullptr) {
                embi_vec_push(&c->stack, (const embi_element *)cpe);
                dropped = !emit_clip_path(c, cpe, M2, bp, hb, clip);
                c->stack.len--;
            }
            if (mke != nullptr && !dropped) {
                embi_vec_push(&c->stack, (const embi_element *)mke);
                dropped = !emit_mask(c, mke, M2, bp, hb, clip);
                c->stack.len--;
            }
        }
        if (dropped) {
            c->dl->items.len = gbegin; /* a clip or mask that lets nothing through: the element is not rendered */
        } else {
            svg_em_group_end(c, gbegin);
        }
    }
    if (wrapped) {
        svgfx_group_end(c, wbegin);
    }
}

void svg_walk_root(svg_ctx *c, embi_element *svg, float vw, float vh)
{
    const embi_style *st = (const embi_style *)svg->style;
    c->vpw = vw;
    c->vph = vh;
    float m[6];
    m_identity(m);
    uint32_t clip = 0;
    if (c->mode == SM_PAINT) {
        uint16_t ov = svg_kw(st, EMBI_PROP_overflow_x);
        if (c->root_clip || ov != EMBI_KW_visible) { /* auto clips too (svgwg#1072) */
            float r[4] = {0, 0, vw, vh};
            clip = svg_em_clip(c, r, m, c->clip0);
        } else {
            clip = c->clip0;
        }
    }
    float vb[4], mc[6];
    bool has_vb;
    svg_viewport_matrix(c, svg, vw, vh, mc, vb, &has_vb);
    if (has_vb) {
        c->vpw = vb[2];
        c->vph = vb[3];
    }
    if (c->mode == SM_HIT) {
        double q[2];
        if (!unmap(mc, c->hx, c->hy, q)) {
            return;
        }
    }
    walk_children(c, svg, st, (nctx){nullptr, false}, mc, clip);
}

void svg_walk_children(svg_ctx *c, embi_element *parent, const float M[6], uint32_t clip)
{
    if (c->depth > SVG_MAX_DEPTH || !uses_ok(c, parent)) {
        return;
    }
    embi_vec_push(&c->stack, (const embi_element *)parent);
    c->depth++;
    walk_children(c, parent, (const embi_style *)parent->style, (nctx){nullptr, false}, M, clip);
    c->depth--;
    c->stack.len--;
}

int embi_svg_ref_content(const embi_svg_env *env, embi_dl *dl, const embi_document *doc, embi_str url, bool clip_path,
                         const float bbox[4], uint32_t xform, uint32_t clip)
{
    if (dl == nullptr || doc == nullptr || !(bbox[2] > 0) || !(bbox[3] > 0)) {
        return EMBI_SVGFX_REF_NONE;
    }
    embi_element *el = svg_ref_element(doc, url);
    if (el == nullptr || svg_tag(el) != (clip_path ? TG_CLIPPATH : TG_MASK)) {
        return EMBI_SVGFX_REF_NONE;
    }
    if (svgfx_hidden_tree(el)) {
        return EMBI_SVGFX_REF_OFF;
    }
    svg_ctx c;
    svg_ctx_init(&c, env, SM_PAINT);
    c.dl = dl;
    c.clip0 = clip;
    c.doc = (embi_document *)doc;
    c.vpw = bbox[2];
    c.vph = bbox[3];
    float t[6] = {1, 0, 0, 1, bbox[0], bbox[1]};
    if (xform > 0 && xform < dl->xforms.len) {
        m_mul(dl->xforms.data[xform].m, t, c.base);
    } else {
        memcpy(c.base, t, sizeof t);
    }
    int r = EMBI_SVGFX_REF_NONE;
    if (uses_ok(&c, el)) {
        float lb[4] = {0, 0, bbox[2], bbox[3]}, m[6];
        m_identity(m);
        embi_vec_push(&c.stack, (const embi_element *)el);
        bool ok = clip_path ? emit_clip_content(&c, el, m, lb, true, clip) : emit_mask_content(&c, el, m, lb, true, clip);
        c.stack.len--;
        r = ok ? EMBI_SVGFX_REF_OK : EMBI_SVGFX_REF_NONE;
    }
    svg_ctx_free(&c);
    return r;
}
