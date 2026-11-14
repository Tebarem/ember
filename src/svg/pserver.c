/*
 * Paints of the SVG module (R4c): the fill and stroke properties, and the paint servers (linear and radial gradients) as
 * EMBI_DI_PATH items. A gradient is drawn by mapping the path into the gradient's own space (the inverse of the gradient
 * matrix) and giving the item the matrix, so the renderer's circular gradient geometry covers every gradientTransform.
 * Patterns (emit_pattern) draw one tile and copy its items to the others. The focal point of radial gradients is not yet.
 */
#include "svg/svg_int.h"

#include "paint/dl.h"

#define MAX_HREF 8
#define MAX_STOPS 64

static uint8_t tag_of(const embi_element *e)
{
    return svg_tag(e);
}

svg_paint svg_style_paint(svg_ctx *c, embi_element *el, const embi_style *st, bool stroke)
{
    (void)c;
    svg_paint r;
    memset(&r, 0, sizeof r);
    const embi_value *v = embi_style_value(st, stroke ? EMBI_PROP_stroke : EMBI_PROP_fill);
    if (v == nullptr) {
        return r;
    }
    uint32_t rgba;
    if (v->kind == EMBI_V_KEYWORD) {
        return r; /* none; context-fill and context-stroke are not yet */
    }
    if (v->kind == EMBI_V_COLOR || v->kind == EMBI_V_LIGHT_DARK) {
        if (svg_color(st, v, &rgba)) {
            r.kind = SP_COLOR;
            r.rgba = rgba;
        }
        return r;
    }
    const embi_value *url = nullptr, *fb = nullptr;
    if (v->kind == EMBI_V_URL) {
        url = v;
    } else if (v->kind == EMBI_V_LIST && v->list.n == 2 && v->list.items[0].kind == EMBI_V_URL) {
        url = &v->list.items[0];
        fb = &v->list.items[1];
    }
    if (url == nullptr) {
        return r;
    }
    if (fb != nullptr && fb->kind != EMBI_V_KEYWORD && svg_color(st, fb, &rgba)) {
        r.has_fb = true;
        r.fb_rgba = rgba;
    }
    embi_element *srv = svg_ref_element(el->base.doc, url->str);
    uint8_t t = srv != nullptr ? tag_of(srv) : TG_NONE;
    if (t == TG_LGRAD || t == TG_RGRAD || t == TG_PATTERN) {
        r.kind = SP_SERVER;
        r.server = srv;
    } else if (r.has_fb) {
        r.kind = SP_COLOR;
        r.rgba = r.fb_rgba;
    }
    return r;
}

uint32_t svg_luma_color(const svg_ctx *c, uint32_t rgba)
{
    uint32_t a = rgba >> 24;
    if (c->luma == 2) {
        return 0xFF000000u;
    }
    if (c->luma == 1) {
        float r = (float)(rgba & 0xFF), g = (float)((rgba >> 8) & 0xFF), b = (float)((rgba >> 16) & 0xFF);
        float l = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
        uint32_t na = (uint32_t)(l * (float)a + 0.5f);
        return na << 24;
    }
    if (c->luma == 3) {
        return a << 24;
    }
    return rgba;
}

static uint32_t with_alpha(uint32_t rgba, float k)
{
    float a = (float)(rgba >> 24) * k;
    uint32_t ia = a <= 0 ? 0 : a >= 255 ? 255 : (uint32_t)(a + 0.5f);
    return (rgba & 0x00FFFFFFu) | (ia << 24);
}

static void push_path_item(svg_ctx *c, embi_path *path, int rule, uint32_t xf, uint32_t clip, uint32_t color, uint8_t grad,
                           const float geo[4], uint32_t stop0, uint32_t nstops, bool repeat)
{
    float b[4];
    if (!svgp_bbox(path, b)) {
        svgp_free(path);
        return;
    }
    embi_dl *d = c->dl;
    uint32_t pi = embi_dl_add_path(d, (struct embi_path *)path, svgp_hash(path));
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_PATH;
    it.sub = (uint8_t)rule;
    it.xform = xf;
    it.clip = clip;
    memcpy(it.rect, b, sizeof b);
    it.colors[0] = color;
    it.aux = pi;
    if (grad != 0) {
        it.styles[0] = grad;
        memcpy(it.f, geo, 4 * sizeof(float));
        it.aux2 = stop0;
        it.aux3 = nstops;
        it.flags = repeat ? EMBI_DF_REPEAT : 0;
    }
    embi_vec_push(&d->items, it);
}

/* A gradient attribute looked up along the href chain. same_kind: only elements of the same gradient kind have it. */
static bool grad_attr(embi_element *g, const char *name, bool same_kind, embi_sb *sb)
{
    uint8_t kind = tag_of(g);
    embi_element *e = g;
    for (int i = 0; i < MAX_HREF && e != nullptr; i++) {
        uint8_t t = tag_of(e);
        if (t != TG_LGRAD && t != TG_RGRAD) {
            break;
        }
        if ((!same_kind || t == kind) && svg_attr_sb(e, name, sb)) {
            /* an empty or blank value counts as not specified, so the inherited value shows through */
            bool blank = true;
            for (size_t k = 0; k < sb->len; k++) {
                char ch = sb->data[k];
                if (ch != ' ' && ch != 9 && ch != 10 && ch != 13 && ch != 12) {
                    blank = false;
                    break;
                }
            }
            if (!blank) {
                return true;
            }
            sb->len = 0;
        }
        embi_sb h = {0};
        embi_element *next = nullptr;
        if (svg_href_sb(e, &h)) {
            next = svg_ref_element(e->base.doc, (embi_str){h.data, h.len});
        }
        embi_sb_free(&h);
        e = next;
    }
    return false;
}

static bool is_stop(const embi_element *e)
{
    embi_str n = embi_atom_str(e->base.name);
    return e->base.extra == EMBI_NS_SVG && n.len == 4 && memcmp(n.ptr, "stop", 4) == 0;
}

/* The stops of the first element along the href chain that has any. Returns the count. */
static uint32_t gather_stops(svg_ctx *c, embi_element *g, embi_dstop *out, float k)
{
    embi_element *e = g;
    for (int i = 0; i < MAX_HREF && e != nullptr; i++) {
        uint8_t t = tag_of(e);
        if (t != TG_LGRAD && t != TG_RGRAD) {
            break;
        }
        uint32_t n = 0;
        float last = 0;
        for (embi_node *nd = e->base.first_child; nd != nullptr && n < MAX_STOPS; nd = nd->next) {
            if (!embi_is_element(nd)) {
                continue;
            }
            embi_element *s = embi_as_element(nd);
            if (!is_stop(s)) {
                continue;
            }
            const embi_style *ss = (const embi_style *)s->style;
            float off = 0;
            embi_sb sb = {0};
            if (svg_attr_sb(s, "offset", &sb)) {
                svg_len l;
                if (svg_parse_length(sb.data != nullptr ? sb.data : "", sb.len, &l)) {
                    off = l.unit == SVGL_PCT ? l.v / 100.0f : l.v;
                }
            }
            embi_sb_free(&sb);
            off = off < 0 ? 0 : off > 1 ? 1 : off;
            if (off < last) {
                off = last;
            }
            last = off;
            uint32_t rgba = 0xFF000000u;
            if (ss != nullptr) {
                uint32_t col;
                if (svg_color(ss, embi_style_value(ss, EMBI_PROP_stop_color), &col)) {
                    rgba = col;
                }
                rgba = with_alpha(rgba, (float)svg_num(ss, EMBI_PROP_stop_opacity, 1.0));
            }
            out[n].offset = off;
            out[n].color = svg_luma_color(c, with_alpha(rgba, k));
            n++;
        }
        if (n > 0) {
            return n;
        }
        embi_sb h = {0};
        embi_element *next = nullptr;
        if (svg_href_sb(e, &h)) {
            next = svg_ref_element(e->base.doc, (embi_str){h.data, h.len});
        }
        embi_sb_free(&h);
        e = next;
    }
    return 0;
}

static float unit_val(const svg_len *l, bool obb, float pct_base, float fs)
{
    if (obb) {
        /* absolute units resolve to their CSS pixel value, then are used as the bounding-box fraction (SVG 2 pservers) */
        return l->unit == SVGL_PCT ? l->v / 100.0f : svg_len_px(*l, fs, 0.0f);
    }
    return svg_len_px(*l, fs, pct_base);
}

static float g_len(embi_element *g, const char *name, bool obb, float pct_base, float def_frac)
{
    embi_sb sb = {0};
    float r = obb ? def_frac : def_frac * pct_base;
    if (grad_attr(g, name, true, &sb)) {
        svg_len l;
        if (svg_parse_length(sb.data != nullptr ? sb.data : "", sb.len, &l)) {
            r = unit_val(&l, obb, pct_base, 16.0f);
        }
    }
    embi_sb_free(&sb);
    return r;
}

static void emit_gradient(svg_ctx *c, embi_path *path, int rule, embi_element *g, const float m[6], const float bbox[4],
                          float opacity, uint32_t clip)
{
    bool obb = true;
    embi_sb sb = {0};
    if (grad_attr(g, "gradientUnits", false, &sb) && sb.len == 14 && memcmp(sb.data, "userSpaceOnUse", 14) == 0) {
        obb = false;
    }
    embi_sb_free(&sb);
    if (obb && (!(bbox[2] > 0) || !(bbox[3] > 0))) {
        svgp_free(path);
        return;
    }
    embi_dstop stops[2 * MAX_STOPS + 2], tmp[MAX_STOPS]; /* reflect doubles the stops, padding adds two */
    uint32_t n = gather_stops(c, g, tmp, opacity);
    if (n == 0) {
        svgp_free(path);
        return;
    }
    float tol = 0.2f / (m_scale(m) * c->dev > 1e-6f ? m_scale(m) * c->dev : 1e-6f);
    if (n == 1) {
        push_path_item(c, path, rule, svg_em_xform(c, m), clip, tmp[0].color, 0, nullptr, 0, 0, false);
        return;
    }
    /* the gradient matrix */
    float G[6];
    m_identity(G);
    if (obb) {
        G[0] = bbox[2];
        G[3] = bbox[3];
        G[4] = bbox[0];
        G[5] = bbox[1];
    }
    embi_sb ts = {0};
    if (grad_attr(g, "gradientTransform", false, &ts) && ts.len > 0) {
        float gt[6];
        if (svg_parse_transform(ts.data, ts.len, gt)) {
            m_mul(G, gt, G);
        }
    }
    embi_sb_free(&ts);
    float Ginv[6];
    if (!m_invert(G, Ginv)) {
        svgp_free(path);
        return;
    }
    uint8_t kind;
    float geo[4];
    if (tag_of(g) == TG_LGRAD) {
        kind = EMBI_GRAD_LINEAR_DL;
        geo[0] = g_len(g, "x1", obb, c->vpw, 0.0f);
        geo[1] = g_len(g, "y1", obb, c->vph, 0.0f);
        geo[2] = g_len(g, "x2", obb, c->vpw, 1.0f);
        geo[3] = g_len(g, "y2", obb, c->vph, 0.0f);
        if (geo[0] == geo[2] && geo[1] == geo[3]) {
            /* a zero-length vector paints the last stop */
            push_path_item(c, path, rule, svg_em_xform(c, m), clip, tmp[n - 1].color, 0, nullptr, 0, 0, false);
            return;
        }
    } else {
        kind = EMBI_GRAD_RADIAL_DL;
        float diag = sqrtf((c->vpw * c->vpw + c->vph * c->vph) * 0.5f);
        geo[0] = g_len(g, "cx", obb, c->vpw, 0.5f);
        geo[1] = g_len(g, "cy", obb, c->vph, 0.5f);
        float r = g_len(g, "r", obb, diag, 0.5f);
        if (!(r > 0)) {
            push_path_item(c, path, rule, svg_em_xform(c, m), clip, tmp[n - 1].color, 0, nullptr, 0, 0, false);
            return;
        }
        geo[2] = geo[3] = r;
    }
    /* spread */
    int spread = 0; /* 0 pad, 1 reflect, 2 repeat */
    embi_sb ss = {0};
    if (grad_attr(g, "spreadMethod", false, &ss)) {
        if (ss.len == 7 && memcmp(ss.data, "reflect", 7) == 0) {
            spread = 1;
        } else if (ss.len == 6 && memcmp(ss.data, "repeat", 6) == 0) {
            spread = 2;
        }
    }
    embi_sb_free(&ss);
    uint32_t m_n = 0;
    if (spread == 1) {
        /* reflect: stops over 0..2, the geometry's vector doubled */
        if (kind == EMBI_GRAD_LINEAR_DL) {
            geo[2] = geo[0] + 2 * (geo[2] - geo[0]);
            geo[3] = geo[1] + 2 * (geo[3] - geo[1]);
        } else {
            geo[2] *= 2;
            geo[3] *= 2;
        }
        for (uint32_t i = 0; i < n; i++) {
            stops[m_n].offset = tmp[i].offset * 0.5f;
            stops[m_n++].color = tmp[i].color;
        }
        for (uint32_t i = n; i-- > 0;) {
            stops[m_n].offset = 1.0f - tmp[i].offset * 0.5f;
            stops[m_n++].color = tmp[i].color;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            stops[m_n++] = tmp[i];
        }
    }
    if (stops[0].offset > 0) {
        memmove(stops + 1, stops, m_n * sizeof stops[0]);
        stops[0].offset = 0;
        m_n++;
    }
    if (stops[m_n - 1].offset < 1.0f) {
        stops[m_n] = stops[m_n - 1];
        stops[m_n].offset = 1.0f;
        m_n++;
    }
    embi_dl *d = c->dl;
    uint32_t s0 = d->stops.len;
    for (uint32_t i = 0; i < m_n; i++) {
        embi_vec_push(&d->stops, stops[i]);
    }
    embi_path *gp = svgp_xform(path, Ginv, tol);
    svgp_free(path);
    float mg[6];
    m_mul(m, G, mg);
    push_path_item(c, gp, rule, svg_em_xform(c, mg), clip, 0, kind, geo, s0, m_n, spread != 0);
}

static void stroke_item(svg_ctx *c, uint32_t pi, const float b0[4], const svg_stroke *st, uint32_t rgba, const float m[6], uint32_t clip);

void svg_emit_stroke_lazy(svg_ctx *c, embi_path *path, const svg_stroke *st, uint32_t rgba, const float m[6], uint32_t clip)
{
    float b[4];
    if (c->dl == nullptr || (rgba >> 24) == 0 || !svgp_bbox(path, b)) {
        svgp_free(path);
        return;
    }
    stroke_item(c, embi_dl_add_path(c->dl, (struct embi_path *)path, svgp_hash(path)), b, st, rgba, m, clip);
}

/* A solid fill and a plain solid stroke of one path: the two items share the path (one reference each). */
void svg_emit_fill_stroke_shared(svg_ctx *c, embi_path *path, int rule, uint32_t fill_rgba, float fop, const svg_stroke *st,
                                 uint32_t stroke_rgba, bool stroke_first, const float m[6], uint32_t clip)
{
    uint32_t col = svg_luma_color(c, with_alpha(fill_rgba, fop));
    float b[4];
    if ((col >> 24) == 0 || c->dl == nullptr || !svgp_bbox(path, b)) {
        svg_emit_stroke_lazy(c, path, st, stroke_rgba, m, clip);
        return;
    }
    embi_dl *d = c->dl;
    uint32_t pi = embi_dl_add_path(d, (struct embi_path *)path, svgp_hash(path));
    if (stroke_first) {
        stroke_item(c, embi_dl_share_path(d, pi), b, st, stroke_rgba, m, clip);
    }
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_PATH;
    it.sub = (uint8_t)rule;
    it.xform = svg_em_xform(c, m);
    it.clip = clip;
    memcpy(it.rect, b, sizeof b);
    it.colors[0] = col;
    it.aux = pi;
    embi_vec_push(&d->items, it);
    if (!stroke_first) {
        stroke_item(c, embi_dl_share_path(d, pi), b, st, stroke_rgba, m, clip);
    }
}

static void stroke_item(svg_ctx *c, uint32_t pi, const float b0[4], const svg_stroke *st, uint32_t rgba, const float m[6], uint32_t clip)
{
    if ((rgba >> 24) == 0) {
        return;
    }
    float b[4] = {b0[0], b0[1], b0[2], b0[3]};
    float grow = 0.5f * st->width * (st->join == 0 ? fmaxf(st->miter_limit, 1.0f) : 1.0f) * (st->cap == 2 ? 1.5f : 1.0f);
    b[0] -= grow;
    b[1] -= grow;
    b[2] += 2 * grow;
    b[3] += 2 * grow;
    embi_dl *d = c->dl;
    embi_ditem it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_DI_PATH;
    it.flags = EMBI_DF_STROKE;
    it.xform = svg_em_xform(c, m);
    it.clip = clip;
    memcpy(it.rect, b, sizeof b);
    it.colors[0] = rgba;
    it.aux = pi;
    it.f[0] = st->width;
    it.f[1] = st->miter_limit;
    it.styles[1] = st->cap;
    it.styles[2] = st->join;
    embi_vec_push(&d->items, it);
}

/* ------------------------------------------------------------------ patterns */

static bool pat_attr(embi_element *p, const char *name, embi_sb *sb)
{
    embi_element *e = p;
    for (int i = 0; i < MAX_HREF && e != nullptr && tag_of(e) == TG_PATTERN; i++) {
        if (svg_attr_sb(e, name, sb)) {
            return true;
        }
        embi_sb h = {0};
        embi_element *next = nullptr;
        if (svg_href_sb(e, &h)) {
            next = svg_ref_element(e->base.doc, (embi_str){h.data, h.len});
        }
        embi_sb_free(&h);
        e = next;
    }
    return false;
}

/* The pattern along the href chain that has child elements (the content). */
static embi_element *pat_content(embi_element *p)
{
    embi_element *e = p;
    for (int i = 0; i < MAX_HREF && e != nullptr && tag_of(e) == TG_PATTERN; i++) {
        for (embi_node *n = e->base.first_child; n != nullptr; n = n->next) {
            if (embi_is_element(n)) {
                return e;
            }
        }
        embi_sb h = {0};
        embi_element *next = nullptr;
        if (svg_href_sb(e, &h)) {
            next = svg_ref_element(e->base.doc, (embi_str){h.data, h.len});
        }
        embi_sb_free(&h);
        e = next;
    }
    return nullptr;
}

static bool pat_is(embi_element *p, const char *name, const char *value)
{
    embi_sb sb = {0};
    bool r = pat_attr(p, name, &sb) && sb.len == strlen(value) && memcmp(sb.data, value, sb.len) == 0;
    embi_sb_free(&sb);
    return r;
}

static float pat_len(embi_element *p, const char *name, bool obb, float base, float obb_base, float origin)
{
    embi_sb sb = {0};
    float r = 0;
    svg_len l;
    if (pat_attr(p, name, &sb) && svg_parse_length(sb.data != nullptr ? sb.data : "", sb.len, &l)) {
        r = obb ? origin + obb_base * (l.unit == SVGL_PCT ? l.v / 100.0f : l.v) : svg_len_px(l, 16.0f, base);
    } else if (obb) {
        r = origin;
    }
    embi_sb_free(&sb);
    return r;
}

/* A pattern paints at most this many tiles over the box of its shape, and copies at most this many list items. Past them
 * it paints nothing (a 10 x 10 tile over a 1000 x 1000 shape is 10000 tiles and fits; a 1 x 1 tile over a full HD page
 * does not). */
#define PAT_MAX_TILES 16384.0
#define PAT_MAX_COPIES 262144u

/* The transform of the page-space matrix X moved by the tile step (di w, dj h) of the pattern space A. */
static uint32_t pat_xform(svg_ctx *c, embi_hashmap *xm, uint32_t xf, const float D[6])
{
    uint64_t v = 0;
    if (embi_hashmap_get(xm, xf, &v)) {
        return (uint32_t)v;
    }
    embi_dl *d = c->dl;
    embi_dxform x;
    float X[6] = {1, 0, 0, 1, 0, 0};
    if (xf < d->xforms.len) {
        memcpy(X, d->xforms.data[xf].m, sizeof X);
    }
    m_mul(D, X, x.m);
    x.scroll = xf < d->xforms.len ? d->xforms.data[xf].scroll : 0;
    embi_vec_push(&d->xforms, x);
    (void)embi_hashmap_put(xm, xf, d->xforms.len - 1);
    return d->xforms.len - 1;
}

/* Copies the items [t0, t1) of the first tile (i0, j0), with the clips [c0, c1) they made, to every other tile. */
static void pat_copy_tiles(svg_ctx *c, const float A[6], const float Ainv[6], float w, float h, int i0, int i1, int j0, int j1,
                           uint32_t t0, uint32_t t1, uint32_t c0, uint32_t c1)
{
    embi_dl *d = c->dl;
    embi_hashmap xm;
    embi_hashmap_init(&xm);
    uint32_t nclips = c1 - c0;
    uint32_t *cmap = nclips > 0 ? embi_alloc((size_t)nclips * sizeof *cmap) : nullptr;
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            if (i == i0 && j == j0) {
                continue;
            }
            float T[6] = {1, 0, 0, 1, (float)(i - i0) * w, (float)(j - j0) * h}, D[6];
            m_mul(A, T, D);
            m_mul(D, Ainv, D);
            embi_hashmap_clear(&xm);
            for (uint32_t k = 0; k < nclips; k++) {
                embi_dclip cl = d->clips.data[c0 + k];
                cl.xform = pat_xform(c, &xm, cl.xform, D);
                if (cl.parent >= c0 && cl.parent < c0 + k) {
                    cl.parent = cmap[cl.parent - c0];
                }
                embi_vec_push(&d->clips, cl);
                cmap[k] = d->clips.len - 1;
            }
            uint32_t n0 = d->items.len;
            for (uint32_t k = t0; k < t1; k++) {
                embi_ditem it = d->items.data[k];
                if (it.kind == EMBI_DI_LAYER_BEGIN || it.kind == EMBI_DI_LAYER_END) {
                    it.aux = it.aux - t0 + n0;
                    if (it.xform != 0) {
                        it.xform = pat_xform(c, &xm, it.xform, D);
                    }
                } else {
                    it.xform = pat_xform(c, &xm, it.xform, D);
                }
                if (it.clip >= c0 && it.clip < c1) {
                    it.clip = cmap[it.clip - c0];
                }
                embi_vec_push(&d->items, it);
            }
        }
    }
    if (cmap != nullptr) {
        embi_free(cmap, (size_t)nclips * sizeof *cmap);
    }
    embi_hashmap_free(&xm);
}

static void emit_pattern(svg_ctx *c, embi_path *path, int rule, embi_element *pat, const float m[6], const float bbox[4],
                         float opacity, uint32_t clip)
{
    embi_element *content = pat_content(pat);
    bool obb = !pat_is(pat, "patternUnits", "userSpaceOnUse");
    bool cobb = pat_is(pat, "patternContentUnits", "objectBoundingBox");
    if (content == nullptr || (obb && (!(bbox[2] > 0) || !(bbox[3] > 0))) || c->depth > 8) {
        svgp_free(path);
        return;
    }
    float x = pat_len(pat, "x", obb, c->vpw, bbox[2], bbox[0]), y = pat_len(pat, "y", obb, c->vph, bbox[3], bbox[1]);
    float w = pat_len(pat, "width", obb, c->vpw, bbox[2], 0), h = pat_len(pat, "height", obb, c->vph, bbox[3], 0);
    if (!(w > 0) || !(h > 0)) {
        svgp_free(path);
        return;
    }
    float pt[6];
    m_identity(pt);
    embi_sb ts = {0};
    if (pat_attr(pat, "patternTransform", &ts) && ts.len > 0) {
        float t[6];
        if (svg_parse_transform(ts.data, ts.len, t)) {
            memcpy(pt, t, sizeof pt);
        }
    }
    embi_sb_free(&ts);
    float ptinv[6];
    if (!m_invert(pt, ptinv)) {
        svgp_free(path);
        return;
    }
    /* the content matrix: the viewBox, else the bounding box units */
    float cm[6];
    m_identity(cm);
    embi_sb vbs = {0};
    bool has_vb = false;
    if (pat_attr(pat, "viewBox", &vbs)) {
        double v[4];
        bool ok;
        if (svg_number_list(vbs.data != nullptr ? vbs.data : "", vbs.len, v, 4, &ok) == 4 && ok && v[2] > 0 && v[3] > 0) {
            float vb[4] = {(float)v[0], (float)v[1], (float)v[2], (float)v[3]};
            svg_par par = {5, false};
            embi_sb pb = {0};
            if (pat_attr(pat, "preserveAspectRatio", &pb)) {
                par = svg_parse_par(pb.data != nullptr ? pb.data : "", pb.len);
            }
            embi_sb_free(&pb);
            svg_viewbox_matrix(vb, w, h, par, cm);
            has_vb = true;
        }
    }
    embi_sb_free(&vbs);
    if (!has_vb && cobb) {
        cm[0] = bbox[2];
        cm[3] = bbox[3];
    }
    /* the tiles that meet the box of the path (in the space of the pattern) */
    float pb4[4], local[4];
    if (!svgp_bbox(path, pb4)) {
        svgp_free(path);
        return;
    }
    m_map_rect(ptinv, pb4, local);
    double di0 = floor(((double)local[0] - x) / w), di1 = ceil(((double)local[0] + local[2] - x) / w) - 1;
    double dj0 = floor(((double)local[1] - y) / h), dj1 = ceil(((double)local[1] + local[3] - y) / h) - 1;
    /* the tile counts are checked in double before anything is converted: a tiny tile against a large shape would overflow */
    if (!isfinite(di0) || !isfinite(di1) || !isfinite(dj0) || !isfinite(dj1) || di1 - di0 + 1 > PAT_MAX_TILES ||
        dj1 - dj0 + 1 > PAT_MAX_TILES || (di1 - di0 + 1) * (dj1 - dj0 + 1) > PAT_MAX_TILES) {
        svgp_free(path);
        return; /* a pattern with too many tiles for the box of the path paints nothing (see PAT_MAX_TILES) */
    }
    int i0 = (int)di0, i1 = (int)di1, j0 = (int)dj0, j1 = (int)dj1;
    uint32_t g = svg_em_group_begin(c, EMBI_LS_ISO, opacity, clip);
    int saved = c->luma;
    float ovw = c->vpw, ovh = c->vph;
    if (has_vb) {
        c->vpw = w;
        c->vph = h;
    }
    /* One tile is walked; the others are copies of its items moved by the tile step (in the pattern's space). */
    embi_dl *d = c->dl;
    float P[6], PC[6], tt[6] = {1, 0, 0, 1, x + (float)i0 * w, y + (float)j0 * h};
    m_mul(m, pt, P);
    m_mul(P, tt, P);
    float r[4] = {0, 0, w, h};
    uint32_t c0 = d->clips.len, t0 = d->items.len;
    uint32_t tclip = svg_em_clip(c, r, P, clip);
    m_mul(P, cm, PC);
    svg_walk_children(c, content, PC, tclip);
    c->vpw = ovw;
    c->vph = ovh;
    uint32_t c1 = d->clips.len, t1 = d->items.len;
    uint64_t ntiles = (uint64_t)(i1 - i0 + 1) * (uint64_t)(j1 - j0 + 1);
    if (t1 > t0 && (uint64_t)(t1 - t0) * (ntiles - 1) > PAT_MAX_COPIES) {
        d->items.len = g; /* the copies would pass the budget: the pattern paints nothing */
        svgp_free(path);
        return;
    }
    if (t1 > t0 && ntiles > 1) {
        float A[6], Ainv[6];
        m_mul(c->base, m, A);
        m_mul(A, pt, A);
        if (m_invert(A, Ainv)) {
            pat_copy_tiles(c, A, Ainv, w, h, i0, i1, j0, j1, t0, t1, c0, c1);
        }
    }
    /* the coverage of the shape keeps what is inside the path */
    uint32_t mg = svg_em_group_begin(c, EMBI_LS_SVGMASK, 1.0f, clip);
    c->luma = 2;
    svg_paint blk = {SP_COLOR, 0xFF000000u, nullptr, 0, false};
    svg_emit_path(c, path, rule, &blk, m, bbox, 1.0f, clip);
    c->luma = saved;
    svg_em_group_end(c, mg);
    svg_em_group_end(c, g);
}

void svg_emit_path(svg_ctx *c, embi_path *path, int rule, const svg_paint *p, const float m[6], const float bbox[4], float opacity,
                   uint32_t clip)
{
    if (c->dl == nullptr || p->kind == SP_NONE || svgp_empty(path)) {
        svgp_free(path);
        return;
    }
    if (p->kind == SP_COLOR) {
        uint32_t col = svg_luma_color(c, with_alpha(p->rgba, opacity));
        if ((col >> 24) == 0) {
            svgp_free(path);
            return;
        }
        push_path_item(c, path, rule, svg_em_xform(c, m), clip, col, 0, nullptr, 0, 0, false);
        return;
    }
    if (p->server != nullptr && tag_of(p->server) == TG_PATTERN) {
        emit_pattern(c, path, rule, p->server, m, bbox, opacity, clip);
        return;
    }
    if (p->server != nullptr) {
        emit_gradient(c, path, rule, p->server, m, bbox, opacity, clip);
        return;
    }
    svgp_free(path);
}
