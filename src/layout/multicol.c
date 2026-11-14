/*
 * Multi-column layout (CSS Multi-column Layout 1, with the break rules of CSS Fragmentation 3).
 *
 * The content of a multi-column container is laid out once by the ordinary flow, in one tall column of the column width
 * (block.c). embi_lay_multicol then cuts that fragment tree into columns: it finds the break positions (a pure function of
 * the geometry: candidates between lines and between block siblings, forbidden inside lines and monolithic boxes), and
 * distributes the fragments over the columns. A block that crosses a break becomes one fragment per column (the first one
 * is the original; the others are copies with the border and padding of the cut edge dropped: box-decoration-break: slice),
 * and a margin at a break is truncated because the next column starts at the top of the border box that follows the break.
 * Balancing is a search over the column height that only reruns the break finder, never the layout.
 *
 * Not done: floats and clear do not look at the column they fall in (a float is a tall box of the one-column flow and is
 * cut like a block), flex, grid and table boxes and replaced and scroll boxes are monolithic, the position of an absolutely
 * positioned box with a static position is that of the one-column flow, and a background image is laid out per fragment.
 */
#include "layout_int.h"

#include <stdlib.h>
#include <string.h>

#define MC_EPS 0.01f
#define MC_MAXCOLS 1024u
#define MC_ORPHANS 2u
#define MC_WIDOWS 2u

/* Candidate flags. */
#define MCF_HARD 1u  /* inside a line or a monolithic box: never a break */
#define MCF_FORCE 2u /* a forced break (break-before or break-after: column, page, always ...) */
#define MCF_SOFT 4u  /* a break that should be avoided: inside a box with break-inside: avoid, next to an avoid, orphans, widows */

typedef struct mc_cand {
    float pos; /* the block position where the next column would start: the top of the box or line that follows the break */
    float end; /* where the content before the break ends (before the margin between the boxes): it must fit in the column */
    uint8_t fl;
} mc_cand;

typedef struct mc_iv {
    float lo, hi;
} mc_iv;

typedef struct mc_item {
    float top, bot;
    bool leaf; /* a line, a monolithic box or a box without children: it fills the columns it crosses */
} mc_item;

typedef struct mc_par {
    embi_frag *p;
    float top; /* the block position (flow frame) of the top of the parent piece */
    float xs;  /* added to the x of the pieces (the column offset, for the container itself) */
} mc_par;

typedef EMBI_VEC(mc_iv) mc_ivvec;
typedef EMBI_VEC(float) mc_fvec;

typedef struct mc_ctx {
    embi_lctx *c;
    const embi_mc_in *in;
    EMBI_VEC(mc_cand) cand;
    EMBI_VEC(mc_cand) forced;
    float gmax; /* the largest distance between a candidate and the end of the content before it */
    mc_ivvec hard, soft, fre, ffre; /* fre, ffre: the content boxes of boxes and of floats without children, where a column may end anywhere */
    mc_fvec hlo, hhi, slo, shi;
    EMBI_VEC(mc_item) items;
    bool cands;
    float brk[MC_MAXCOLS + 1u];
    uint32_t ncol;
    const mc_par *top; /* the columns of the container itself (a float that sticks out of its parent goes there) */
} mc_ctx;

/* ------------------------------------------------------------------ geometry */

void embi_lay_mc_geom(const embi_lctx *c, const embi_box *b, float U, embi_mc_geo *g)
{
    const embi_value *cc = embi_lay_prop(c, b, EMBI_PROP_column_count);
    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_column_width);
    const embi_value *gv = embi_lay_prop(c, b, EMBI_PROP_column_gap);
    float gap = gv == nullptr || gv->kind == EMBI_V_KEYWORD ? embi_lay_font_px(c, b) : embi_lay_len(c, b, gv, U);
    gap = isnan(gap) || gap < 0 ? 0.0f : gap;
    bool has_n = cc != nullptr && (cc->kind == EMBI_V_INTEGER || cc->kind == EMBI_V_NUMBER) && cc->num >= 1;
    float n = has_n ? (cc->num > MC_MAXCOLS ? (float)MC_MAXCOLS : (float)cc->num) : 1.0f;
    float w = wv != nullptr && wv->kind != EMBI_V_KEYWORD ? embi_lay_len(c, b, wv, NAN) : NAN;
    bool has_w = !isnan(w);
    U = isnan(U) || U < 0 ? 0.0f : U;
    float N = n;
    if (has_w) {
        /* section 3.4: the most columns of at least the given width that fit, at most column-count */
        float fit = w + gap > 0 ? floorf((U + gap) / (w + gap)) : (float)MC_MAXCOLS;
        fit = fit < 1.0f ? 1.0f : (fit > (float)MC_MAXCOLS ? (float)MC_MAXCOLS : fit);
        N = has_n && n < fit ? n : fit;
    }
    float cw = (U - (N - 1.0f) * gap) / N;
    g->n = (uint32_t)N;
    g->colw = cw < 0 ? 0.0f : cw;
    g->gap = gap;
}

void embi_lay_mc_intrinsic(const embi_lctx *c, const embi_box *b, float *mn, float *mx)
{
    const embi_value *cc = embi_lay_prop(c, b, EMBI_PROP_column_count);
    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_column_width);
    const embi_value *gv = embi_lay_prop(c, b, EMBI_PROP_column_gap);
    float gap = gv == nullptr || gv->kind == EMBI_V_KEYWORD ? embi_lay_font_px(c, b) : embi_lay_len(c, b, gv, 0.0f);
    gap = isnan(gap) || gap < 0 ? 0.0f : gap;
    bool has_n = cc != nullptr && (cc->kind == EMBI_V_INTEGER || cc->kind == EMBI_V_NUMBER) && cc->num >= 1;
    float N = has_n ? (cc->num > MC_MAXCOLS ? (float)MC_MAXCOLS : (float)cc->num) : 1.0f;
    float w = wv != nullptr && wv->kind != EMBI_V_KEYWORD ? embi_lay_len(c, b, wv, NAN) : NAN;
    float a = *mn, z = *mx;
    if (!isnan(w)) {
        a = a < w ? w : a;
        z = z < w ? w : z;
    }
    *mn = N * a + (N - 1.0f) * gap;
    *mx = N * z + (N - 1.0f) * gap;
}

bool embi_lay_is_spanner(const embi_lctx *c, const embi_box *k)
{
    if (embi_box_is_text(k) || k->kind == EMBI_BK_INLINE || k->kind == EMBI_BK_BR ||
        (k->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_INLINE_LEVEL | EMBI_BF_ANON)) != 0) {
        return false;
    }
    const embi_value *v = embi_lay_prop(c, k, EMBI_PROP_column_span);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_all;
}

/* ------------------------------------------------------------------ the candidates */

static bool mc_forced_kw(uint16_t k)
{
    return k == EMBI_KW_column || k == EMBI_KW_page || k == EMBI_KW_always || k == EMBI_KW_left || k == EMBI_KW_right ||
           k == EMBI_KW_recto || k == EMBI_KW_verso;
}

static bool mc_avoid_kw(uint16_t k)
{
    return k == EMBI_KW_avoid || k == EMBI_KW_avoid_column;
}

static uint16_t mc_kw(const embi_lctx *c, const embi_frag *k, embi_prop_id p)
{
    if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
        return 0;
    }
    const embi_value *v = embi_lay_prop(c, k->box, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* A box that is not cut: replaced and atomic boxes, scroll containers, flex, grid and table boxes, a nested multi-column
 * container. */
static bool mc_mono(const embi_frag *k)
{
    const embi_box *b = k->box;
    return k->kind != EMBI_FK_BOX || b == nullptr || b->kind != EMBI_BK_BLOCK ||
           (b->flags & (EMBI_BF_ATOMIC | EMBI_BF_REPLACED | EMBI_BF_INLINE_LEVEL | EMBI_BF_SCROLL)) != 0 ||
           (k->flags & EMBI_FF_SCROLL) != 0 || (b->tflags & EMBI_TF_MC) != 0 || k->inl != 0;
}

static bool mc_is_float(const embi_frag *k)
{
    return k->box != nullptr && (k->box->flags & EMBI_BF_FLOAT) != 0;
}

/* The break-before of a box, or of its first in-flow child when it has none (the break propagates to the parent). */
static uint16_t mc_before(const embi_lctx *c, const embi_frag *k)
{
    for (int depth = 0; k != nullptr && k->kind == EMBI_FK_BOX && depth < 32; depth++) {
        uint16_t v = mc_kw(c, k, EMBI_PROP_break_before);
        if (v != 0 && v != EMBI_KW_auto) {
            return v;
        }
        if (mc_mono(k)) {
            break;
        }
        const embi_frag *n = k->first;
        while (n != nullptr && n->kind == EMBI_FK_BOX && mc_is_float(n)) {
            n = n->next;
        }
        k = n;
    }
    return 0;
}

static uint16_t mc_after(const embi_lctx *c, const embi_frag *k)
{
    for (int depth = 0; k != nullptr && k->kind == EMBI_FK_BOX && depth < 32; depth++) {
        uint16_t v = mc_kw(c, k, EMBI_PROP_break_after);
        if (v != 0 && v != EMBI_KW_auto) {
            return v;
        }
        if (mc_mono(k)) {
            break;
        }
        const embi_frag *last = nullptr;
        for (const embi_frag *n = k->first; n != nullptr; n = n->next) {
            if (n->kind == EMBI_FK_BOX && !mc_is_float(n)) {
                last = n;
            } else if (n->kind == EMBI_FK_LINE) {
                last = nullptr;
            }
        }
        k = last;
    }
    return 0;
}

static void mc_add_iv(mc_ctx *m, float lo, float hi, bool soft)
{
    if (hi - lo <= 0.05f) {
        return;
    }
    mc_iv v = {lo, hi};
    if (soft) {
        embi_vec_push(&m->soft, v);
    } else {
        embi_vec_push(&m->hard, v);
    }
}

/* The candidates, intervals and extents of the children from first on (a container whose top is at gy). */
static void mc_collect(mc_ctx *m, const embi_frag *first, float gy)
{
    embi_lctx *c = m->c;
    uint32_t nlines = 0;
    for (const embi_frag *k = first; k != nullptr; k = k->next) {
        nlines += k->kind == EMBI_FK_LINE ? 1u : 0u;
    }
    uint32_t li = 0;
    const embi_frag *prev = nullptr;
    float prev_bot = gy;
    for (const embi_frag *k = first; k != nullptr; k = k->next) {
        float top = gy + k->y, bot = top + k->h;
        if (k->kind == EMBI_FK_LINE) {
            li++;
            mc_item it = {top, bot, true};
            embi_vec_push(&m->items, it);
            mc_add_iv(m, top, bot, false);
            if (li >= 2 && m->cands) {
                bool bad = li - 1u < MC_ORPHANS || nlines - (li - 1u) < MC_WIDOWS;
                mc_cand cd = {top, top, bad ? MCF_SOFT : 0u};
                embi_vec_push(&m->cand, cd);
            }
            continue;
        }
        if (k->kind != EMBI_FK_BOX) {
            continue;
        }
        bool flt = mc_is_float(k);
        bool mono = mc_mono(k);
        if (!flt && prev != nullptr && m->cands) {
            uint16_t ba = mc_after(c, prev), bb = mc_before(c, k);
            uint8_t fl = 0;
            if (mc_forced_kw(ba) || mc_forced_kw(bb)) {
                fl = MCF_FORCE;
            } else if (mc_avoid_kw(ba) || mc_avoid_kw(bb)) {
                fl = MCF_SOFT;
            }
            mc_cand cd = {top, prev_bot < top ? prev_bot : top, fl};
            embi_vec_push(&m->cand, cd);
        }
        if (!flt) {
            prev = k;
            /* what sticks out of the box (a float taller than its parent) is content before the break too */
            prev_bot = k->inl == 0 && (k->flags & (EMBI_FF_CLIP | EMBI_FF_SCROLL)) == 0 && top + k->ov[3] > bot + 2.0f ? top + k->ov[3] : bot;
        }
        if (mono) {
            mc_item it = {top, bot, true};
            embi_vec_push(&m->items, it);
            mc_add_iv(m, top, bot, false);
            continue;
        }
        mc_item it = {top, bot, k->first == nullptr};
        embi_vec_push(&m->items, it);
        if (k->first == nullptr) {
            float lo = top + embi_frag_bdr(k, 1) + embi_frag_pad(k, 1), hi = bot - embi_frag_bdr(k, 3) - embi_frag_pad(k, 3);
            if (hi - lo > 0.05f) {
                mc_iv v = {lo, hi};
                if (flt) {
                    embi_vec_push(&m->ffre, v);
                } else {
                    embi_vec_push(&m->fre, v);
                }
            }
        }
        if (mc_avoid_kw(mc_kw(c, k, EMBI_PROP_break_inside))) {
            mc_add_iv(m, top, bot, true);
        }
        bool saved = m->cands;
        m->cands = saved && !flt;
        mc_collect(m, k->first, top);
        m->cands = saved;
    }
}

static int mc_cmp_cand(const void *a, const void *b)
{
    float x = ((const mc_cand *)a)->pos, y = ((const mc_cand *)b)->pos;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int mc_cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* The number of elements of the sorted array a that are < v (le false) or <= v (le true). */
static uint32_t mc_count(const float *a, uint32_t n, float v, bool le)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (a[mid] < v || (le && a[mid] <= v)) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void mc_sorted_ends(mc_ivvec *iv, mc_fvec *lo, mc_fvec *hi)
{
    embi_vec_clear(lo);
    embi_vec_clear(hi);
    for (uint32_t i = 0; i < iv->len; i++) {
        embi_vec_push(lo, iv->data[i].lo);
        embi_vec_push(hi, iv->data[i].hi);
    }
    if (lo->len > 1) {
        qsort(lo->data, lo->len, sizeof(float), mc_cmp_f);
        qsort(hi->data, hi->len, sizeof(float), mc_cmp_f);
    }
}

static uint8_t mc_cover(const mc_ctx *m, float p);

static void mc_analyze(mc_ctx *m, const embi_frag *first)
{
    embi_vec_clear(&m->cand);
    embi_vec_clear(&m->forced);
    m->gmax = 0;
    embi_vec_clear(&m->hard);
    embi_vec_clear(&m->soft);
    embi_vec_clear(&m->fre);
    embi_vec_clear(&m->ffre);
    embi_vec_clear(&m->items);
    m->cands = true;
    mc_collect(m, first, 0.0f);
    if (m->cand.len > 1) {
        qsort(m->cand.data, m->cand.len, sizeof(mc_cand), mc_cmp_cand);
    }
    if (m->fre.len > 1) {
        qsort(m->fre.data, m->fre.len, sizeof(mc_iv), mc_cmp_f); /* by lo, the first member */
    }
    if (m->ffre.len > 1) {
        qsort(m->ffre.data, m->ffre.len, sizeof(mc_iv), mc_cmp_f);
    }
    mc_sorted_ends(&m->hard, &m->hlo, &m->hhi);
    mc_sorted_ends(&m->soft, &m->slo, &m->shi);
    for (uint32_t i = 0; i < m->cand.len; i++) {
        mc_cand *cd = &m->cand.data[i];
        cd->fl |= mc_cover(m, cd->pos);
        if ((cd->fl & (MCF_FORCE | MCF_HARD)) == MCF_FORCE) {
            embi_vec_push(&m->forced, *cd);
        }
        m->gmax = cd->pos - cd->end > m->gmax ? cd->pos - cd->end : m->gmax;
    }
}

/* The flags (MCF_HARD, MCF_SOFT) of a position: inside a line or monolithic box, inside a box with break-inside: avoid. */
static uint8_t mc_cover(const mc_ctx *m, float p)
{
    if ((int64_t)mc_count(m->hlo.data, m->hlo.len, p - MC_EPS, false) -
            (int64_t)mc_count(m->hhi.data, m->hhi.len, p + MC_EPS, true) > 0) {
        return MCF_HARD;
    }
    if ((int64_t)mc_count(m->slo.data, m->slo.len, p - MC_EPS, false) -
            (int64_t)mc_count(m->shi.data, m->shi.len, p + MC_EPS, true) > 0) {
        return MCF_SOFT;
    }
    return 0;
}

/* Whether a column may end at y inside the content box of a box without children. */
static bool mc_free_at(const mc_ivvec *fre, float y)
{
    uint32_t lo = 0, hi = fre->len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (fre->data[mid].lo < y - MC_EPS) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo > 0 && y < fre->data[lo - 1u].hi - MC_EPS;
}

/* ------------------------------------------------------------------ finding the breaks */

/* The start of the column that follows a column that starts at S and is Hh tall, or INFINITY when the rest of the content (up to Z)
 * fits in it. */
static float mc_fill(const mc_ctx *m, float S, float Hh, float Z)
{
    float lim = S + (Hh < 0 ? 0.0f : Hh);
    {
        /* the first forced break after S ends the column when the content before it fits */
        uint32_t lo = 0, hi = m->forced.len;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2u;
            if (m->forced.data[mid].pos <= S + MC_EPS) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo < m->forced.len && m->forced.data[lo].end <= lim + MC_EPS) {
            return m->forced.data[lo].pos;
        }
    }
    if (Z <= lim + MC_EPS) {
        return INFINITY;
    }
    uint32_t n = m->cand.len;
    uint32_t lo = 0, hi = n; /* the number of candidates that start at most gmax after lim */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (m->cand.data[mid].pos <= lim + m->gmax + MC_EPS) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    /* A float that crosses the end of the column is cut there, rather than the column ending before the box that holds it. */
    if (m->ffre.len != 0 && lim > S + MC_EPS && mc_free_at(&m->ffre, lim) && (mc_cover(m, lim) & MCF_HARD) == 0) {
        return lim;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = lo; i-- > 0;) {
            const mc_cand *cd = &m->cand.data[i];
            if (cd->pos <= S + MC_EPS) {
                break;
            }
            if (cd->end <= lim + MC_EPS && (cd->fl & MCF_HARD) == 0 && (pass == 1 || (cd->fl & MCF_SOFT) == 0)) {
                return cd->pos;
            }
        }
    }
    /* No break between boxes or lines fits: a box without children that crosses the end of the column is cut there. */
    if (m->fre.len != 0 && lim > S + MC_EPS && mc_free_at(&m->fre, lim) && (mc_cover(m, lim) & MCF_HARD) == 0) {
        return lim;
    }
    /* Nothing fits in the column: it overflows up to the next place a break is allowed. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            const mc_cand *cd = &m->cand.data[i];
            if (cd->pos > lim + MC_EPS && (cd->fl & MCF_HARD) == 0 && (pass == 1 || (cd->fl & MCF_SOFT) == 0)) {
                return cd->pos;
            }
        }
    }
    return INFINITY;
}

/* Fills columns of height Hh from S0 to Z; the number of columns used (at most MC_MAXCOLS), and with brk their starts. */
static uint32_t mc_sim(mc_ctx *m, float S0, float Hh, float Z, bool keep)
{
    uint32_t cols = 0;
    float S = S0;
    for (;;) {
        if (keep) {
            m->brk[cols] = S;
        }
        cols++;
        float nx = mc_fill(m, S, Hh, Z);
        if (!isfinite(nx) || cols >= MC_MAXCOLS) {
            break;
        }
        S = nx;
    }
    return cols;
}

/* The column of a position: the last column that starts at or before y (clamped). */
static uint32_t mc_col_of(const mc_ctx *m, float y)
{
    uint32_t lo = 0, hi = m->ncol;
    while (hi - lo > 1u) {
        uint32_t mid = (lo + hi) / 2u;
        if (m->brk[mid] <= y + MC_EPS) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The last column that starts before y (the column that holds a bottom edge at y). */
static uint32_t mc_col_end(const mc_ctx *m, float y)
{
    return mc_col_of(m, y - 2.0f * MC_EPS);
}

/* ------------------------------------------------------------------ cutting the fragments */

static void mc_set_met(embi_lctx *c, embi_frag *p, const float bdr[4], const float pad[4], const float mg[4], bool first,
                       bool last)
{
    embi_edges b = {bdr[0], first ? bdr[1] : 0.0f, bdr[2], last ? bdr[3] : 0.0f};
    embi_edges d = {pad[0], first ? pad[1] : 0.0f, pad[2], last ? pad[3] : 0.0f};
    embi_lay_frag_set_edges(c->ld, p, &b, &d);
    embi_lay_frag_set_mg(c->ld, p, mg[0], first ? mg[1] : 0.0f, mg[2], last ? mg[3] : 0.0f);
}

/* Puts g (at block position gy of the flow) into the columns of the pieces in par (indexed from off), clamped to [lo, hi]. */
static void mc_dist(mc_ctx *m, embi_frag *g, float gy, const mc_par *par, uint32_t off, uint32_t lo, uint32_t hi, float ax)
{
    embi_lctx *c = m->c;
    float gb = gy + g->h;
    uint32_t c0 = mc_col_of(m, gy);
    uint32_t c1 = g->h > MC_EPS ? mc_col_end(m, gb) : c0;
    /* A float may stick out of its parent, into columns where the parent has no piece: its pieces there go to the container. */
    bool ext = g->kind == EMBI_FK_BOX && mc_is_float(g) && (c0 < lo || c1 > hi);
    if (!ext) {
        if (c0 < lo) {
            c0 = lo;
        }
        if (c0 > hi) {
            c0 = hi;
        }
        if (c1 < c0) {
            c1 = c0;
        }
        if (c1 > hi) {
            c1 = hi;
        }
    }
    bool cuttable = g->kind == EMBI_FK_BOX && !mc_mono(g);
    /* A box that stays in one column can hold a float that reaches into the next ones. */
    bool deep = cuttable && c1 == c0 && c0 + 1u < m->ncol && g->inl == 0 && gy + g->ov[3] > m->brk[c0 + 1u] + MC_EPS &&
                !(c0 < lo || c0 > hi);
    bool split = (c1 > c0 && cuttable) || deep;
    if (!split) {
        if (c0 < lo || c0 > hi) {
            g->x += ax + m->top[c0].xs;
            g->y = gy - m->top[c0].top;
            embi_lay_frag_append(m->top[c0].p, g);
            return;
        }
        const mc_par *pp = &par[c0 - off];
        g->x += pp->xs;
        g->y = gy - pp->top;
        embi_lay_frag_append(pp->p, g);
        return;
    }
    uint32_t np = c1 - c0 + 1u;
    mc_par *gp = embi_calloc(np, sizeof *gp);
    if (gp == nullptr) {
        const mc_par *pp = &par[c0 - off];
        g->x += pp->xs;
        g->y = gy - pp->top;
        embi_lay_frag_append(pp->p, g);
        return;
    }
    float bdr[4], pad[4], mg[4];
    for (int i = 0; i < 4; i++) {
        bdr[i] = embi_frag_bdr(g, i);
        pad[i] = embi_frag_pad(g, i);
        mg[i] = embi_frag_mg(g, i);
    }
    float x0 = g->x;
    embi_frag *kids = g->first;
    g->first = nullptr;
    g->last = nullptr;
    g->box->tflags |= EMBI_TF_SPLIT;
    for (uint32_t j = c0; j <= c1; j++) {
        embi_frag *P = g;
        if (j != c0) {
            P = embi_lay_new_frag(c, g->box, EMBI_FK_BOX);
            if (P == nullptr) {
                P = g;
            } else {
                P->flags = (uint8_t)(g->flags & ~EMBI_FF_EXT);
                P->w = g->w;
                P->bl_first = NAN;
                P->bl_last = NAN;
            }
        }
        bool first = j == c0, last = j == c1;
        float ptop = gy > m->brk[j] ? gy : m->brk[j];
        float pbot = last ? gb : (gb < m->brk[j + 1u] ? gb : m->brk[j + 1u]);
        bool in = j >= lo && j <= hi;
        const mc_par *pp = in ? &par[j - off] : &m->top[j];
        mc_set_met(c, P, bdr, pad, mg, first, last);
        P->x = (in ? 0.0f : ax) + x0 + pp->xs;
        P->y = ptop - pp->top;
        embi_lay_frag_set_size(P, g->w, pbot - ptop);
        if (P != g || j == c0) {
            embi_lay_frag_append(pp->p, P);
        }
        gp[j - c0] = (mc_par){P, ptop, 0.0f};
    }
    for (embi_frag *k = kids; k != nullptr;) {
        embi_frag *nx = k->next;
        mc_dist(m, k, gy + k->y, gp, c0, c0, c1, ax + x0);
        k = nx;
    }
    for (uint32_t j = c0; j <= c1; j++) {
        embi_frag *P = gp[j - c0].p;
        bool first = j == c0, last = j == c1;
        float h = P->h;
        if (!last && P->first != nullptr) {
            /* A piece that is cut ends at its last child: the margin before the break is not part of it. */
            float nb = first ? bdr[1] + pad[1] : 0.0f;
            for (embi_frag *k = P->first; k != nullptr; k = k->next) {
                nb = k->y + k->h > nb ? k->y + k->h : nb;
            }
            h = nb < h ? nb : h;
            P->h = h;
        }
        P->ov[0] = bdr[0];
        P->ov[1] = first ? bdr[1] : 0.0f;
        P->ov[2] = P->w - bdr[2];
        P->ov[3] = h - (last ? bdr[3] : 0.0f);
        for (embi_frag *k = P->first; k != nullptr; k = k->next) {
            embi_lay_ov_add_child(P, k);
        }
    }
    embi_free(gp, np * sizeof *gp);
}

/* ------------------------------------------------------------------ the column sets */

static bool mc_frag_is_span(const embi_lctx *c, const embi_box *b, const embi_frag *k)
{
    return k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->parent == b && embi_lay_is_spanner(c, k->box);
}

/* Cuts one run of children (a chain of fragments starting at seg, between two spanners) into columns at out_top; returns the
 * height of the column set. Z is the end of the run's content in the one-column flow. */
static float mc_segment(mc_ctx *m, embi_frag *f, embi_frag *seg, float A, float Z, float out_top, float cap_rem,
                        embi_mcset *set)
{
    const embi_mc_in *in = m->in;
    float stride = in->g.colw + in->g.gap;
    mc_analyze(m, seg);
    float H;
    float total = Z - A;
    if (in->fill_auto && isfinite(cap_rem)) {
        H = cap_rem;
    } else {
        float hi = total;
        if (cap_rem < hi) {
            hi = cap_rem;
        }
        float lo = total / (float)in->g.n;
        if (lo > hi) {
            lo = hi;
        }
        if (mc_sim(m, A, lo, Z, false) <= in->g.n) {
            H = lo;
        } else if (mc_sim(m, A, hi, Z, false) > in->g.n) {
            H = hi;
        } else {
            for (int it = 0; it < 16 && hi - lo > 0.05f; it++) {
                float mid = (lo + hi) * 0.5f;
                if (mc_sim(m, A, mid, Z, false) <= in->g.n) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            H = hi;
        }
    }
    m->ncol = mc_sim(m, A, H, Z, true);
    m->brk[m->ncol] = Z;
    /* The height each column really uses (the margins at a break are not part of it). */
    float end[MC_MAXCOLS];
    for (uint32_t j = 0; j < m->ncol; j++) {
        end[j] = m->brk[j];
    }
    for (uint32_t i = 0; i < m->items.len; i++) {
        const mc_item *it = &m->items.data[i];
        uint32_t cl = mc_col_of(m, it->top), ch = it->bot > it->top + MC_EPS ? mc_col_end(m, it->bot) : cl;
        if (ch < cl) {
            ch = cl;
        }
        if (it->bot > end[ch]) {
            end[ch] = it->bot;
        }
        if (it->leaf) {
            for (uint32_t j = cl; j < ch; j++) {
                end[j] = m->brk[j + 1u];
            }
        }
    }
    if (Z > end[m->ncol - 1u]) {
        end[m->ncol - 1u] = Z;
    }
    float used = 0;
    for (uint32_t j = 0; j < m->ncol; j++) {
        float hj = end[j] - m->brk[j];
        used = hj > used ? hj : used;
    }
    float seth = in->fill_auto && isfinite(cap_rem) && in->definite ? cap_rem : used;
    mc_par *par = embi_calloc(m->ncol, sizeof *par);
    uint32_t nc = m->ncol;
    for (uint32_t j = 0; par != nullptr && j < nc; j++) {
        float xo = in->rtl ? ((float)in->g.n - 1.0f - (float)j) * stride : (float)j * stride;
        par[j] = (mc_par){f, m->brk[j] - out_top, xo};
    }
    m->top = par;
    for (embi_frag *k = seg; k != nullptr;) {
        embi_frag *nx = k->next;
        if (par != nullptr) {
            mc_dist(m, k, k->y, par, 0, 0, nc - 1u, 0.0f);
        } else {
            embi_lay_frag_append(f, k);
        }
        k = nx;
    }
    embi_free(par, (size_t)nc * sizeof *par);
    set->y = out_top;
    set->h = seth;
    set->cols = nc;
    return seth;
}

float embi_lay_multicol(embi_lctx *c, embi_box *b, embi_frag *f, const embi_mc_in *in)
{
    mc_ctx *m = embi_calloc(1, sizeof *m);
    if (m == nullptr) {
        return in->ch;
    }
    m->c = c;
    m->in = in;
    embi_frag *kids = f->first;
    f->first = nullptr;
    f->last = nullptr;
    uint32_t nspan = 0;
    for (embi_frag *k = kids; k != nullptr; k = k->next) {
        nspan += mc_frag_is_span(c, b, k) ? 1u : 0u;
    }
    embi_mcol *rec = embi_lay_alloc(&c->ld->fa, sizeof *rec + (nspan + 1u) * sizeof(embi_mcset), alignof(embi_mcol));
    if (rec != nullptr) {
        memset(rec, 0, sizeof *rec + (nspan + 1u) * sizeof(embi_mcset));
        rec->colw = in->g.colw;
        rec->gap = in->g.gap;
        float stride = in->g.colw + in->g.gap;
        rec->x0 = in->ox + (in->rtl ? ((float)in->g.n - 1.0f) * stride : 0.0f);
        rec->stride = in->rtl ? -stride : stride;
    }
    float oy = in->oy;
    float out = oy, flow = oy;
    float end_flow = oy + in->ch;
    bool had_last = false;
    embi_frag *k = kids;
    while (k != nullptr) {
        embi_frag *seg = k, *tail = nullptr;
        while (k != nullptr && !mc_frag_is_span(c, b, k)) {
            tail = k;
            k = k->next;
        }
        embi_frag *span = k;
        if (tail != nullptr) {
            tail->next = nullptr;
        }
        if (seg != nullptr && seg != span) {
            float A = seg->y, Z = 0;
            for (embi_frag *q = seg; q != nullptr; q = q->next) {
                A = q->y < A ? q->y : A;
                float qb = q->y + q->h;
                if (q->kind == EMBI_FK_BOX && q->inl == 0 && (q->flags & (EMBI_FF_CLIP | EMBI_FF_SCROLL)) == 0 && q->y + q->ov[3] > qb + 2.0f) {
                    qb = q->y + q->ov[3]; /* what sticks out (a float, a child of a box without height) is content too; up to 2px is glyph ink */
                }
                Z = qb > Z ? qb : Z;
            }
            bool last = span == nullptr;
            if (!had_last && out == oy && flow == oy) {
                A = oy;
            }
            if (last && end_flow > Z) {
                Z = end_flow;
            }
            float top = out + (A - flow);
            float cap_rem = isfinite(in->cap) ? in->cap - (top - oy) : INFINITY;
            if (cap_rem < 0) {
                cap_rem = 0;
            }
            embi_mcset tmp;
            embi_mcset *set = rec != nullptr ? &rec->sets[rec->nsets] : &tmp;
            float h = mc_segment(m, f, seg, A, Z, top, cap_rem, set);
            if (rec != nullptr) {
                rec->nsets++;
            }
            out = top + h;
            flow = Z;
            had_last = last;
        }
        if (span != nullptr) {
            k = span->next;
            span->next = nullptr;
            float gy = span->y;
            span->y = out + (gy - flow);
            embi_lay_frag_append(f, span);
            out = span->y + span->h;
            flow = gy + span->h;
        }
    }
    if (!had_last && end_flow > flow) {
        out += end_flow - flow; /* what follows the last spanner */
    }
    /* The overflow of the container from its new children. */
    f->ov[0] = in->bdr.l;
    f->ov[1] = in->bdr.t;
    f->ov[2] = f->w - in->bdr.r;
    f->ov[3] = 0;
    for (embi_frag *q = f->first; q != nullptr; q = q->next) {
        embi_lay_ov_add_child(f, q);
    }
    if (rec != nullptr && rec->nsets != 0) {
        bool any = false;
        for (uint32_t i = 0; i < rec->nsets; i++) {
            any = any || rec->sets[i].cols > 1u;
        }
        embi_fext *x = any ? embi_lay_frag_ext(c->ld, f) : nullptr;
        if (x != nullptr) {
            x->mc = rec;
        }
    }
    embi_vec_free(&m->cand);
    embi_vec_free(&m->forced);
    embi_vec_free(&m->hard);
    embi_vec_free(&m->soft);
    embi_vec_free(&m->fre);
    embi_vec_free(&m->ffre);
    embi_vec_free(&m->hlo);
    embi_vec_free(&m->hhi);
    embi_vec_free(&m->slo);
    embi_vec_free(&m->shi);
    embi_vec_free(&m->items);
    embi_free(m, sizeof *m);
    return out - oy;
}
