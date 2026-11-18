/*
 * Glyph hinting (see hint.h): blue zones, horizontal stems and interpolation of the remaining points; vertical stems in x for
 * the sharp mode; detection and snapping of pixel-art glyphs.
 *
 * The glyph is a list of quadratic curves in font units, y up, device px per unit s. The horizontal edges are found first (flat
 * segments and the tangents at curve extrema), each classed as a top edge (ink below it) or a bottom edge (ink above it).
 * Edges near an alignment zone go to the row of the zone; the two edges of a stem go to rows a whole number of px apart;
 * the points between anchored edges follow by linear interpolation.
 */

#include "text/hint.h"

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

/* Glyphs with more curves than this are drawn without hinting. */
#define HINT_MAX_QUADS 2048u
/* A zone catches an edge this many px beyond its flat and overshoot values. */
#define HINT_ZONE_REACH_PX 0.6f
/* ... and under a few px of x-height, where a pull of that size is a visible change of shape. */
#define HINT_ZONE_REACH_TINY_PX 0.3f
/* An anchor never moves its edge by more than this many px. */
#define HINT_MAX_MOVE_PX 0.75f
/* Below this x-height (device px) only the zone edges are fitted, not the stems: there are too few rows to keep a stem and the
 * gaps between its neighbours. */
#define HINT_STEM_MIN_XHEIGHT_PX 5.0f
/* A detached contour (the dot of i, an accent) shorter than this, in em, moves as a whole. */
#define HINT_DOT_MAX_EM 0.25f
/* The thickest stem, in em. */
#define HINT_STEM_MAX_EM 0.22f
/* How far outside its span (as a part of it) the extremum of a curve may lie to still count, and the steepest tangent at the
 * end of the curve that is taken as horizontal there. */
#define HINT_NEAR_EXTREMUM 0.15f
#define HINT_FLAT_SLOPE 0.06f
/* The overshoot of a zone, at most, in em. */
#define HINT_OVERSHOOT_MAX_EM 0.05f

static float rnd(float v)
{
    return SDL_floorf(v + 0.5f);
}

/* ------------------------------------------------------------------ zones */

void embi_hint_extent(const embi_qcurve *c, uint32_t n, float *ymin, float *ymax)
{
    float lo = 0.0f, hi = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &c[i];
        float ys[3] = {q->y0, q->y1, q->y0};
        uint32_t m = 2;
        float den = q->y0 - 2.0f * q->cy + q->y1;
        if (SDL_fabsf(den) > 1e-9f) {
            float t = (q->y0 - q->cy) / den;
            if (t > 0.0f && t < 1.0f) {
                float u = 1.0f - t;
                ys[2] = u * u * q->y0 + 2.0f * u * t * q->cy + t * t * q->y1;
                m = 3;
            }
        }
        for (uint32_t k = 0; k < m; k++) {
            if (i == 0 && k == 0) {
                lo = hi = ys[0];
            }
            lo = ys[k] < lo ? ys[k] : lo;
            hi = ys[k] > hi ? ys[k] : hi;
        }
    }
    *ymin = lo;
    *ymax = hi;
}

void embi_hint_zones_measure(embi_hint_zones *out, float upem, float os2_xheight, embi_hint_extent_fn fn, void *user)
{
    SDL_memset(out, 0, sizeof *out);
    const float lim = HINT_OVERSHOOT_MAX_EM * upem;
    float lo, hi;

    out->z[EMBI_HINT_BASELINE] = (embi_hint_zone){0.0f, 0.0f};
    out->n_valid_mask |= 1u << EMBI_HINT_BASELINE;
    if (fn(user, 'o', &lo, &hi) && lo < 0.0f && lo > -lim) {
        out->z[EMBI_HINT_BASELINE].over = lo;
    }

    /* The top zones: a flat glyph and a round one. */
    static const struct {
        int zone;
        uint32_t flat_cp[2], round_cp;
    } top[] = {
        {EMBI_HINT_XHEIGHT, {'x', 0}, 'o'},
        {EMBI_HINT_CAP, {'H', 0}, 'O'},
        {EMBI_HINT_FIGURES, {'1', '4'}, '0'},
    };
    for (uint32_t i = 0; i < sizeof top / sizeof top[0]; i++) {
        float flat = 0.0f;
        bool have = false;
        for (uint32_t k = 0; k < 2 && !have; k++) {
            if (top[i].flat_cp[k] != 0 && fn(user, top[i].flat_cp[k], &lo, &hi)) {
                have = true;
                flat = hi;
            }
        }
        if (!have && top[i].zone == EMBI_HINT_XHEIGHT && os2_xheight > 0.0f) {
            have = true;
            flat = os2_xheight;
        }
        if (!have || !(flat > 0.0f)) {
            continue;
        }
        float over = flat;
        if (fn(user, top[i].round_cp, &lo, &hi)) {
            over = hi < flat ? flat : hi;
        }
        if (over - flat >= lim) {
            continue;
        }
        out->z[top[i].zone] = (embi_hint_zone){flat, over};
        out->n_valid_mask |= 1u << top[i].zone;
    }

    /* The ascender: d (else l), the round b only when it barely exceeds it. */
    {
        bool have = fn(user, 'd', &lo, &hi) || fn(user, 'l', &lo, &hi);
        const float flat = have ? hi : 0.0f;
        if (have && flat > 0.0f) {
            float over = flat;
            if (fn(user, 'b', &lo, &hi) && hi > flat && hi - flat < lim) {
                over = hi;
            }
            out->z[EMBI_HINT_ASCENDER] = (embi_hint_zone){flat, over};
            out->n_valid_mask |= 1u << EMBI_HINT_ASCENDER;
        }
    }

    /* The descender: p, and g when it reaches a little lower. */
    if (fn(user, 'p', &lo, &hi) && lo < 0.0f) {
        float flat = lo, over = lo;
        if (fn(user, 'g', &lo, &hi) && lo < flat && flat - lo < lim) {
            over = lo;
        }
        out->z[EMBI_HINT_DESCENDER] = (embi_hint_zone){flat, over};
        out->n_valid_mask |= 1u << EMBI_HINT_DESCENDER;
    }
}

bool embi_hint_zones_usable(const embi_hint_zones *z)
{
    return (z->n_valid_mask & ((1u << EMBI_HINT_XHEIGHT) | (1u << EMBI_HINT_CAP))) != 0;
}

/* ------------------------------------------------------------------ edges */

/* One contour of the glyph. A dot is a small contour apart from the rest: it moves rigidly (a shift and a change of height). */
typedef struct hint_contour {
    float xa, xb, ya, yb; /* bounding box, font units */
    bool dot;
    float shift_px;       /* the dot: device px added to its bottom (ya) ... */
    float scale;          /* ... and the factor on its height */
} hint_contour;

typedef struct hint_edge {
    float y;       /* font units */
    float xa, xb;  /* horizontal extent */
    float weight;  /* length, for the mean of merged edges */
    bool top;      /* ink below the edge */
    bool anchored;
    float target;  /* px above the baseline, when anchored */
} hint_edge;

static void edge_insert(hint_edge *e, uint32_t *n, const hint_edge *add, float tol)
{
    /* Merge into an edge of the same class within tol (the list is short for a glyph: scan back while close). */
    for (uint32_t i = *n; i > 0; i--) {
        hint_edge *o = &e[i - 1];
        if (o->top != add->top) {
            continue;
        }
        if (SDL_fabsf(o->y - add->y) < tol) {
            float w = o->weight + add->weight;
            o->y = (o->y * o->weight + add->y * add->weight) / w;
            o->weight = w;
            o->xa = add->xa < o->xa ? add->xa : o->xa;
            o->xb = add->xb > o->xb ? add->xb : o->xb;
            return;
        }
    }
    e[(*n)++] = *add;
}

static int edge_cmp(const hint_edge *a, const hint_edge *b)
{
    if (a->y != b->y) {
        return a->y < b->y ? -1 : 1;
    }
    return (int)a->top - (int)b->top;
}

static float qx(const embi_qcurve *q, float t)
{
    float u = 1.0f - t;
    return u * u * q->x0 + 2.0f * u * t * q->cx + t * t * q->x1;
}

/* Collects the edges of the glyph. cw: the ink is to the right of the direction of travel. */
static uint32_t collect_edges(const embi_qcurve *in, uint32_t n, float tol, bool cw, const uint32_t *cid,
                              const hint_contour *ct, hint_edge *e)
{
    uint32_t ne = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        if (ct[cid[i]].dot) {
            continue;
        }
        hint_edge add;
        SDL_memset(&add, 0, sizeof add);
        if (SDL_fabsf(q->y0 - q->cy) <= tol && SDL_fabsf(q->cy - q->y1) <= tol && SDL_fabsf(q->y0 - q->y1) <= tol) {
            float dx = q->x1 - q->x0;
            /* A flat run narrower than tol is the notch where a spur meets a stem, not an edge. */
            if (SDL_fabsf(dx) < tol) {
                continue;
            }
            add.y = 0.5f * (q->y0 + q->y1);
            add.xa = q->x0 < q->x1 ? q->x0 : q->x1;
            add.xb = q->x0 < q->x1 ? q->x1 : q->x0;
            add.weight = SDL_fabsf(dx);
            add.top = (dx > 0.0f) == cw;
        } else {
            float den = q->y0 - 2.0f * q->cy + q->y1;
            if (SDL_fabsf(den) < 1e-6f) {
                continue;
            }
            float te = (q->y0 - q->cy) / den;
            if (te < 0.0f || te > 1.0f) {
                /* A curve fitted to a cubic leaves its extremum a little outside the span: an end with an almost horizontal
                 * tangent counts as the extremum. */
                const bool start = te < 0.0f;
                const float dy = start ? q->cy - q->y0 : q->y1 - q->cy, dx = start ? q->cx - q->x0 : q->x1 - q->cx;
                if (!(te > -HINT_NEAR_EXTREMUM && te < 1.0f + HINT_NEAR_EXTREMUM) ||
                    SDL_fabsf(dy) > HINT_FLAT_SLOPE * SDL_fabsf(dx)) {
                    continue;
                }
                te = start ? 0.0f : 1.0f;
            }
            float xd = (1.0f - te) * (q->cx - q->x0) + te * (q->x1 - q->cx);
            if (SDL_fabsf(xd) < 1e-3f * tol) {
                continue;
            }
            float u = 1.0f - te;
            add.y = u * u * q->y0 + 2.0f * u * te * q->cy + te * te * q->y1;
            /* The stretch of the curve within tol of the extremum. */
            float dt = SDL_sqrtf(tol / SDL_fabsf(den));
            float t0 = te - dt < 0.0f ? 0.0f : te - dt, t1 = te + dt > 1.0f ? 1.0f : te + dt;
            float xa = qx(q, t0), xb = qx(q, t1);
            add.xa = xa < xb ? xa : xb;
            add.xb = xa < xb ? xb : xa;
            add.weight = add.xb - add.xa + 1e-3f * tol;
            add.top = (xd > 0.0f) == cw;
        }
        edge_insert(e, &ne, &add, tol);
    }
    /* Sort by y (insertion sort: stable and deterministic). */
    for (uint32_t i = 1; i < ne; i++) {
        hint_edge v = e[i];
        uint32_t j = i;
        while (j > 0 && edge_cmp(&e[j - 1], &v) > 0) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = v;
    }
    return ne;
}

/* ------------------------------------------------------------------ the fit */

static bool zone_target(const embi_hint_zones *zs, const hint_edge *e, float s, float reach_px, float *target)
{
    static const int top_zones[] = {EMBI_HINT_XHEIGHT, EMBI_HINT_CAP, EMBI_HINT_ASCENDER, EMBI_HINT_FIGURES};
    static const int bottom_zones[] = {EMBI_HINT_BASELINE, EMBI_HINT_DESCENDER};
    const int *list = e->top ? top_zones : bottom_zones;
    const uint32_t count = e->top ? 4u : 2u;
    const float reach = reach_px / s;
    float best = 1e30f;
    int zone = -1;
    for (uint32_t i = 0; i < count; i++) {
        const int z = list[i];
        if ((zs->n_valid_mask & (1u << z)) == 0) {
            continue;
        }
        const embi_hint_zone *zz = &zs->z[z];
        float lo = zz->flat < zz->over ? zz->flat : zz->over, hi = zz->flat < zz->over ? zz->over : zz->flat;
        if (e->y < lo - reach || e->y > hi + reach) {
            continue;
        }
        float d = SDL_fabsf(e->y - zz->flat);
        float d2 = SDL_fabsf(e->y - zz->over);
        d = d2 < d ? d2 : d;
        if (d < best) {
            best = d;
            zone = z;
        }
    }
    if (zone < 0) {
        return false;
    }
    const embi_hint_zone *zz = &zs->z[zone];
    float t = rnd(zz->flat * s);
    if (SDL_fabsf(e->y - zz->over) < SDL_fabsf(e->y - zz->flat)) {
        t += rnd((zz->over - zz->flat) * s);
    }
    *target = t;
    return true;
}

static void edge_set(hint_edge *e, float target)
{
    e->anchored = true;
    e->target = target;
}

static float overlap_frac(const hint_edge *a, const hint_edge *b, float tol)
{
    float lo = a->xa > b->xa ? a->xa : b->xa;
    float hi = a->xb < b->xb ? a->xb : b->xb;
    float wa = a->xb - a->xa, wb = b->xb - b->xa;
    float w = wa < wb ? wa : wb;
    w = w < tol ? tol : w;
    return (hi - lo) / w;
}

static void drop_far(hint_edge *e, uint32_t ne, float s)
{
    for (uint32_t i = 0; i < ne; i++) {
        if (e[i].anchored && SDL_fabsf(e[i].target - e[i].y * s) > HINT_MAX_MOVE_PX) {
            e[i].anchored = false;
        }
    }
}

/* Splits the curves into contours (cid per curve) and finds the dots. Returns the number of contours. */
static uint32_t find_contours(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                              uint32_t *cid, hint_contour *ct)
{
    uint32_t nc = 0, start = 0;
    for (uint32_t i = 0; i < n; i++) {
        cid[i] = nc;
        if (i + 1 == n || in[i + 1].x0 != in[i].x1 || in[i + 1].y0 != in[i].y1) {
            hint_contour *c = &ct[nc++];
            SDL_memset(c, 0, sizeof *c);
            c->xa = c->xb = in[start].x0;
            embi_hint_extent(in + start, i + 1 - start, &c->ya, &c->yb);
            for (uint32_t k = start; k <= i; k++) {
                const float xs[3] = {in[k].x0, in[k].cx, in[k].x1};
                for (int j = 0; j < 3; j++) {
                    c->xa = xs[j] < c->xa ? xs[j] : c->xa;
                    c->xb = xs[j] > c->xb ? xs[j] : c->xb;
                }
            }
            start = i + 1;
        }
    }
    if (nc < 2) {
        return nc;
    }
    static const int top_zones[] = {EMBI_HINT_XHEIGHT, EMBI_HINT_CAP, EMBI_HINT_ASCENDER, EMBI_HINT_FIGURES};
    for (uint32_t k = 0; k < nc; k++) {
        hint_contour *c = &ct[k];
        if (!(c->yb - c->ya < HINT_DOT_MAX_EM * upem)) {
            continue;
        }
        bool apart = true;
        for (uint32_t j = 0; j < nc && apart; j++) {
            apart = j == k || c->xb < ct[j].xa || c->xa > ct[j].xb || c->yb < ct[j].ya || c->ya > ct[j].yb;
        }
        if (!apart) {
            continue;
        }
        const float h = (c->yb - c->ya) * s;
        const float H = rnd(h) < 1.0f ? 1.0f : rnd(h);
        /* The highest top zone value at or below the bottom of the dot is the row it keeps its distance from. */
        float ref = -1e30f;
        for (int i = 0; i < 4; i++) {
            const int z = top_zones[i];
            const float f = zones->z[z].flat;
            if ((zones->n_valid_mask & (1u << z)) != 0 && f <= c->ya && f > ref) {
                ref = f;
            }
        }
        float bottom;
        if (ref > -1e29f && (zones->n_valid_mask & (1u << EMBI_HINT_XHEIGHT)) != 0 &&
            c->ya > zones->z[EMBI_HINT_XHEIGHT].flat) {
            const float gap = (c->ya - ref) * s;
            const float G = gap >= 0.5f ? (rnd(gap) < 1.0f ? 1.0f : rnd(gap)) : 0.0f;
            bottom = rnd(ref * s) + G;
        } else if (c->yb < 0.0f) {
            const float gap = -c->yb * s;
            const float G = gap >= 0.5f ? (rnd(gap) < 1.0f ? 1.0f : rnd(gap)) : 0.0f;
            bottom = -G - H;
        } else {
            continue;
        }
        c->dot = true;
        c->shift_px = bottom - c->ya * s;
        c->scale = H / h;
    }
    return nc;
}

uint32_t embi_hint_glyph(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                         embi_qcurve *out)
{
    return embi_hint_glyph_map(zones, upem, s, in, n, out, nullptr);
}

static uint32_t hint_y(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                       embi_qcurve *out, uint32_t *map)
{
    if (n == 0 || n > HINT_MAX_QUADS || !(s > 0.0f) || !(upem > 0.0f) || !embi_hint_zones_usable(zones)) {
        return 0;
    }
    const float tol = upem / 256.0f;

    /* The side of the ink: the orientation of the whole glyph (the outer contours dominate the signed area). */
    double area = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        area += (double)q->x0 * q->cy - (double)q->cx * q->y0 + (double)q->cx * q->y1 - (double)q->x1 * q->cy;
    }
    if (area == 0.0) {
        return 0;
    }
    const bool cw = area < 0.0;

    const size_t cbytes = sizeof(hint_contour) * n + sizeof(uint32_t) * n;
    hint_contour *ct = embi_alloc(cbytes);
    uint32_t *cid = (uint32_t *)(ct + n);
    find_contours(zones, upem, s, in, n, cid, ct);
    hint_edge *e = embi_alloc(sizeof *e * n);
    uint32_t ne = collect_edges(in, n, tol, cw, cid, ct, e);
    uint32_t written = 0;
    if (ne == 0) {
        embi_free(e, sizeof *e * n);
        embi_free(ct, cbytes);
        return 0;
    }
    /* Under a few px of x-height only the zones are fitted. */
    const bool have_xh = (zones->n_valid_mask & (1u << EMBI_HINT_XHEIGHT)) != 0;
    const bool stems = have_xh ? zones->z[EMBI_HINT_XHEIGHT].flat * s >= HINT_STEM_MIN_XHEIGHT_PX
                               : zones->z[EMBI_HINT_CAP].flat * s >= 1.4f * HINT_STEM_MIN_XHEIGHT_PX;

    /* 1. Zones. */
    for (uint32_t i = 0; i < ne; i++) {
        float t;
        if (zone_target(zones, &e[i], s, stems ? HINT_ZONE_REACH_PX : HINT_ZONE_REACH_TINY_PX, &t)) {
            edge_set(&e[i], t);
        }
    }
    drop_far(e, ne, s);

    /* 2. Stems: each bottom edge with the nearest top edge above it that it overlaps. */
    for (uint32_t i = 0; i < ne; i++) {
        if (e[i].top || !stems) {
            continue;
        }
        int pair = -1;
        for (uint32_t j = i + 1; j < ne; j++) {
            if (!e[j].top || e[j].y - e[i].y <= tol) {
                continue;
            }
            if (e[j].y - e[i].y > HINT_STEM_MAX_EM * upem) {
                break;
            }
            if (overlap_frac(&e[i], &e[j], tol) >= 0.3f) {
                pair = (int)j;
                break;
            }
        }
        if (pair < 0) {
            continue;
        }
        hint_edge *b = &e[i], *t = &e[pair];
        float w = (t->y - b->y) * s;
        float W = rnd(w);
        W = W < 1.0f ? 1.0f : W;
        if (SDL_fabsf(W - w) > 0.5f) {
            continue;
        }
        if (b->anchored && t->anchored) {
            continue;
        }
        if (b->anchored) {
            edge_set(t, b->target + W);
        } else if (t->anchored) {
            edge_set(b, t->target - W);
        } else {
            float centre = 0.5f * (b->y + t->y) * s;
            /* The integer bottom row that moves the centre least; a tie goes down. */
            float lo = SDL_floorf(centre - 0.5f * W);
            float c0 = SDL_fabsf(lo + 0.5f * W - centre), c1 = SDL_fabsf(lo + 1.0f + 0.5f * W - centre);
            float bt = c1 < c0 ? lo + 1.0f : lo;
            edge_set(b, bt);
            edge_set(t, bt + W);
        }
    }
    drop_far(e, ne, s);

    /* 3. Order: the targets of the anchors must grow with their edges. */
    uint32_t na = 0;
    float prev = -1e30f;
    bool prev_top = false;
    float *ay = embi_alloc(sizeof(float) * ne * 3u);
    float *at = ay + ne, *ap = ay + 2u * ne;
    for (uint32_t i = 0; i < ne; i++) {
        if (!e[i].anchored) {
            continue;
        }
        /* Two top edges of one zone (the flat top of a stem and the overshoot of a bowl) may share a row. */
        if (e[i].target < prev || (e[i].target == prev && !(e[i].top && prev_top))) {
            continue;
        }
        prev = e[i].target;
        prev_top = e[i].top;
        ay[na] = e[i].y;
        at[na] = e[i].target;
        na++;
    }
    if (na == 0) {
        embi_free(ay, sizeof(float) * ne * 3u);
        embi_free(e, sizeof *e * n);
        embi_free(ct, cbytes);
        return 0;
    }
    /* Points within ap of an anchor go to its target exactly, so a flat segment stays flat. */
    for (uint32_t i = 0; i < na; i++) {
        float gap = 1e30f;
        if (i > 0) {
            gap = ay[i] - ay[i - 1];
        }
        if (i + 1 < na && ay[i + 1] - ay[i] < gap) {
            gap = ay[i + 1] - ay[i];
        }
        ap[i] = 0.45f * gap < tol ? 0.45f * gap : tol;
    }

    /* 4. Cut the curves at their vertical extrema, then map every point. */
    const float inv = 1.0f / s;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        const uint32_t first = written;
        if (map != nullptr) {
            map[i] = written;
        }
        float den = q->y0 - 2.0f * q->cy + q->y1;
        float t = SDL_fabsf(den) > 1e-9f ? (q->y0 - q->cy) / den : -1.0f;
        if (t > 1e-4f && t < 1.0f - 1e-4f) {
            float ax = q->x0 + t * (q->cx - q->x0), bx = q->cx + t * (q->x1 - q->cx);
            float mx = ax + t * (bx - ax);
            float u = 1.0f - t;
            float ym = u * u * q->y0 + 2.0f * u * t * q->cy + t * t * q->y1;
            out[written++] = (embi_qcurve){q->x0, q->y0, ax, ym, mx, ym};
            out[written++] = (embi_qcurve){mx, ym, bx, ym, q->x1, q->y1};
        } else {
            out[written++] = *q;
        }
        const hint_contour *dc = &ct[cid[i]];
        for (uint32_t w = first; w < written; w++) {
            float *ys[3] = {&out[w].y0, &out[w].cy, &out[w].y1};
            for (int k = 0; k < 3; k++) {
                const float y = *ys[k];
                float px;
                if (dc->dot) {
                    /* A dot keeps its shape: its bottom goes to a row and its height to a whole number of px. */
                    px = (dc->ya + (y - dc->ya) * dc->scale) * s + dc->shift_px;
                    *ys[k] = px * inv;
                    continue;
                }
                uint32_t a = 0;
                bool on = false;
                for (; a < na; a++) {
                    if (SDL_fabsf(y - ay[a]) <= ap[a]) {
                        on = true;
                        break;
                    }
                }
                if (on) {
                    px = at[a];
                } else if (y < ay[0]) {
                    px = at[0] + (y - ay[0]) * s;
                } else if (y > ay[na - 1]) {
                    px = at[na - 1] + (y - ay[na - 1]) * s;
                } else {
                    uint32_t j = 0;
                    while (j + 2 < na && y > ay[j + 1]) {
                        j++;
                    }
                    px = at[j] + (y - ay[j]) * (at[j + 1] - at[j]) / (ay[j + 1] - ay[j]);
                }
                *ys[k] = px * inv;
            }
        }
    }
    if (map != nullptr) {
        map[n] = written;
    }
    embi_free(ay, sizeof(float) * ne * 3u);
    embi_free(e, sizeof *e * n);
    embi_free(ct, cbytes);
    return written;
}

uint32_t embi_hint_glyph_map(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                             embi_qcurve *out, uint32_t *map)
{
    return hint_y(zones, upem, s, in, n, out, map);
}

/* ------------------------------------------------------------------ x fitting */

/* Fits the vertical stems of the n curves. The curves are swapped (x for y) so that the edge finder of the horizontal case
 * serves: a left edge of the glyph is then a bottom edge, a right edge a top edge. out has room for 2 * n curves, map for n + 1
 * entries. Returns the number of curves written, or 0 when no stem was fitted. */
static uint32_t hint_x(float upem, float s, const embi_qcurve *in, uint32_t n, embi_qcurve *out, uint32_t *map)
{
    const float tol = upem / 256.0f;
    double area = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        area += (double)q->x0 * q->cy - (double)q->cx * q->y0 + (double)q->cx * q->y1 - (double)q->x1 * q->cy;
    }
    if (area == 0.0) {
        return 0;
    }
    const bool cw = area < 0.0;

    const size_t bytes = (sizeof(embi_qcurve) + sizeof(hint_edge) + sizeof(uint32_t)) * n + sizeof(hint_contour);
    embi_qcurve *sw = embi_alloc(bytes);
    hint_edge *e = (hint_edge *)(sw + n);
    uint32_t *cid = (uint32_t *)(e + n);
    hint_contour *ct = (hint_contour *)(cid + n);
    SDL_memset(cid, 0, sizeof(uint32_t) * n);
    SDL_memset(ct, 0, sizeof *ct);
    for (uint32_t i = 0; i < n; i++) {
        sw[i] = (embi_qcurve){in[i].y0, in[i].x0, in[i].cy, in[i].cx, in[i].y1, in[i].x1};
    }
    /* Swapping the axes reverses the orientation. */
    const uint32_t ne = collect_edges(sw, n, tol, !cw, cid, ct, e);

    const size_t abytes = sizeof(float) * (ne + 1u) * 3u;
    float *ay = embi_alloc(abytes);
    float *at = ay + ne + 1u, *ap = at + ne + 1u;
    uint32_t na = 0;
    float last_orig = -1e30f, last_target = -1e30f;
    for (uint32_t i = 0; i < ne; i++) {
        if (e[i].top) {
            continue;
        }
        int pair = -1;
        for (uint32_t j = i + 1; j < ne; j++) {
            if (!e[j].top || e[j].y - e[i].y <= tol) {
                continue;
            }
            if (e[j].y - e[i].y > HINT_STEM_MAX_EM * upem) {
                break;
            }
            if (overlap_frac(&e[i], &e[j], tol) >= 0.3f) {
                pair = (int)j;
                break;
            }
        }
        if (pair < 0) {
            continue;
        }
        const hint_edge *l = &e[i], *r = &e[pair];
        /* A stem inside the previous one (the dot of i beside its stem) or touching it is left to the interpolation. */
        if (l->y - last_orig < tol) {
            continue;
        }
        const float w = (r->y - l->y) * s;
        float W = rnd(w);
        W = W < 1.0f ? 1.0f : W;
        const float centre = 0.5f * (l->y + r->y) * s;
        /* The integer left column that moves the centre least; a tie goes left. */
        const float lo = SDL_floorf(centre - 0.5f * W);
        const float c0 = SDL_fabsf(lo + 0.5f * W - centre), c1 = SDL_fabsf(lo + 1.0f + 0.5f * W - centre);
        float lt = c1 < c0 ? lo + 1.0f : lo;
        /* The gap to the previous stem stays a gap (one px when it was half a px or more). */
        if (na > 0) {
            const float gap_px = (l->y - last_orig) * s;
            const float need = last_target + (gap_px >= 0.5f ? 1.0f : 0.0f);
            lt = lt < need ? need : lt;
        }
        if (SDL_fabsf(lt - l->y * s) > 1.0f || SDL_fabsf(lt + W - r->y * s) > 1.0f) {
            continue;
        }
        ay[na] = l->y;
        at[na++] = lt;
        ay[na] = r->y;
        at[na++] = lt + W;
        last_orig = r->y;
        last_target = lt + W;
    }
    if (na == 0) {
        embi_free(ay, abytes);
        embi_free(sw, bytes);
        return 0;
    }
    for (uint32_t i = 0; i < na; i++) {
        float gap = 1e30f;
        if (i > 0) {
            gap = ay[i] - ay[i - 1];
        }
        if (i + 1 < na && ay[i + 1] - ay[i] < gap) {
            gap = ay[i + 1] - ay[i];
        }
        ap[i] = 0.45f * gap < tol ? 0.45f * gap : tol;
    }

    const float inv = 1.0f / s;
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        const uint32_t first = written;
        map[i] = written;
        float den = q->x0 - 2.0f * q->cx + q->x1;
        float t = SDL_fabsf(den) > 1e-9f ? (q->x0 - q->cx) / den : -1.0f;
        if (t > 1e-4f && t < 1.0f - 1e-4f) {
            float ya = q->y0 + t * (q->cy - q->y0), yb = q->cy + t * (q->y1 - q->cy);
            float ym = ya + t * (yb - ya);
            float u = 1.0f - t;
            float xm = u * u * q->x0 + 2.0f * u * t * q->cx + t * t * q->x1;
            out[written++] = (embi_qcurve){q->x0, q->y0, xm, ya, xm, ym};
            out[written++] = (embi_qcurve){xm, ym, xm, yb, q->x1, q->y1};
        } else {
            out[written++] = *q;
        }
        for (uint32_t w = first; w < written; w++) {
            float *xs[3] = {&out[w].x0, &out[w].cx, &out[w].x1};
            for (int k = 0; k < 3; k++) {
                const float x = *xs[k];
                float px;
                uint32_t a = 0;
                bool on = false;
                for (; a < na; a++) {
                    if (SDL_fabsf(x - ay[a]) <= ap[a]) {
                        on = true;
                        break;
                    }
                }
                if (on) {
                    px = at[a];
                } else if (x < ay[0]) {
                    px = at[0] + (x - ay[0]) * s;
                } else if (x > ay[na - 1]) {
                    px = at[na - 1] + (x - ay[na - 1]) * s;
                } else {
                    uint32_t j = 0;
                    while (j + 2 < na && x > ay[j + 1]) {
                        j++;
                    }
                    px = at[j] + (x - ay[j]) * (at[j + 1] - at[j]) / (ay[j + 1] - ay[j]);
                }
                *xs[k] = px * inv;
            }
        }
    }
    map[n] = written;
    embi_free(ay, abytes);
    embi_free(sw, bytes);
    return written;
}

uint32_t embi_hint_glyph_fit(const embi_hint_zones *zones, float upem, float s, const embi_qcurve *in, uint32_t n,
                             embi_qcurve *out, uint32_t *map, bool fit_x)
{
    if (!fit_x) {
        return zones != nullptr ? hint_y(zones, upem, s, in, n, out, map) : 0;
    }
    if (n == 0 || n > HINT_MAX_QUADS || !(s > 0.0f) || !(upem > 0.0f)) {
        return 0;
    }
    const size_t mbytes = sizeof(uint32_t) * (n + 1u);
    uint32_t *map1 = embi_alloc(mbytes);
    /* The y fit goes first; x then works on its curves (or on the plain ones when y fitted nothing). */
    const uint32_t my = zones != nullptr ? hint_y(zones, upem, s, in, n, out, map1) : 0;
    uint32_t m = my;
    if (my == 0) {
        SDL_memcpy(out, in, sizeof *in * n);
        for (uint32_t i = 0; i <= n; i++) {
            map1[i] = i;
        }
        m = n;
    }
    const size_t obytes = sizeof(embi_qcurve) * 2u * m + sizeof(uint32_t) * (m + 1u);
    embi_qcurve *tmp = embi_alloc(obytes);
    uint32_t *map2 = (uint32_t *)(tmp + 2u * m);
    const uint32_t mx = hint_x(upem, s, out, m, tmp, map2);
    uint32_t result = my;
    if (mx != 0) {
        SDL_memcpy(out, tmp, sizeof *tmp * mx);
        result = mx;
        for (uint32_t i = 0; map != nullptr && i <= n; i++) {
            map[i] = map2[map1[i]];
        }
    } else if (map != nullptr && my != 0) {
        SDL_memcpy(map, map1, mbytes);
    }
    embi_free(tmp, obytes);
    embi_free(map1, mbytes);
    return result;
}

/* ------------------------------------------------------------------ pixel-art glyphs */

/* The tolerance of the grid, font units. */
#define PIXEL_TOL (1.0 / 64.0)
/* The most steps a glyph may span, per axis: a finer grid is a text face that happens to have straight outlines. */
#define PIXEL_MAX_STEPS 64.0

/* The step common to a and b (both positive), by Euclid with remainders under PIXEL_TOL taken as zero. */
static double grid_gcd(double a, double b)
{
    if (a < b) {
        const double t = a;
        a = b;
        b = t;
    }
    while (b > PIXEL_TOL) {
        double r = SDL_fmod(a, b);
        r = r > 0.5 * b ? b - r : r;
        a = b;
        b = r;
    }
    return a;
}

bool embi_hint_pixel_grid(const embi_qcurve *c, uint32_t n, float *unit)
{
    if (n == 0) {
        return false;
    }
    const float tolf = (float)PIXEL_TOL;
    for (uint32_t i = 0; i < n; i++) {
        const bool horiz = SDL_fabsf(c[i].y0 - c[i].y1) <= tolf && SDL_fabsf(c[i].cy - c[i].y0) <= tolf;
        const bool vert = SDL_fabsf(c[i].x0 - c[i].x1) <= tolf && SDL_fabsf(c[i].cx - c[i].x0) <= tolf;
        if (!horiz && !vert) {
            return false;
        }
    }
    /* The step: the common divisor of the distances of every end point from the first, per axis. */
    const double rx = c[0].x0, ry = c[0].y0;
    double g = 0.0, span = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const double d[4] = {SDL_fabs(c[i].x0 - rx), SDL_fabs(c[i].x1 - rx), SDL_fabs(c[i].y0 - ry), SDL_fabs(c[i].y1 - ry)};
        for (int k = 0; k < 4; k++) {
            span = d[k] > span ? d[k] : span;
            if (d[k] > PIXEL_TOL) {
                g = g == 0.0 ? d[k] : grid_gcd(g, d[k]);
            }
        }
    }
    if (g < 1.0 || span / g > PIXEL_MAX_STEPS) {
        return false;
    }
    /* Every distance is a whole multiple of the step found. */
    for (uint32_t i = 0; i < n; i++) {
        const double d[4] = {SDL_fabs(c[i].x0 - rx), SDL_fabs(c[i].x1 - rx), SDL_fabs(c[i].y0 - ry), SDL_fabs(c[i].y1 - ry)};
        for (int k = 0; k < 4; k++) {
            const double q = d[k] / g;
            if (SDL_fabs(q - SDL_floor(q + 0.5)) * g > PIXEL_TOL * 2.0) {
                return false;
            }
        }
    }
    if (unit != nullptr) {
        *unit = (float)g;
    }
    return true;
}

float embi_hint_pixel_scale(float unit, float scale, bool sharp)
{
    if (!(unit > 0.0f) || !(scale > 0.0f)) {
        return 0.0f;
    }
    const float px = unit * scale;
    float k = rnd(px);
    if (sharp) {
        if (px < 0.5f) {
            return 0.0f;
        }
        k = k < 1.0f ? 1.0f : k;
    } else if (k < 1.0f || SDL_fabsf(px - k) > 0.02f) {
        return 0.0f;
    }
    return k / unit;
}

void embi_hint_pixel_snap(const embi_qcurve *in, uint32_t n, float s, embi_qcurve *out)
{
    const float inv = 1.0f / s;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &in[i];
        embi_qcurve o = {rnd(q->x0 * s) * inv, rnd(q->y0 * s) * inv, 0.0f, 0.0f, rnd(q->x1 * s) * inv, rnd(q->y1 * s) * inv};
        /* The control point stays on its segment: the shared coordinate of the end points, the middle of the other. */
        const bool vert = SDL_fabsf(q->x0 - q->x1) <= (float)PIXEL_TOL && SDL_fabsf(q->cx - q->x0) <= (float)PIXEL_TOL;
        o.cx = vert ? o.x0 : 0.5f * (o.x0 + o.x1);
        o.cy = vert ? 0.5f * (o.y0 + o.y1) : o.y0;
        out[i] = o;
    }
}

bool embi_hint_pixel_on_grid(const embi_qcurve *c, uint32_t n, float unit)
{
    if (n == 0 || !(unit >= 1.0f)) {
        return false;
    }
    const float tolf = (float)PIXEL_TOL;
    const double rx = c[0].x0, ry = c[0].y0;
    for (uint32_t i = 0; i < n; i++) {
        const bool horiz = SDL_fabsf(c[i].y0 - c[i].y1) <= tolf && SDL_fabsf(c[i].cy - c[i].y0) <= tolf;
        const bool vert = SDL_fabsf(c[i].x0 - c[i].x1) <= tolf && SDL_fabsf(c[i].cx - c[i].x0) <= tolf;
        if (!horiz && !vert) {
            return false;
        }
        const double d[4] = {SDL_fabs(c[i].x0 - rx), SDL_fabs(c[i].x1 - rx), SDL_fabs(c[i].y0 - ry), SDL_fabs(c[i].y1 - ry)};
        for (int k = 0; k < 4; k++) {
            const double q = d[k] / unit;
            if (SDL_fabs(q - SDL_floor(q + 0.5)) * unit > PIXEL_TOL * 2.0) {
                return false;
            }
        }
    }
    return true;
}

bool embi_hint_pixel_face(const embi_qcurve *const *glyphs, const uint32_t *counts, uint32_t ng, float upem, float *unit)
{
    double g = 0.0;
    for (uint32_t i = 0; i < ng; i++) {
        float u;
        if (counts[i] == 0) {
            continue;
        }
        if (!embi_hint_pixel_grid(glyphs[i], counts[i], &u)) {
            return false;
        }
        g = g == 0.0 ? (double)u : grid_gcd(g, (double)u);
    }
    if (g < 1.0) {
        return false;
    }
    for (uint32_t i = 0; i < ng; i++) {
        if (counts[i] != 0 && !embi_hint_pixel_on_grid(glyphs[i], counts[i], (float)g)) {
            return false;
        }
    }
    const double steps = (double)upem / g;
    if (steps < 5.0 - 1e-3 || steps > 64.0 + 1e-3) {
        return false;
    }
    if (unit != nullptr) {
        *unit = (float)g;
    }
    return true;
}
