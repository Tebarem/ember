/*
 * Text of the SVG module (R4c): <text> and <tspan> with plain character data, the x, y, dx, dy and rotate lists (one value per
 * addressable character, the last rotate value repeating), text-anchor per text chunk, dominant-baseline and the fill color.
 * A run of characters with no per-character value is one shaped run drawn as one EMBI_DI_TEXT item, so kerning and ligatures
 * survive the common case; a character with a value of its own is shaped alone. Not yet: textPath, textLength,
 * baseline-shift, writing modes, and gradient fills of text.
 */
#include "svg/svg_int.h"

#include "dom/dstr.h"
#include "layout/layout_int.h"
#include "layout/text_shim.h"
#include "paint/dl.h"

#define MAX_PIECES 4096
#define MAX_CHARS 65536
#define MAX_LIST 4096

enum { A_X, A_Y, A_DX, A_DY, A_ROT, A_COUNT };
enum { H_X = 1, H_Y = 2, H_DX = 4, H_DY = 8, H_ROT = 16 };

typedef struct tpiece {
    embi_lay_shaped *run;
    const embi_style *st;
    float x, y, w, asc, desc;
    float rot; /* radians, about (x, y) */
    uint32_t chunk;
    uint8_t anchor;
} tpiece;

/* One addressable character with the values the x, y, dx, dy and rotate lists give it. */
typedef struct tchar {
    uint32_t off;
    uint8_t n;
    uint8_t has;
    const embi_style *st;
    float v[A_COUNT];
} tchar;

/* The lists of one element: they index the characters from the one at start. */
typedef struct lrec {
    uint32_t start;
    uint32_t n[A_COUNT];
    float *v[A_COUNT];
} lrec;

typedef struct tctx {
    svg_ctx *c;
    EMBI_VEC(tpiece) v;
    EMBI_VEC(tchar) chars;
    embi_sb txt;
    lrec recs[18];
    uint32_t nrec;
    int depth;
    bool inst; /* a use instance: the styles of the tspans are computed under their parents' */
} tctx;

static const char *const k_list_names[A_COUNT] = {"x", "y", "dx", "dy", "rotate"};

/* Parses a list attribute (lengths; degrees for rotate). Returns the count, values in out (cap at most). */
static uint32_t parse_list(svg_ctx *c, const embi_element *el, int which, float *out, uint32_t cap)
{
    embi_sb sb = {0};
    uint32_t n = 0;
    if (svg_attr_sb(el, k_list_names[which], &sb) && sb.len > 0) {
        const char *p = sb.data, *end = sb.data + sb.len;
        float fs = svg_font_px((const embi_style *)el->style);
        while (p < end && n < cap) {
            p = svg_skip_wsp_comma(p, end);
            if (p >= end) {
                break;
            }
            const char *e = p;
            while (e < end && *e != ' ' && *e != ',' && *e != '\t' && *e != '\n' && *e != '\r') {
                e++;
            }
            if (which == A_ROT) {
                const char *q = p;
                double d;
                if (!svg_number(&q, e, &d)) {
                    break;
                }
                out[n++] = (float)d;
            } else {
                svg_len l;
                if (!svg_parse_length(p, (uint32_t)(e - p), &l)) {
                    break;
                }
                out[n++] = svg_len_px(l, fs, which == A_X || which == A_DX ? c->vpw : c->vph);
            }
            p = e;
        }
    }
    embi_sb_free(&sb);
    return n;
}

/* The character data of a text node with white space collapsed (xml:space default): runs of white space become one space,
 * and the ends of the whole text element are trimmed by the caller. */
static void collapse(const embi_chardata *cd, embi_sb *out, bool *last_space)
{
    embi_sb raw = {0};
    embi_dstr_append_utf8(&raw, cd->data);
    for (uint32_t i = 0; i < raw.len; i++) {
        char ch = raw.data[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!*last_space) {
                embi_sb_append_char(out, ' ');
            }
            *last_space = true;
        } else {
            embi_sb_append_char(out, ch);
            *last_space = false;
        }
    }
    embi_sb_free(&raw);
}

static void add_char(tctx *t, const char *b, uint8_t n, const embi_style *st)
{
    if (t->chars.len >= MAX_CHARS) {
        return;
    }
    tchar ch;
    memset(&ch, 0, sizeof ch);
    ch.off = t->txt.len;
    ch.n = n;
    ch.st = st;
    uint32_t ci = t->chars.len;
    for (int k = 0; k < A_COUNT; k++) {
        for (uint32_t r = t->nrec; r-- > 0;) {
            const lrec *lr = &t->recs[r];
            if (lr->n[k] == 0) {
                continue;
            }
            uint32_t idx = ci - lr->start;
            if (idx < lr->n[k]) {
                ch.v[k] = lr->v[k][idx];
            } else if (k == A_ROT) {
                ch.v[k] = lr->v[k][lr->n[k] - 1]; /* the last rotate value repeats */
            } else {
                continue;
            }
            ch.has |= (uint8_t)(1u << k);
            break;
        }
    }
    if ((ch.has & H_ROT) && ch.v[A_ROT] == 0.0f) {
        ch.has &= (uint8_t)~H_ROT;
    }
    for (uint8_t i = 0; i < n; i++) {
        embi_sb_append_char(&t->txt, b[i]);
    }
    embi_vec_push(&t->chars, ch);
}

static void collect(tctx *t, embi_element *el, const embi_style *st, bool *last_space)
{
    svg_ctx *c = t->c;
    if (st == nullptr || t->depth > 16 || svg_kw(st, EMBI_PROP_display) == EMBI_KW_none) {
        return;
    }
    lrec *lr = &t->recs[t->nrec];
    memset(lr, 0, sizeof *lr);
    lr->start = t->chars.len;
    float *tmp = embi_alloc(MAX_LIST * sizeof(float));
    for (int k = 0; k < A_COUNT; k++) {
        uint32_t n = parse_list(c, el, k, tmp, MAX_LIST);
        if (n > 0) {
            lr->v[k] = embi_alloc(n * sizeof(float));
            memcpy(lr->v[k], tmp, n * sizeof(float));
            lr->n[k] = n;
        }
    }
    embi_free(tmp, MAX_LIST * sizeof(float));
    t->nrec++;
    for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
        if (n->type == EMBI_NODE_TEXT) {
            embi_sb s = {0};
            collapse((const embi_chardata *)(const void *)n, &s, last_space);
            for (uint32_t i = 0; i < s.len;) {
                unsigned char b0 = (unsigned char)s.data[i];
                uint32_t cl = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
                if (i + cl > s.len) {
                    cl = s.len - i;
                }
                add_char(t, s.data + i, (uint8_t)cl, st);
                i += cl;
            }
            embi_sb_free(&s);
        } else if (embi_is_element(n)) {
            embi_element *ch = embi_as_element(n);
            embi_str nm = embi_atom_str(ch->base.name);
            if (ch->base.extra == EMBI_NS_SVG &&
                ((nm.len == 5 && memcmp(nm.ptr, "tspan", 5) == 0) || (nm.len == 1 && nm.ptr[0] == 'a')) &&
                t->nrec < sizeof t->recs / sizeof t->recs[0]) {
                t->depth++;
                collect(t, ch, t->inst ? svg_style_of(c, ch, st) : (const embi_style *)ch->style, last_space);
                t->depth--;
            }
        }
    }
    t->nrec--;
    for (int k = 0; k < A_COUNT; k++) {
        if (lr->v[k] != nullptr) {
            embi_free(lr->v[k], lr->n[k] * sizeof(float));
        }
    }
}

/* How far below y the glyph origin (the alphabetic baseline) lies when the dominant baseline sits on y. */
static float baseline_shift(const embi_style *st, float asc, float desc, float fs)
{
    switch (svg_kw(st, EMBI_PROP_dominant_baseline)) {
    case EMBI_KW_central:
        return (asc - desc) * 0.5f;
    case EMBI_KW_middle:
        return fs * 0.25f;
    case EMBI_KW_hanging:
        return asc * 0.8f;
    case EMBI_KW_mathematical:
        return asc * 0.5f;
    case EMBI_KW_text_top:
        return asc;
    case EMBI_KW_text_bottom:
    case EMBI_KW_ideographic:
        return -desc;
    default:
        return 0.0f;
    }
}

/* Turns the characters into shaped pieces. */
static void layout_chars(tctx *t, const embi_style *tst)
{
    svg_ctx *c = t->c;
    float cx = 0, cy = 0;
    uint32_t chunk = 0;
    uint32_t i = 0;
    while (i < t->chars.len && t->v.len < MAX_PIECES) {
        const tchar *ch = &t->chars.data[i];
        if ((ch->has & (H_X | H_Y)) || i == 0) {
            chunk++;
        }
        if (ch->has & H_X) {
            cx = ch->v[A_X];
        }
        if (ch->has & H_Y) {
            cy = ch->v[A_Y];
        }
        if (ch->has & H_DX) {
            cx += ch->v[A_DX];
        }
        if (ch->has & H_DY) {
            cy += ch->v[A_DY];
        }
        uint32_t j = i + 1;
        if (!(ch->has & H_ROT)) {
            while (j < t->chars.len && t->chars.data[j].has == 0 && t->chars.data[j].st == ch->st) {
                j++;
            }
        }
        const tchar *last = &t->chars.data[j - 1];
        uint32_t b0 = ch->off, b1 = last->off + last->n;
        embi_lay_tstyle ts;
        embi_str fams[16];
        embi_lay_text_style(ch->st, &ts, fams, 16);
        embi_lay_shaped *run = embi_tshim_shape(c->env->text, t->txt.data + b0, b1 - b0, &ts);
        if (run != nullptr) {
            tpiece p;
            memset(&p, 0, sizeof p);
            p.run = run;
            p.st = ch->st;
            p.w = embi_tshim_advance(run);
            float a = 0, d = 0;
            if (!embi_tshim_content_metrics(run, &a, &d)) {
                a = svg_font_px(ch->st) * 0.9f;
                d = svg_font_px(ch->st) * 0.25f;
            }
            p.asc = a;
            p.desc = d;
            p.x = cx;
            const embi_style *bst = svg_kw(ch->st, EMBI_PROP_dominant_baseline) != EMBI_KW_auto ? ch->st : tst;
            p.y = cy + baseline_shift(bst, a, d, svg_font_px(ch->st));
            p.rot = (ch->has & H_ROT) ? ch->v[A_ROT] * 3.14159265358979323846f / 180.0f : 0.0f;
            p.chunk = chunk;
            uint16_t ta = svg_kw(ch->st, EMBI_PROP_text_anchor);
            p.anchor = (uint8_t)(ta == EMBI_KW_middle ? 1 : ta == EMBI_KW_end ? 2 : 0);
            cx += p.w;
            embi_vec_push(&t->v, p);
        }
        i = j;
    }
}

/* Applies text-anchor per chunk: the pieces of a chunk move together. */
static void anchor_chunks(tctx *t)
{
    uint32_t i = 0;
    while (i < t->v.len) {
        uint32_t j = i;
        while (j < t->v.len && t->v.data[j].chunk == t->v.data[i].chunk) {
            j++;
        }
        float span = t->v.data[j - 1].x + t->v.data[j - 1].w - t->v.data[i].x;
        float shift = t->v.data[i].anchor == 1 ? -span * 0.5f : t->v.data[i].anchor == 2 ? -span : 0.0f;
        for (uint32_t k = i; k < j; k++) {
            t->v.data[k].x += shift;
        }
        i = j;
    }
}

static void free_pieces(tctx *t, bool owned_by_list)
{
    if (!owned_by_list) {
        for (uint32_t i = 0; i < t->v.len; i++) {
            embi_tshim_free(t->v.data[i].run);
        }
    }
    embi_vec_free(&t->v);
}

static void gather(svg_ctx *c, embi_element *el, const embi_style *st, tctx *t)
{
    memset(t, 0, sizeof *t);
    t->c = c;
    t->inst = st != (const embi_style *)el->style;
    bool last_space = true; /* leading white space is dropped */
    collect(t, el, st, &last_space);
    if (t->chars.len > 0) {
        const tchar *l = &t->chars.data[t->chars.len - 1];
        if (l->n == 1 && t->txt.data[l->off] == ' ') {
            t->chars.len--; /* and so is the trailing one, which would offset middle and end anchoring */
        }
    }
    layout_chars(t, st);
    anchor_chunks(t);
    embi_vec_free(&t->chars);
    embi_sb_free(&t->txt);
}

bool svg_text_bbox(svg_ctx *c, embi_element *el, float out[4])
{
    if (c->env == nullptr || c->env->text == nullptr) {
        return false;
    }
    tctx t;
    gather(c, el, (const embi_style *)el->style, &t);
    bool have = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; i < t.v.len; i++) {
        const tpiece *p = &t.v.data[i];
        float a = p->x, b = p->y - p->asc, d = p->x + p->w, e = p->y + p->desc;
        if (!have) {
            x0 = a;
            y0 = b;
            x1 = d;
            y1 = e;
            have = true;
        } else {
            x0 = fminf(x0, a);
            y0 = fminf(y0, b);
            x1 = fmaxf(x1, d);
            y1 = fmaxf(y1, e);
        }
    }
    free_pieces(&t, false);
    if (have) {
        out[0] = x0;
        out[1] = y0;
        out[2] = x1 - x0;
        out[3] = y1 - y0;
    }
    return have;
}

/* ---- R4e: stroked text and text-shadow */

typedef struct qctx {
    embi_path *path;
    bool open;
    float sx, sy, cx, cy;
} qctx;

static void close_contour(qctx *k)
{
    if (k->open && fabsf(k->cx - k->sx) < 1e-3f && fabsf(k->cy - k->sy) < 1e-3f) {
        svgp_close(k->path);
    }
    k->open = false;
}

static void quad_cb(void *user, const float q[6])
{
    qctx *k = user;
    if (!k->open || fabsf(q[0] - k->cx) > 1e-3f || fabsf(q[1] - k->cy) > 1e-3f) {
        close_contour(k);
        svgp_move(k->path, q[0], q[1]);
        k->sx = q[0];
        k->sy = q[1];
        k->open = true;
    }
    svgp_quad(k->path, q[2], q[3], q[4], q[5]);
    k->cx = q[4];
    k->cy = q[5];
}

/* The outline of a piece in its own space (baseline origin at 0, 0); nullptr when it has none. */
static embi_path *piece_outline(svg_ctx *c, const tpiece *p, float tol)
{
    qctx k = {svgp_new(tol), false, 0, 0, 0, 0};
    if (k.path == nullptr) {
        return nullptr;
    }
    embi_tshim_outline(c->env->text, p->run, 0.0f, 0.0f, quad_cb, &k);
    close_contour(&k);
    if (svgp_empty(k.path)) {
        svgp_free(k.path);
        return nullptr;
    }
    return k.path;
}

/* One length of a text-shadow in user units. */
static float shadow_len(const embi_value *v, float fs)
{
    if (v->kind == EMBI_V_LENGTH) {
        return v->unit == EMBI_U_EM || v->unit == EMBI_U_REM ? (float)v->num * fs : (float)v->num;
    }
    return 0.0f;
}

/* Whether paint-order paints the stroke before the fill: the listed keywords come first, in the order given, and the ones left
 * out follow in the default order (fill, stroke, markers). A single keyword is a list of one. */
static bool stroke_before_fill(const embi_value *po)
{
    int rank_fill = 0, rank_stroke = 1;
    if (po != nullptr && (po->kind == EMBI_V_LIST || po->kind == EMBI_V_KEYWORD)) {
        uint32_t n = po->kind == EMBI_V_LIST ? po->list.n : 1;
        rank_fill = 10;
        rank_stroke = 11;
        for (uint32_t i = 0; i < n; i++) {
            const embi_value *it = po->kind == EMBI_V_LIST ? &po->list.items[i] : po;
            if (it->kind != EMBI_V_KEYWORD) {
                continue;
            }
            if (it->kw == EMBI_KW_fill) {
                rank_fill = (int)i;
            } else if (it->kw == EMBI_KW_stroke) {
                rank_stroke = (int)i;
            }
        }
    }
    return rank_stroke < rank_fill;
}

typedef struct pfx {
    bool fill, stroke, stroke_first;
    uint32_t fill_rgba, stroke_rgba;
} pfx;

/* The items of one piece: the fill and the stroke in the paint order, moved by (dx, dy) in the space of the piece. */
static void emit_piece(svg_ctx *c, const tpiece *p, uint32_t ri, const float MR[6], uint32_t pxf, uint32_t clip,
                       const pfx *fx, float dx, float dy)
{
    embi_dl *d = c->dl;
    for (int pass = 0; pass < 2; pass++) {
        bool is_stroke = fx->stroke_first ? pass == 0 : pass == 1;
        if (is_stroke) {
            if (!fx->stroke || (fx->stroke_rgba >> 24) == 0) {
                continue;
            }
            float sc = m_scale(MR) * c->dev;
            float tol = 0.2f / (sc > 1e-6f ? sc : 1e-6f);
            embi_path *gp = piece_outline(c, p, tol);
            if (gp != nullptr) {
                float T[6] = {1, 0, 0, 1, p->x + dx, p->y + dy}, Mg[6];
                m_mul(MR, T, Mg);
                svg_emit_stroke_of(c, p->st, gp, Mg, clip, fx->stroke_rgba);
            }
        } else {
            if (!fx->fill || (fx->fill_rgba >> 24) == 0) {
                continue;
            }
            embi_ditem it;
            memset(&it, 0, sizeof it);
            it.kind = EMBI_DI_TEXT;
            it.xform = pxf;
            it.clip = clip;
            it.rect[0] = p->x + dx;
            it.rect[1] = p->y - p->asc + dy;
            it.rect[2] = p->w;
            it.rect[3] = p->asc + p->desc;
            it.aux = ri;
            it.aux2 = embi_tshim_len(p->run);
            it.aux3 = 0;
            embi_ditem_set_chash(&it, embi_tshim_content_hash(p->run, 0, it.aux2));
            it.f[0] = p->x + dx;
            it.f[1] = p->y + dy;
            it.colors[0] = fx->fill_rgba;
            /* text-rendering: geometricPrecision draws the glyphs with no pixel fitting, as in HTML text. */
            const embi_value *tr = embi_style_value(p->st, EMBI_PROP_text_rendering);
            if (tr != nullptr && tr->kind == EMBI_V_KEYWORD && tr->kw == EMBI_KW_geometricprecision) {
                it.flags |= EMBI_DF_GEOMPREC;
            }
            embi_vec_push(&d->items, it);
        }
    }
}

void svg_emit_text(svg_ctx *c, embi_element *el, const embi_style *st, const float M[6], uint32_t clip, bool *hit, double hp[2])
{
    *hit = false;
    if (c->env == nullptr || c->env->text == nullptr) {
        return;
    }
    tctx t;
    gather(c, el, st, &t);
    if (c->mode == SM_HIT) {
        for (uint32_t i = 0; i < t.v.len; i++) {
            const tpiece *p = &t.v.data[i];
            if (hp[0] >= p->x && hp[0] <= p->x + p->w && hp[1] >= p->y - p->asc && hp[1] <= p->y + p->desc) {
                *hit = true;
            }
        }
        free_pieces(&t, false);
        return;
    }
    uint32_t xf = svg_em_xform(c, M);
    embi_dl *d = c->dl;
    for (uint32_t i = 0; i < t.v.len; i++) {
        tpiece *p = &t.v.data[i];
        const embi_style *ps = p->st;
        /* visibility is per piece: a tspan can show what its text hides (SVG 2 section 13.7) */
        uint16_t vis = svg_kw(ps, EMBI_PROP_visibility);
        bool visible = vis != EMBI_KW_hidden && vis != EMBI_KW_collapse;
        svg_paint fp = svg_style_paint(c, el, ps, false);
        pfx fx;
        memset(&fx, 0, sizeof fx);
        fx.fill = visible && fp.kind == SP_COLOR;
        if (fx.fill) {
            float a = (float)(fp.rgba >> 24) * (float)svg_num(ps, EMBI_PROP_fill_opacity, 1.0);
            fx.fill_rgba = svg_luma_color(c, (fp.rgba & 0x00FFFFFFu) | ((a <= 0 ? 0u : a >= 255 ? 255u : (uint32_t)(a + 0.5f)) << 24));
        }
        if (visible && c->luma != 2) {
            svg_paint sp = svg_style_paint(c, el, ps, true);
            if (sp.kind == SP_COLOR && svg_stroke_width_of(c, ps) > 0) {
                float a = (float)(sp.rgba >> 24) * (float)svg_num(ps, EMBI_PROP_stroke_opacity, 1.0);
                fx.stroke = true;
                fx.stroke_rgba = (sp.rgba & 0x00FFFFFFu) | ((a <= 0 ? 0u : a >= 255 ? 255u : (uint32_t)(a + 0.5f)) << 24);
            }
            fx.stroke_first = stroke_before_fill(embi_style_value(ps, EMBI_PROP_paint_order));
        }
        if (!fx.fill && !fx.stroke) {
            embi_tshim_free(p->run);
            continue;
        }
        uint32_t pxf = xf;
        float MR[6];
        memcpy(MR, M, sizeof MR);
        if (p->rot != 0.0f) {
            float cs = cosf(p->rot), sn = sinf(p->rot);
            float R[6] = {cs, sn, -sn, cs, p->x - cs * p->x + sn * p->y, p->y - sn * p->x - cs * p->y};
            m_mul(M, R, MR);
            pxf = svg_em_xform(c, MR);
        }
        uint32_t ri = d->runs.len;
        embi_vec_push(&d->runs, (const void *)p->run);
        embi_vec_push(&d->own_runs, (void *)p->run);
        /* text-shadow (R4e): last to first, below the text; each shadow is the fill and the stroke in its color, moved by its
         * offset, and a blurred one is a filtered group (as the shadow of html text, paint/text_paint.c) */
        const embi_value *shv = c->luma == 0 ? embi_style_value(ps, EMBI_PROP_text_shadow) : nullptr;
        if (shv != nullptr && shv->kind == EMBI_V_SHADOWS && shv->shadows.n > 0) {
            float fs = svg_font_px(ps);
            for (uint32_t k = shv->shadows.n; k-- > 0;) {
                const embi_shadow *sh = &shv->shadows.p[k];
                uint32_t sc = 0;
                if (!svg_color(ps, &sh->color, &sc) || (sc >> 24) == 0) {
                    continue;
                }
                float ox = shadow_len(&sh->x, fs), oy = shadow_len(&sh->y, fs);
                float sigma = shadow_len(&sh->blur, fs) * 0.5f;
                pfx sfx = fx;
                sfx.fill_rgba = sc;
                sfx.stroke_rgba = sc;
                if (!(sigma >= 0.4f)) {
                    emit_piece(c, p, ri, MR, pxf, clip, &sfx, ox, oy);
                    continue;
                }
                embi_dfx op;
                memset(&op, 0, sizeof op);
                op.kind = EMBI_FX_BLUR;
                op.sigma = sigma;
                uint32_t first = d->fx.len;
                embi_vec_push(&d->fx, op);
                uint32_t gb = svg_em_group_begin(c, EMBI_LAYER_FILTERED, 1.0f, clip);
                d->items.data[gb].aux2 = first;
                d->items.data[gb].aux3 = 1;
                d->items.data[gb].xform = pxf;
                emit_piece(c, p, ri, MR, pxf, clip, &sfx, ox, oy);
                svg_em_group_end(c, gb);
                if (d->items.len > gb) {
                    d->fx_items++;
                }
            }
        }
        emit_piece(c, p, ri, MR, pxf, clip, &fx, 0.0f, 0.0f);
    }
    free_pieces(&t, true);
}
