/*
 * Outline builder: contours of quadratic segments, cubic to quadratic subdivision, tight bounds,
 * and affine appends used to flatten composite glyphs.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_CUBIC_MAX_DEPTH 12

void embi_olb_init(embi_olb *b, embi_outline *o)
{
    b->o = o;
    b->cur = (embi_pt){0, 0};
    b->start = (embi_pt){0, 0};
    b->open = false;
}

static bool pt_eq(embi_pt a, embi_pt b)
{
    return a.x == b.x && a.y == b.y;
}

/* Largest outline one glyph may produce (16 MB of quads). Real glyphs use a few thousand. Cubic
 * subdivision can multiply a hostile charstring by 4096, so the cap keeps that bounded. */
#define EMBI_OUTLINE_MAX_QUADS (1u << 20)

static bool olb_push(embi_outline *o, embi_quad q)
{
    if (o->quads.len >= EMBI_OUTLINE_MAX_QUADS) {
        return false;
    }
    embi_vec_push(&o->quads, q);
    return true;
}

bool embi_olb_close(embi_olb *b)
{
    if (!b->open) {
        return true;
    }
    bool ok = true;
    if (!pt_eq(b->cur, b->start)) {
        embi_pt mid = {(b->cur.x + b->start.x) * 0.5f, (b->cur.y + b->start.y) * 0.5f};
        embi_quad q = {b->cur, mid, b->start};
        ok = olb_push(b->o, q);
        b->cur = b->start;
    }
    /* A contour that produced no segments is dropped, keeping contour indices valid. */
    embi_outline *o = b->o;
    if (o->contours.len > 0 && o->contours.data[o->contours.len - 1] == o->quads.len) {
        o->contours.len--;
    }
    b->open = false;
    return ok;
}

bool embi_olb_move(embi_olb *b, embi_pt p)
{
    if (!embi_olb_close(b)) {
        return false;
    }
    if (b->o->contours.len == UINT32_MAX) {
        return false;
    }
    embi_vec_push(&b->o->contours, b->o->quads.len);
    b->cur = p;
    b->start = p;
    b->open = true;
    return true;
}

bool embi_olb_quad(embi_olb *b, embi_pt c, embi_pt p)
{
    if (!b->open) {
        return false;
    }
    if (pt_eq(b->cur, c) && pt_eq(c, p)) {
        return true;
    }
    embi_quad q = {b->cur, c, p};
    if (!olb_push(b->o, q)) {
        return false;
    }
    b->cur = p;
    return true;
}

bool embi_olb_line(embi_olb *b, embi_pt p)
{
    if (!b->open) {
        return false;
    }
    if (pt_eq(b->cur, p)) {
        return true;
    }
    embi_pt mid = {(b->cur.x + p.x) * 0.5f, (b->cur.y + p.y) * 0.5f};
    return embi_olb_quad(b, mid, p);
}

/* Cubic point at t. */
static embi_pt cubic_at(embi_pt p0, embi_pt p1, embi_pt p2, embi_pt p3, float t)
{
    float u = 1.0f - t;
    float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
    return (embi_pt){a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

static embi_pt quad_at(embi_pt p0, embi_pt p1, embi_pt p2, float t)
{
    float u = 1.0f - t;
    float a = u * u, b = 2.0f * u * t, c = t * t;
    return (embi_pt){a * p0.x + b * p1.x + c * p2.x, a * p0.y + b * p1.y + c * p2.y};
}

static bool cubic_rec(embi_olb *b, embi_pt p0, embi_pt p1, embi_pt p2, embi_pt p3, float tol,
                      int depth)
{
    /* Single-quad candidate: control point from the cubic's end tangents. */
    embi_pt q = {(3.0f * (p1.x + p2.x) - p0.x - p3.x) * 0.25f,
                 (3.0f * (p1.y + p2.y) - p0.y - p3.y) * 0.25f};
    float err2 = 0.0f;
    /* The single-quad error peaks at t = (1 -+ 1/sqrt(3)) / 2; sample there and at the middle. */
    static const float samples[3] = {0.2113249f, 0.5f, 0.7886751f};
    for (int i = 0; i < 3; i++) {
        embi_pt c = cubic_at(p0, p1, p2, p3, samples[i]);
        embi_pt d = quad_at(p0, q, p3, samples[i]);
        float dx = c.x - d.x, dy = c.y - d.y;
        float e2 = dx * dx + dy * dy;
        if (e2 > err2) {
            err2 = e2;
        }
    }
    if (err2 <= tol * tol || depth >= EMBI_CUBIC_MAX_DEPTH) {
        return embi_olb_quad(b, q, p3);
    }
    /* De Casteljau split at t = 1/2. */
    embi_pt m01 = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    embi_pt m12 = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    embi_pt m23 = {(p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f};
    embi_pt m012 = {(m01.x + m12.x) * 0.5f, (m01.y + m12.y) * 0.5f};
    embi_pt m123 = {(m12.x + m23.x) * 0.5f, (m12.y + m23.y) * 0.5f};
    embi_pt mid = {(m012.x + m123.x) * 0.5f, (m012.y + m123.y) * 0.5f};
    return cubic_rec(b, p0, m01, m012, mid, tol, depth + 1) &&
           cubic_rec(b, mid, m123, m23, p3, tol, depth + 1);
}

bool embi_olb_cubic(embi_olb *b, embi_pt p1, embi_pt p2, embi_pt p3, float tol)
{
    if (!b->open) {
        return false;
    }
    if (pt_eq(b->cur, p1) && pt_eq(p1, p2) && pt_eq(p2, p3)) {
        return true;
    }
    return cubic_rec(b, b->cur, p1, p2, p3, tol, 0);
}

/* Tight bound of one quad: endpoints plus the curve's extrema on each axis. */
static void quad_bounds(const embi_quad *q, float *x0, float *y0, float *x1, float *y1)
{
    embi_pt pts[4];
    int n = 0;
    pts[n++] = q->p0;
    pts[n++] = q->p2;
    float dx = q->p0.x - 2.0f * q->p1.x + q->p2.x;
    if (dx != 0.0f) {
        float t = (q->p0.x - q->p1.x) / dx;
        if (t > 0.0f && t < 1.0f) {
            pts[n++] = quad_at(q->p0, q->p1, q->p2, t);
        }
    }
    float dy = q->p0.y - 2.0f * q->p1.y + q->p2.y;
    if (dy != 0.0f) {
        float t = (q->p0.y - q->p1.y) / dy;
        if (t > 0.0f && t < 1.0f) {
            pts[n++] = quad_at(q->p0, q->p1, q->p2, t);
        }
    }
    for (int i = 0; i < n; i++) {
        if (pts[i].x < *x0) {
            *x0 = pts[i].x;
        }
        if (pts[i].x > *x1) {
            *x1 = pts[i].x;
        }
        if (pts[i].y < *y0) {
            *y0 = pts[i].y;
        }
        if (pts[i].y > *y1) {
            *y1 = pts[i].y;
        }
    }
}

bool embi_olb_finish(embi_olb *b)
{
    bool ok = embi_olb_close(b);
    embi_outline *o = b->o;
    if (o->quads.len == 0) {
        o->xmin = o->ymin = o->xmax = o->ymax = 0.0f;
        return ok;
    }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < o->quads.len; i++) {
        quad_bounds(&o->quads.data[i], &x0, &y0, &x1, &y1);
    }
    o->xmin = x0;
    o->ymin = y0;
    o->xmax = x1;
    o->ymax = y1;
    return ok;
}

bool embi_outline_append_xf(embi_outline *dst, const embi_outline *src, const float xf[6])
{
    uint32_t base = dst->quads.len;
    if ((uint64_t)dst->quads.len + src->quads.len > UINT32_MAX ||
        (uint64_t)dst->contours.len + src->contours.len > UINT32_MAX) {
        return false;
    }
    embi_vec_reserve(&dst->quads, dst->quads.len + src->quads.len);
    embi_vec_reserve(&dst->contours, dst->contours.len + src->contours.len);
    for (uint32_t i = 0; i < src->quads.len; i++) {
        embi_quad q = src->quads.data[i];
        embi_pt *pts[3] = {&q.p0, &q.p1, &q.p2};
        for (int k = 0; k < 3; k++) {
            float x = pts[k]->x, y = pts[k]->y;
            pts[k]->x = xf[0] * x + xf[2] * y + xf[4];
            pts[k]->y = xf[1] * x + xf[3] * y + xf[5];
        }
        embi_vec_push(&dst->quads, q);
    }
    for (uint32_t i = 0; i < src->contours.len; i++) {
        embi_vec_push(&dst->contours, base + src->contours.data[i]);
    }
    return true;
}

void embi_outline_free(embi_outline *o)
{
    if (o == nullptr) {
        return;
    }
    embi_vec_free(&o->quads);
    embi_vec_free(&o->contours);
    o->xmin = o->ymin = o->xmax = o->ymax = 0.0f;
}
