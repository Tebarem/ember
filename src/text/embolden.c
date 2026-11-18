/*
 * Outline emboldening (see embolden.h). A contour is treated as its control polygon: the start and the control point of
 * every curve, in order. Each point moves along the bisector of its two edge normals.
 */

#include "text/embolden.h"

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

/* The miter limit: a corner moves its point at most this many half strengths. */
#define EMBI_MITER_LIMIT 4.0f
/* Edges shorter than this (font units) have no direction. */
#define EMBI_EDGE_EPS 1e-6f
/* A corner whose 1 + cos of the turn is below this folds back on its edge: it takes the incoming edge's normal. */
#define EMBI_FOLD_EPS 1e-4f
/* The shift of a hole is at most this fraction of the smaller extent of its bounding box (a narrow counter does not invert). */
#define EMBI_HOLE_CLAMP 0.45f

typedef struct embo_pt {
    float x, y;
} embo_pt;

/* Per contour: the push sign (+1 pushes along the right normal of the travel direction, -1 along the left), whether it is a
 * hole, and the largest shift a hole point may take. */
typedef struct embo_ct {
    float push;
    bool hole;
    float clamp;
} embo_ct;

/* Point q of the contour of cnt curves that starts at curve s: even points are curve starts, odd points are controls. */
static embo_pt raw_point(const embi_qcurve *in, uint32_t s, uint32_t cnt, uint32_t q)
{
    const uint32_t m = 2u * cnt;
    q %= m;
    const embi_qcurve *c = &in[s + q / 2u];
    return (q & 1u) != 0 ? (embo_pt){c->cx, c->cy} : (embo_pt){c->x0, c->y0};
}

static uint32_t contour_len(uint32_t n, const uint32_t *starts, uint32_t nc, uint32_t c)
{
    return (c + 1 < nc ? starts[c + 1] : n) - starts[c];
}

/* The unit normal of the edge a to b, pushed along sgn times its right normal. False for a zero-length edge. */
static bool edge_normal(embo_pt a, embo_pt b, float sgn, float nrm[2])
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = SDL_sqrtf(dx * dx + dy * dy);
    if (!(len > EMBI_EDGE_EPS)) {
        return false;
    }
    nrm[0] = sgn * dy / len;
    nrm[1] = -sgn * dx / len;
    return true;
}

/* The shifted position of point q of a contour (the centred shift, the hole clamp, then the bottom-left anchor). */
static embo_pt shifted_point(const embi_qcurve *in, uint32_t s, uint32_t cnt, uint32_t q, const embo_ct *ct, float hx,
                             float hy, bool anchor)
{
    const uint32_t m = 2u * cnt;
    const embo_pt p = raw_point(in, s, cnt, q);
    const embo_pt a = raw_point(in, s, cnt, q + m - 1u);
    const embo_pt b = raw_point(in, s, cnt, q + 1u);
    float n1[2] = {0.0f, 0.0f}, n2[2] = {0.0f, 0.0f};
    const bool ok1 = edge_normal(a, p, ct->push, n1);
    const bool ok2 = edge_normal(p, b, ct->push, n2);
    if (!ok1 && !ok2) {
        return p;
    }
    if (!ok1) {
        n1[0] = n2[0];
        n1[1] = n2[1];
    }
    if (!ok2) {
        n2[0] = n1[0];
        n2[1] = n1[1];
    }
    const float c = n1[0] * n2[0] + n1[1] * n2[1];
    float vx, vy;
    if (1.0f + c < EMBI_FOLD_EPS) {
        /* The corner folds back on its edge: the incoming edge's normal, shifted by the half strength. */
        vx = n1[0] * hx;
        vy = n1[1] * hy;
    } else {
        const float k = 1.0f / (1.0f + c);
        vx = (n1[0] + n2[0]) * hx * k;
        vy = (n1[1] + n2[1]) * hy * k;
        /* The miter: a sharp corner is capped at EMBI_MITER_LIMIT half strengths. */
        const float lim = EMBI_MITER_LIMIT * (hx > hy ? hx : hy);
        const float len = SDL_sqrtf(vx * vx + vy * vy);
        if (len > lim) {
            vx *= lim / len;
            vy *= lim / len;
        }
    }
    if (ct->hole) {
        const float len = SDL_sqrtf(vx * vx + vy * vy);
        if (len > ct->clamp) {
            vx *= ct->clamp / len;
            vy *= ct->clamp / len;
        }
    }
    if (anchor) {
        vx += hx;
        vy += hy;
    }
    return (embo_pt){p.x + vx, p.y + vy};
}

/* Even-odd: is p inside the control polygon of contour d (curves [s, s + cnt)). */
static bool inside_contour(embo_pt p, const embi_qcurve *in, uint32_t s, uint32_t cnt)
{
    bool in_poly = false;
    const uint32_t m = 2u * cnt;
    for (uint32_t q = 0; q < m; q++) {
        const embo_pt a = raw_point(in, s, cnt, q), b = raw_point(in, s, cnt, q + 1u);
        if ((a.y > p.y) != (b.y > p.y)) {
            const float xi = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (p.x < xi) {
                in_poly = !in_poly;
            }
        }
    }
    return in_poly;
}

void embi_embolden(const embi_qcurve *in, uint32_t n, const uint32_t *starts, uint32_t nc, float xstr, float ystr, bool anchor_bl,
                   embi_qcurve *out)
{
    if (in == nullptr || out == nullptr || n == 0) {
        return;
    }
    static const uint32_t whole[1] = {0};
    if (starts == nullptr || nc == 0) {
        starts = whole;
        nc = 1;
    }
    embo_ct *ct = embi_alloc(sizeof(embo_ct) * nc);
    /* Orientation and extent of every contour, then its role by nesting. */
    for (uint32_t c = 0; c < nc; c++) {
        const uint32_t cnt = contour_len(n, starts, nc, c) ;
        const uint32_t m = 2u * cnt;
        float area = 0.0f, x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
        for (uint32_t q = 0; q < m; q++) {
            const embo_pt p = raw_point(in, starts[c], cnt, q), r = raw_point(in, starts[c], cnt, q + 1u);
            area += p.x * r.y - r.x * p.y;
            if (q == 0 || p.x < x0) {
                x0 = p.x;
            }
            if (q == 0 || p.x > x1) {
                x1 = p.x;
            }
            if (q == 0 || p.y < y0) {
                y0 = p.y;
            }
            if (q == 0 || p.y > y1) {
                y1 = p.y;
            }
        }
        const float sgn = area < 0.0f ? -1.0f : 1.0f;
        int depth = 0;
        const embo_pt probe = raw_point(in, starts[c], cnt, 0);
        for (uint32_t d = 0; d < nc; d++) {
            if (d != c && inside_contour(probe, in, starts[d], contour_len(n, starts, nc, d))) {
                depth++;
            }
        }
        ct[c].hole = (depth & 1) != 0;
        /* Outer contours push out (the right normal for a counter-clockwise contour); holes push in. */
        ct[c].push = (ct[c].hole ? -1.0f : 1.0f) * sgn;
        const float ext = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
        ct[c].clamp = EMBI_HOLE_CLAMP * ext;
    }
    const float hx = 0.5f * xstr, hy = 0.5f * ystr;
    for (uint32_t c = 0; c < nc; c++) {
        const uint32_t cnt = contour_len(n, starts, nc, c);
        const uint32_t m = 2u * cnt;
        for (uint32_t k = 0; k < cnt; k++) {
            const embo_pt p0 = shifted_point(in, starts[c], cnt, 2u * k, &ct[c], hx, hy, anchor_bl);
            const embo_pt pc = shifted_point(in, starts[c], cnt, 2u * k + 1u, &ct[c], hx, hy, anchor_bl);
            const embo_pt p1 = shifted_point(in, starts[c], cnt, (2u * k + 2u) % m, &ct[c], hx, hy, anchor_bl);
            out[starts[c] + k] = (embi_qcurve){p0.x, p0.y, pc.x, pc.y, p1.x, p1.y};
        }
    }
    embi_free(ct, sizeof(embo_ct) * nc);
}
