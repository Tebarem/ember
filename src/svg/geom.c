/*
 * Path geometry for the SVG module (R4c): the wrappers of the gpu module's paths and strokes, and the queries (box, hit,
 * length) that the SVG DOM and hit testing need. See geom.h for why this is a file of its own.
 */
#include "svg/geom.h"

#include "base/base.h"
#include "gpu/path.h"
#include "gpu/stroke.h"

#include <math.h>
#include <string.h>

embi_path *svgp_new(float tol)
{
    return embi_path_create(tol);
}

void svgp_free(embi_path *p)
{
    embi_path_destroy(p);
}

bool svgp_move(embi_path *p, float x, float y)
{
    return embi_path_move_to(p, x, y);
}

bool svgp_line(embi_path *p, float x, float y)
{
    return embi_path_line_to(p, x, y);
}

bool svgp_quad(embi_path *p, float cx, float cy, float x, float y)
{
    return embi_path_quad_to(p, cx, cy, x, y);
}

bool svgp_cubic(embi_path *p, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    return embi_path_cubic_to(p, c1x, c1y, c2x, c2y, x, y);
}

bool svgp_arc(embi_path *p, float rx, float ry, float rot, bool large, bool sweep, float x, float y)
{
    return embi_path_arc_to(p, rx, ry, rot, large, sweep, x, y);
}

bool svgp_close(embi_path *p)
{
    return embi_path_close(p);
}

bool svgp_empty(const embi_path *p)
{
    return embi_path_is_empty(p);
}

uint32_t svgp_segments(const embi_path *p)
{
    uint32_t n = 0;
    (void)embi_path_segments(p, &n);
    return n;
}

size_t svgp_bytes(const embi_path *p)
{
    uint32_t n = 0;
    (void)embi_path_segments(p, &n);
    return sizeof(void *) * 16 + (size_t)n * sizeof(embi_qcurve) * 2 + (size_t)embi_path_subpath_count(p) * 16;
}

embi_path *svgp_stroke(const embi_path *p, const svg_stroke *st, float tol)
{
    if (p == nullptr || st == nullptr || !(st->width > 0.0f) || embi_path_is_empty(p)) {
        return nullptr;
    }
    embi_stroke_style s;
    memset(&s, 0, sizeof s);
    s.width = st->width;
    s.cap = (embi_stroke_cap)st->cap;
    s.join = (embi_stroke_join)st->join;
    s.miter_limit = st->miter_limit;
    s.dash = st->dash;
    s.dash_count = st->ndash;
    s.dash_offset = st->dash_offset;
    s.scale = 1.0f;
    embi_path *out = embi_path_stroke(p, &s, tol);
    if (out != nullptr && embi_path_is_empty(out)) {
        embi_path_destroy(out);
        return nullptr;
    }
    return out;
}

embi_path *svgp_xform(const embi_path *p, const float m[6], float tol)
{
    embi_path *out = embi_path_create(tol);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    uint32_t ns = embi_path_subpath_count(p);
#define MX(x, y) (m[0] * (x) + m[2] * (y) + m[4])
#define MY(x, y) (m[1] * (x) + m[3] * (y) + m[5])
    for (uint32_t k = 0; k < ns; k++) {
        embi_path_subpath_info si;
        if (!embi_path_subpath(p, k, &si)) {
            continue;
        }
        (void)embi_path_move_to(out, MX(si.sx, si.sy), MY(si.sx, si.sy));
        if (si.count == 0) {
            if (si.closed) {
                (void)embi_path_close(out);
            } else {
                (void)embi_path_line_to(out, MX(si.sx, si.sy), MY(si.sx, si.sy));
            }
            continue;
        }
        for (uint32_t i = 0; i < si.count; i++) {
            const embi_qcurve *c = &q[si.first + i];
            (void)embi_path_quad_to(out, MX(c->cx, c->cy), MY(c->cx, c->cy), MX(c->x1, c->y1), MY(c->x1, c->y1));
        }
        if (si.closed) {
            (void)embi_path_close(out);
        }
    }
#undef MX
#undef MY
    return out;
}

bool svgp_bbox(const embi_path *p, float out[4])
{
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    /* point subpaths have a box of zero size at the point */
    uint32_t ns = embi_path_subpath_count(p);
    if (ns == 0) {
        return false;
    }
    for (uint32_t k = 0; k < ns; k++) {
        embi_path_subpath_info si;
        if (embi_path_subpath(p, k, &si)) {
            x0 = fminf(x0, si.sx);
            y0 = fminf(y0, si.sy);
            x1 = fmaxf(x1, si.sx);
            y1 = fmaxf(y1, si.sy);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *c = &q[i];
        x0 = fminf(x0, fminf(c->x0, c->x1));
        x1 = fmaxf(x1, fmaxf(c->x0, c->x1));
        y0 = fminf(y0, fminf(c->y0, c->y1));
        y1 = fmaxf(y1, fmaxf(c->y0, c->y1));
        /* the extremum of (1-t)^2 a + 2 (1-t) t b + t^2 c is at t = (a - b) / (a - 2b + c) */
        float dx = c->x0 - 2.0f * c->cx + c->x1, dy = c->y0 - 2.0f * c->cy + c->y1;
        if (dx != 0.0f) {
            float t = (c->x0 - c->cx) / dx;
            if (t > 0.0f && t < 1.0f) {
                float u = 1.0f - t;
                float px = u * u * c->x0 + 2 * u * t * c->cx + t * t * c->x1;
                x0 = fminf(x0, px);
                x1 = fmaxf(x1, px);
            }
        }
        if (dy != 0.0f) {
            float t = (c->y0 - c->cy) / dy;
            if (t > 0.0f && t < 1.0f) {
                float u = 1.0f - t;
                float py = u * u * c->y0 + 2 * u * t * c->cy + t * t * c->y1;
                y0 = fminf(y0, py);
                y1 = fmaxf(y1, py);
            }
        }
    }
    out[0] = x0;
    out[1] = y0;
    out[2] = x1 - x0;
    out[3] = y1 - y0;
    return true;
}

uint64_t svgp_hash(const embi_path *p)
{
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    uint64_t h = embi_hash_bytes(&n, sizeof n, 0x5367u);
    if (n > 0) {
        h = embi_hash_bytes(q, (size_t)n * sizeof *q, h);
    }
    uint32_t ns = embi_path_subpath_count(p);
    for (uint32_t k = 0; k < ns; k++) {
        embi_path_subpath_info si;
        if (embi_path_subpath(p, k, &si)) {
            uint32_t w[4] = {si.first, si.count, si.closed ? 1u : 0u, 0};
            h = embi_hash_bytes(w, sizeof w, h);
            if (si.count == 0) {
                float pt[2] = {si.sx, si.sy};
                h = embi_hash_bytes(pt, sizeof pt, h);
            }
        }
    }
    return h;
}

/* ------------------------------------------------------------------ flattening for queries */

#define SEG_STEPS 16

typedef struct fpt {
    double x, y;
} fpt;

/* Calls fn for every line segment of the flattened path, in order. */
typedef void (*seg_fn)(void *u, fpt a, fpt b, uint32_t subpath);

static void flatten(const embi_path *p, bool close_all, seg_fn fn, void *u)
{
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    uint32_t ns = embi_path_subpath_count(p);
    for (uint32_t k = 0; k < ns; k++) {
        embi_path_subpath_info si;
        if (!embi_path_subpath(p, k, &si)) {
            continue;
        }
        fpt start = {si.sx, si.sy}, cur = start;
        for (uint32_t i = 0; i < si.count; i++) {
            const embi_qcurve *c = &q[si.first + i];
            fpt prev = {c->x0, c->y0};
            for (int s = 1; s <= SEG_STEPS; s++) {
                double t = (double)s / SEG_STEPS, v = 1.0 - t;
                fpt pt = {v * v * c->x0 + 2 * v * t * c->cx + t * t * c->x1, v * v * c->y0 + 2 * v * t * c->cy + t * t * c->y1};
                fn(u, prev, pt, k);
                prev = pt;
            }
            cur = prev;
        }
        if ((close_all || si.closed) && (cur.x != start.x || cur.y != start.y)) {
            fn(u, cur, start, k);
        }
    }
}

typedef struct wind_st {
    double x, y;
    int wn;
    bool odd;
} wind_st;

static void wind_seg(void *u, fpt a, fpt b, uint32_t sp)
{
    (void)sp;
    wind_st *w = u;
    if ((a.y <= w->y) != (b.y <= w->y)) {
        double t = (w->y - a.y) / (b.y - a.y);
        double xi = a.x + t * (b.x - a.x);
        if (xi > w->x) {
            w->wn += b.y > a.y ? 1 : -1;
            w->odd = !w->odd;
        }
    }
}

bool svgp_contains(const embi_path *p, int rule, double x, double y)
{
    wind_st w = {x, y, 0, false};
    flatten(p, true, wind_seg, &w);
    return rule == 1 ? w.odd : w.wn != 0;
}

static void len_seg(void *u, fpt a, fpt b, uint32_t sp)
{
    (void)sp;
    *(double *)u += hypot(b.x - a.x, b.y - a.y);
}

double svgp_length(const embi_path *p)
{
    double total = 0.0;
    flatten(p, false, len_seg, &total);
    return total;
}

typedef struct at_st {
    double want, acc;
    bool done;
    double out[2];
    double angle;
    fpt last;
    double last_angle;
} at_st;

static void at_seg(void *u, fpt a, fpt b, uint32_t sp)
{
    (void)sp;
    at_st *s = u;
    double l = hypot(b.x - a.x, b.y - a.y);
    double ang = l > 0 ? atan2(b.y - a.y, b.x - a.x) : s->last_angle;
    s->last = b;
    s->last_angle = ang;
    if (s->done) {
        return;
    }
    if (s->acc + l >= s->want) {
        double t = l > 0 ? (s->want - s->acc) / l : 0.0;
        s->out[0] = a.x + t * (b.x - a.x);
        s->out[1] = a.y + t * (b.y - a.y);
        s->angle = ang;
        s->done = true;
    }
    s->acc += l;
}

bool svgp_point_at(const embi_path *p, double d, double out[2], double *angle)
{
    at_st s;
    memset(&s, 0, sizeof s);
    s.want = d < 0 ? 0 : d;
    flatten(p, false, at_seg, &s);
    if (!s.done) {
        if (s.acc == 0) {
            embi_path_subpath_info si;
            if (embi_path_subpath(p, 0, &si)) {
                out[0] = si.sx;
                out[1] = si.sy;
                if (angle != nullptr) {
                    *angle = 0;
                }
                return true;
            }
            return false;
        }
        out[0] = s.last.x;
        out[1] = s.last.y;
        if (angle != nullptr) {
            *angle = s.last_angle;
        }
        return true;
    }
    out[0] = s.out[0];
    out[1] = s.out[1];
    if (angle != nullptr) {
        *angle = s.angle;
    }
    return true;
}
