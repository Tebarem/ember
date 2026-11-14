#include "emb_test.h"

#include <math.h>
#include <stdint.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include "base/base.h"
#include "gpu/stroke.h"

/* CPU checks of stroke expansion: the coverage of the expanded path (its nonzero winding) against
 * a reference. The reference is a distance field to a dense polyline for round caps and joins
 * (exact), and the textbook rectangles + join polygons for lines with the other joins and caps.
 * Points within a small band of the boundary are skipped: the expansion is allowed an error of
 * the tolerance there. */

#define TOL (1.0f / 64.0f)
#define BAND 0.06f

static float test_nan(void)
{
    return __builtin_nanf("");
}

static float test_inf(void)
{
    return __builtin_inff();
}

/* Winding number ----------------------------------------------------------- */

static int winding_at(const embi_path *p, float x, float y)
{
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    int w = 0;
    enum { FLAT = 24 };
    for (uint32_t i = 0; i < n; i++) {
        float px = q[i].x0, py = q[i].y0;
        for (int k = 1; k <= FLAT; k++) {
            const float t = (float)k / (float)FLAT, mt = 1.0f - t;
            float qx = mt * mt * q[i].x0 + 2.0f * mt * t * q[i].cx + t * t * q[i].x1;
            float qy = mt * mt * q[i].y0 + 2.0f * mt * t * q[i].cy + t * t * q[i].y1;
            if (k == FLAT) {
                qx = q[i].x1;
                qy = q[i].y1;
            }
            if ((py <= y && qy > y) || (qy <= y && py > y)) {
                const float xi = px + (y - py) / (qy - py) * (qx - px);
                if (xi > x) {
                    w += qy > py ? 1 : -1;
                }
            }
            px = qx;
            py = qy;
        }
    }
    return w;
}

/* Reference: dense polyline of the curves of a path ------------------------ */

typedef struct dense {
    float *pts; /* x, y pairs; consecutive pairs are segments unless a break */
    uint8_t *brk; /* brk[i]: no segment from point i to i + 1 */
    uint32_t n;
} dense;

enum { DENSE_PER_QUAD = 400 };

static dense dense_of(const embi_path *p)
{
    uint32_t nq = 0;
    const embi_qcurve *q = embi_path_segments(p, &nq);
    dense d = {0};
    d.pts = SDL_malloc((size_t)nq * (DENSE_PER_QUAD + 1) * 2 * sizeof(float) + 8);
    d.brk = SDL_calloc((size_t)nq * (DENSE_PER_QUAD + 1) + 1, 1);
    for (uint32_t i = 0; i < nq; i++) {
        for (int k = 0; k <= DENSE_PER_QUAD; k++) {
            const float t = (float)k / (float)DENSE_PER_QUAD, mt = 1.0f - t;
            d.pts[2 * d.n] = mt * mt * q[i].x0 + 2.0f * mt * t * q[i].cx + t * t * q[i].x1;
            d.pts[2 * d.n + 1] = mt * mt * q[i].y0 + 2.0f * mt * t * q[i].cy + t * t * q[i].y1;
            d.brk[d.n] = (k == DENSE_PER_QUAD);
            d.n++;
        }
    }
    return d;
}

static void dense_free(dense *d)
{
    SDL_free(d->pts);
    SDL_free(d->brk);
}

static float dense_dist(const dense *d, float x, float y)
{
    float best = 1e30f;
    for (uint32_t i = 0; i < d->n; i++) {
        const float ax = d->pts[2 * i], ay = d->pts[2 * i + 1];
        float bx = ax, by = ay;
        if (!d->brk[i] && i + 1 < d->n) {
            bx = d->pts[2 * i + 2];
            by = d->pts[2 * i + 3];
        }
        const float ex = bx - ax, ey = by - ay;
        const float l2 = ex * ex + ey * ey;
        float t = l2 > 0.0f ? ((x - ax) * ex + (y - ay) * ey) / l2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float dx = ax + t * ex - x, dy = ay + t * ey - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
        }
    }
    return sqrtf(best);
}

/* Compares the coverage of s with the distance field over a window. Returns the mismatches. */
static int compare_round(const embi_path *src, const embi_path *s, float hw, float x0, float y0,
                         float x1, float y1, float step, float band)
{
    dense d = dense_of(src);
    if (SDL_getenv("EMB_STROKE_FULL") == nullptr) {
        step *= 1.7f; /* coarser sampling for quick local runs; EMB_STROKE_FULL=1 for the full grid */
    }
    int bad = 0;
    int signs = 0; /* bit 0: positive winding seen, bit 1: negative */
    for (float y = y0 + 0.0213f; y < y1; y += step) {
        for (float x = x0 + 0.0137f; x < x1; x += step * 1.07f) {
            const float dist = dense_dist(&d, x, y);
            const int w = winding_at(s, x, y);
            signs |= (w > 0 ? 1 : 0) | (w < 0 ? 2 : 0);
            if (fabsf(dist - hw) < band) {
                continue;
            }
            const bool want = dist < hw;
            if ((w != 0) != want) {
                if (bad < 3) {
                    SDL_Log("mismatch at (%.2f, %.2f): dist %.3f hw %.3f winding %d", x, y, dist,
                            hw, w);
                }
                bad++;
            }
        }
    }
    dense_free(&d);
    if (signs == 3) {
        SDL_Log("winding of both signs");
        bad++;
    }
    return bad;
}

/* Reference: a polyline (lines only) with caps and joins as polygons ------- */

static bool in_tri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy)
{
    const float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
    const float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
    const float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static bool in_seg_rect(float x, float y, float ax, float ay, float bx, float by, float hw,
                        float ext0, float ext1)
{
    float dx = bx - ax, dy = by - ay;
    const float l = sqrtf(dx * dx + dy * dy);
    dx /= l;
    dy /= l;
    const float u = (x - ax) * dx + (y - ay) * dy;
    const float v = -(x - ax) * dy + (y - ay) * dx;
    return u >= -ext0 && u <= l + ext1 && fabsf(v) <= hw;
}

static bool ref_inside1(const float *pt, int n, bool closed, float hw, embi_stroke_cap cap,
                        embi_stroke_join join, float ml, float x, float y)
{
    const int nseg = closed ? n : n - 1;
    for (int i = 0; i < nseg; i++) {
        const float *a = &pt[2 * i];
        const float *b = &pt[2 * ((i + 1) % n)];
        const float e0 = (!closed && i == 0 && cap == EMBI_CAP_SQUARE) ? hw : 0.0f;
        const float e1 = (!closed && i == nseg - 1 && cap == EMBI_CAP_SQUARE) ? hw : 0.0f;
        if (in_seg_rect(x, y, a[0], a[1], b[0], b[1], hw, e0, e1)) {
            return true;
        }
    }
    if (!closed && cap == EMBI_CAP_ROUND) {
        for (int e = 0; e < 2; e++) {
            const float *p = e == 0 ? &pt[0] : &pt[2 * (n - 1)];
            if (hypotf(x - p[0], y - p[1]) <= hw) {
                return true;
            }
        }
    }
    const int j0 = closed ? 0 : 1;
    const int j1 = closed ? n : n - 1;
    for (int j = j0; j < j1; j++) {
        const float *a = &pt[2 * ((j + n - 1) % n)];
        const float *p = &pt[2 * j];
        const float *b = &pt[2 * ((j + 1) % n)];
        float d0x = p[0] - a[0], d0y = p[1] - a[1], d1x = b[0] - p[0], d1y = b[1] - p[1];
        float l = hypotf(d0x, d0y);
        d0x /= l;
        d0y /= l;
        l = hypotf(d1x, d1y);
        d1x /= l;
        d1y /= l;
        const float cross = d0x * d1y - d0y * d1x;
        const float dot = d0x * d1x + d0y * d1y;
        if (join == EMBI_JOIN_ROUND) {
            if (hypotf(x - p[0], y - p[1]) <= hw) {
                return true;
            }
            continue;
        }
        const float s = cross > 0 ? -1.0f : 1.0f;
        const float ax_ = p[0] + s * hw * -d0y, ay_ = p[1] + s * hw * d0x;
        const float bx_ = p[0] + s * hw * -d1y, by_ = p[1] + s * hw * d1x;
        if (in_tri(x, y, p[0], p[1], ax_, ay_, bx_, by_)) {
            return true;
        }
        if (join == EMBI_JOIN_MITER && 1.0f + dot > 1e-6f &&
            1.0f / sqrtf((1.0f + dot) * 0.5f) <= ml) {
            const float k = s * hw / (1.0f + dot);
            const float mx = p[0] + k * (-d0y - d1y), my = p[1] + k * (d0x + d1x);
            if (in_tri(x, y, p[0], p[1], ax_, ay_, mx, my) ||
                in_tri(x, y, p[0], p[1], mx, my, bx_, by_)) {
                return true;
            }
        }
    }
    return false;
}

/* Agrees with the reference except in a thin band around its boundary. */
static int compare_poly(const embi_path *s, const float *pt, int n, bool closed, float hw,
                        embi_stroke_cap cap, embi_stroke_join join, float ml, float x0, float y0,
                        float x1, float y1, float step)
{
    int bad = 0;
    const float e = 0.05f;
    for (float y = y0 + 0.0213f; y < y1; y += step) {
        for (float x = x0 + 0.0137f; x < x1; x += step * 1.07f) {
            const bool c = ref_inside1(pt, n, closed, hw, cap, join, ml, x, y);
            if (ref_inside1(pt, n, closed, hw, cap, join, ml, x + e, y + e) != c ||
                ref_inside1(pt, n, closed, hw, cap, join, ml, x - e, y + e) != c ||
                ref_inside1(pt, n, closed, hw, cap, join, ml, x + e, y - e) != c ||
                ref_inside1(pt, n, closed, hw, cap, join, ml, x - e, y - e) != c) {
                continue;
            }
            const int w = winding_at(s, x, y);
            if ((w != 0) != c || w > 0) {
                if (bad < 3) {
                    SDL_Log("poly mismatch at (%.2f, %.2f): want %d winding %d", x, y, c, w);
                }
                bad++;
            }
        }
    }
    return bad;
}

static embi_path *poly_path(const float *pt, int n, bool closed)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, pt[0], pt[1]);
    for (int i = 1; i < n; i++) {
        embi_path_line_to(p, pt[2 * i], pt[2 * i + 1]);
    }
    if (closed) {
        embi_path_close(p);
    }
    return p;
}

/* Lines ---------------------------------------------------------------------- */

EMB_TEST(stroke_line_caps)
{
    const float pt[4] = {10, 20, 50, 28};
    embi_path *p = poly_path(pt, 2, false);
    for (int cap = 0; cap < 3; cap++) {
        embi_stroke_style st = {.width = 8, .cap = (embi_stroke_cap)cap};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        EMB_CHECK_EQ_INT(compare_poly(s, pt, 2, false, 4.0f, (embi_stroke_cap)cap,
                                      EMBI_JOIN_MITER, 4, 0, 5, 64, 40, 0.45f),
                         0);
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_polyline_joins)
{
    const float pt[8] = {8, 10, 40, 14, 20, 40, 52, 44};
    embi_path *p = poly_path(pt, 4, false);
    for (int join = 0; join < 3; join++) {
        for (int cap = 0; cap < 3; cap++) {
            embi_stroke_style st = {
                .width = 7, .cap = (embi_stroke_cap)cap, .join = (embi_stroke_join)join,
                .miter_limit = 3};
            embi_path *s = embi_path_stroke(p, &st, TOL);
            EMB_REQUIRE(s != nullptr);
            EMB_CHECK_EQ_INT(compare_poly(s, pt, 4, false, 3.5f, (embi_stroke_cap)cap,
                                          (embi_stroke_join)join, 3, 0, 0, 64, 56, 0.5f),
                             0);
            embi_path_destroy(s);
        }
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_miter_limit_switches_to_bevel)
{
    /* A 30 degree wedge: ratio 1 / sin(15 deg) = 3.86. Limit 4 miters, limit 3 bevels. */
    const float pt[6] = {10, 10, 50, 10, 10, 10 + 40 * tanf(0.5235988f)};
    embi_path *p = poly_path(pt, 3, false);
    const float limits[2] = {4.0f, 3.0f};
    for (int i = 0; i < 2; i++) {
        embi_stroke_style st = {.width = 4, .join = EMBI_JOIN_MITER, .miter_limit = limits[i]};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_CHECK_EQ_INT(compare_poly(s, pt, 3, false, 2.0f, EMBI_CAP_BUTT, EMBI_JOIN_MITER,
                                      limits[i], 0, 0, 64, 50, 0.4f),
                         0);
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_closed_triangle_and_square)
{
    const float tri[6] = {10, 10, 50, 14, 24, 46};
    const float sq[8] = {10, 10, 50, 10, 50, 50, 10, 50};
    for (int join = 0; join < 3; join++) {
        embi_path *p = poly_path(tri, 3, true);
        embi_stroke_style st = {.width = 6, .join = (embi_stroke_join)join, .miter_limit = 10};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_CHECK_EQ_INT(compare_poly(s, tri, 3, true, 3.0f, EMBI_CAP_BUTT, (embi_stroke_join)join,
                                      10, 0, 0, 64, 56, 0.5f),
                         0);
        embi_path_destroy(s);
        embi_path_destroy(p);
        p = poly_path(sq, 4, true);
        s = embi_path_stroke(p, &st, TOL);
        EMB_CHECK_EQ_INT(compare_poly(s, sq, 4, true, 3.0f, EMBI_CAP_BUTT, (embi_stroke_join)join,
                                      10, 0, 0, 62, 62, 0.5f),
                         0);
        embi_path_destroy(s);
        embi_path_destroy(p);
    }
}

EMB_TEST(stroke_180_degree_turn)
{
    const float pt[6] = {10, 20, 40, 20, 10, 20};
    embi_path *p = poly_path(pt, 3, false);
    for (int join = 0; join < 3; join++) {
        embi_stroke_style st = {.width = 8, .join = (embi_stroke_join)join, .miter_limit = 4};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        /* Miter and bevel end flat at x = 40. Round ends in a half disc. */
        EMB_CHECK(winding_at(s, 25, 22) != 0);
        EMB_CHECK(winding_at(s, 25, 26) == 0);
        EMB_CHECK((winding_at(s, 42, 20) != 0) == (join == EMBI_JOIN_ROUND));
        EMB_CHECK(winding_at(s, 44.5f, 20) == 0);
        EMB_CHECK(winding_at(s, 39.5f, 20) != 0);
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_very_short_segments)
{
    /* A 1e-3 and a 1e-5 px segment between two long ones must not make spikes: the bounding box
     * of the stroke stays that of the long lines (miter limit 4 would allow far more). */
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 10, 10);
    embi_path_line_to(p, 40, 10);
    embi_path_line_to(p, 40.001f, 10.0005f);
    embi_path_line_to(p, 40.00101f, 10.00051f);
    embi_path_line_to(p, 40, 40);
    embi_stroke_style st = {.width = 4, .join = EMBI_JOIN_MITER, .miter_limit = 4};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(s, &n);
    float maxx = -1e9f, miny = 1e9f;
    for (uint32_t i = 0; i < n; i++) {
        maxx = fmaxf(maxx, fmaxf(q[i].x0, q[i].cx));
        miny = fminf(miny, fminf(q[i].y0, q[i].cy));
    }
    EMB_CHECK(maxx < 42.3f);
    EMB_CHECK(miny > 7.9f);
    EMB_CHECK(winding_at(s, 25, 10) != 0);
    EMB_CHECK(winding_at(s, 40, 25) != 0);
    EMB_CHECK(winding_at(s, 41.9f, 11) != 0); /* the corner is mitered */
    embi_path_destroy(s);
    embi_path_destroy(p);

    /* Segments below the tolerance in a row, round caps: still a clean capsule. */
    p = embi_path_create(TOL);
    embi_path_move_to(p, 10, 10);
    for (int i = 1; i <= 50; i++) {
        embi_path_line_to(p, 10.0f + (float)i * 1e-4f, 10.0f);
    }
    embi_path_line_to(p, 30, 10);
    st.cap = EMBI_CAP_ROUND;
    s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 20, 11.5f) != 0);
    EMB_CHECK(winding_at(s, 20, 12.5f) == 0);
    EMB_CHECK(winding_at(s, 8.5f, 10) != 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

/* Curves ----------------------------------------------------------------------- */

static embi_path *curve_quad(void)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 8, 40);
    embi_path_quad_to(p, 32, -8, 56, 40);
    return p;
}

static embi_path *curve_s(void)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 6, 10);
    embi_path_cubic_to(p, 70, 0, -10, 50, 58, 50);
    return p;
}

static embi_path *curve_circle(void)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 12, 32);
    embi_path_arc_to(p, 20, 20, 0, false, true, 52, 32);
    embi_path_arc_to(p, 20, 20, 0, false, true, 12, 32);
    embi_path_close(p);
    return p;
}

EMB_TEST(stroke_curves_against_distance_field)
{
    embi_path *(*mk[3])(void) = {curve_quad, curve_s, curve_circle};
    const float widths[4] = {1.0f, 3.0f, 8.0f, 20.0f};
    for (int c = 0; c < 3; c++) {
        embi_path *p = mk[c]();
        for (int w = 0; w < 4; w++) {
            embi_stroke_style st = {.width = widths[w], .cap = EMBI_CAP_ROUND,
                                    .join = EMBI_JOIN_ROUND};
            embi_path *s = embi_path_stroke(p, &st, TOL);
            EMB_REQUIRE(s != nullptr);
            const int bad = compare_round(p, s, widths[w] * 0.5f, -12, -16, 76, 70, 0.9f, BAND);
            if (bad != 0) {
                SDL_Log("curve %d width %g: %d bad", c, widths[w], bad);
            }
            EMB_CHECK_EQ_INT(bad, 0);
            embi_path_destroy(s);
        }
        embi_path_destroy(p);
    }
}

EMB_TEST(stroke_curves_with_miter_and_butt_stay_inside_round)
{
    /* Butt caps and miter joins on a smooth curve: same as round (no corners, no caps to see) away
     * from the ends. Compare the middle of the quad away from its end caps. */
    embi_path *p = curve_quad();
    embi_stroke_style st = {.width = 4, .cap = EMBI_CAP_BUTT, .join = EMBI_JOIN_MITER,
                            .miter_limit = 4};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 32, 17.9f) != 0); /* the apex is at y = 16 */
    EMB_CHECK(winding_at(s, 32, 18.2f) == 0);
    EMB_CHECK(winding_at(s, 32, 14.1f) != 0);
    EMB_CHECK(winding_at(s, 32, 13.8f) == 0);
    EMB_CHECK(winding_at(s, 8.22f, 39.55f) != 0);
    EMB_CHECK(winding_at(s, 7.78f, 40.45f) == 0); /* butt: nothing past the end */
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_cusp_and_tight_curves)
{
    /* A quadratic that doubles back (a cusp), a cubic with a loop, a curve whose radius is far
     * below the half width, and a cubic with a near cusp. All compared with the distance field. */
    for (int c = 0; c < 4; c++) {
        embi_path *p = embi_path_create(TOL);
        embi_path_move_to(p, 14, 30);
        switch (c) {
        case 0:
            embi_path_quad_to(p, 54, 30, 34, 30);
            break;
        case 1:
            embi_path_cubic_to(p, 70, 0, -6, 0, 50, 40);
            break;
        case 2:
            embi_path_quad_to(p, 22, 30, 22, 34);
            embi_path_quad_to(p, 22, 38, 20, 36);
            break;
        default:
            embi_path_cubic_to(p, 50, 30.0f, 14, 30.4f, 50, 30.8f);
            break;
        }
        const float widths[2] = {2.0f, 12.0f};
        for (int w = 0; w < 2; w++) {
            embi_stroke_style st = {.width = widths[w], .cap = EMBI_CAP_ROUND,
                                    .join = EMBI_JOIN_ROUND};
            embi_path *s = embi_path_stroke(p, &st, TOL);
            EMB_REQUIRE(s != nullptr);
            const int bad = compare_round(p, s, widths[w] * 0.5f, -4, -6, 66, 60, 0.8f, BAND);
            if (bad != 0) {
                SDL_Log("hard case %d width %g: %d bad", c, widths[w], bad);
            }
            EMB_CHECK_EQ_INT(bad, 0);
            embi_path_destroy(s);
        }
        embi_path_destroy(p);
    }
}

/* A cubic whose control points lie on its chord, past the ends (a node editor's wire between two sockets at one
 * height): the curve goes forward, back and forward along one line. The stroke is the union of the swept width, so
 * every point of the line the curve covers is inside, wherever the doubling back falls. */
EMB_TEST(stroke_collinear_cubic_is_solid)
{
    const float lens[] = {3, 8, 12, 16, 19, 24, 27, 33, 40, 46, 55, 66, 80, 100, 160, 300};
    for (uint32_t li = 0; li < sizeof lens / sizeof *lens; li++) {
        for (int ov = 0; ov < 3; ov++) {
            for (int join = 0; join < 3; join++) {
                const float len = lens[li];
                const float dx = ov == 0 ? fmaxf(40.0f, len * 0.5f) : ov == 1 ? len * 0.25f : len * 0.5f;
                embi_path *p = embi_path_create(TOL);
                embi_path_move_to(p, 100.0f, 31.5f);
                embi_path_cubic_to(p, 100.0f + dx, 31.5f, 100.0f + len - dx, 31.5f, 100.0f + len, 31.5f);
                embi_stroke_style st = {.width = 2, .join = (embi_stroke_join)join, .miter_limit = 10};
                embi_path *s = embi_path_stroke(p, &st, 0.25f);
                /* the extent of the curve along x */
                float lo = 1e9f, hi = -1e9f;
                for (int k = 0; k <= 2000; k++) {
                    const float t = (float)k / 2000.0f, mt = 1.0f - t;
                    const float x = 3 * mt * mt * t * dx + 3 * mt * t * t * (len - dx) + t * t * t * len;
                    lo = fminf(lo, x);
                    hi = fmaxf(hi, x);
                }
                int holes = 0, stacked = 0;
                for (float x = 100.0f + lo + 0.05f; x < 100.0f + hi - 0.05f; x += 0.1f) {
                    for (float y = 30.7f; y <= 32.3f; y += 0.2f) {
                        const int w = winding_at(s, x, y);
                        if (w == 0) {
                            holes++;
                        }
                        if (w < -1) {
                            stacked++; /* pieces drawn over each other: their antialiased edges would add up */
                        }
                    }
                }
                if (join != 1) {
                    EMB_CHECK_EQ_INT(stacked, 0); /* one bar (round joins add discs at the turns) */
                }
                if (holes != 0) {
                    SDL_Log("collinear cubic len %g dx %g join %d: %d holes", (double)len, (double)dx, join, holes);
                }
                EMB_CHECK_EQ_INT(holes, 0);
                embi_path_destroy(s);
                embi_path_destroy(p);
            }
        }
    }
}

EMB_TEST(stroke_huge_widths)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 100, 100);
    embi_path_quad_to(p, 160, 40, 200, 110);
    const float widths[3] = {300.0f, 4000.0f, 1e5f};
    for (int w = 0; w < 3; w++) {
        embi_stroke_style st = {.width = widths[w], .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        dense d = dense_of(p);
        int bad = 0;
        const float hw = widths[w] * 0.5f;
        const float span = hw < 1000.0f ? hw * 1.6f : 1500.0f;
        for (int i = 0; i < 400; i++) {
            const float x = 150.0f + span * (2.0f * (float)((i * 37) % 101) / 100.0f - 1.0f);
            const float y = 80.0f + span * (2.0f * (float)((i * 53) % 103) / 102.0f - 1.0f);
            const float dist = dense_dist(&d, x, y);
            if (fabsf(dist - hw) < 0.05f * hw + 1.0f) {
                continue;
            }
            if ((winding_at(s, x, y) != 0) != (dist < hw)) {
                bad++;
            }
        }
        dense_free(&d);
        EMB_CHECK_EQ_INT(bad, 0);
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_winding_has_one_sign)
{
    /* Overlapping pieces (a tight zigzag with round joins, a closed star) never leave a
     * negative-winding lobe next to positive ones: the nonzero rule would punch holes. */
    embi_path *p = embi_path_create(TOL);
    const float star[10 * 2] = {32, 4,  38, 24, 60, 24, 42, 36, 50, 58,
                                32, 44, 14, 58, 22, 36, 4,  24, 26, 24};
    embi_path_move_to(p, star[0], star[1]);
    for (int i = 1; i < 10; i++) {
        embi_path_line_to(p, star[2 * i], star[2 * i + 1]);
    }
    embi_path_close(p);
    for (int join = 0; join < 3; join++) {
        embi_stroke_style st = {.width = 10, .join = (embi_stroke_join)join, .miter_limit = 10};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        int signs = 0;
        for (float y = -10; y < 70; y += 0.7f) {
            for (float x = -10; x < 75; x += 0.73f) {
                const int w = winding_at(s, x, y);
                signs |= (w > 0 ? 1 : 0) | (w < 0 ? 2 : 0);
            }
        }
        EMB_CHECK(signs == 2); /* negative only, by construction */
        /* The star's center is inside the stroke of its own edges: no hole. */
        EMB_CHECK(winding_at(s, 51, 30) != 0);
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}


/* Random paths ---------------------------------------------------------------------- */

static uint32_t rng_state = 12345u;

static void rng_seed_from_env(void)
{
    const char *e = SDL_getenv("EMB_STROKE_SEED");
    if (e != nullptr) {
        rng_state = (uint32_t)SDL_strtoul(e, nullptr, 10);
    }
}

/* Quick local default (1/12 of the full count); EMB_STROKE_FULL=1 runs everything (box). */
static int rnd_iters(int full)
{
    return SDL_getenv("EMB_STROKE_FULL") != nullptr ? full : full / 12;
}

static float rnd(float lo, float hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng_state >> 8) / 16777216.0f;
}

EMB_TEST(stroke_random_paths_against_distance_field)
{
    rng_seed_from_env();
    int total_bad = 0;
    for (int it = 0; it < (EMB_TEST_ASAN ? 8 : rnd_iters(60)); it++) {
        embi_path *p = embi_path_create(TOL);
        embi_path_move_to(p, rnd(5, 59), rnd(5, 59));
        const int nseg = 1 + (int)rnd(0, 4);
        for (int i = 0; i < nseg; i++) {
            const int kind = (int)rnd(0, 4);
            if (kind == 0) {
                embi_path_line_to(p, rnd(5, 59), rnd(5, 59));
            } else if (kind == 1) {
                embi_path_quad_to(p, rnd(0, 64), rnd(0, 64), rnd(5, 59), rnd(5, 59));
            } else if (kind == 2) {
                embi_path_cubic_to(p, rnd(0, 64), rnd(0, 64), rnd(0, 64), rnd(0, 64), rnd(5, 59),
                                   rnd(5, 59));
            } else {
                embi_path_arc_to(p, rnd(2, 30), rnd(2, 30), rnd(0, 3), rnd(0, 1) > 0.5f,
                                 rnd(0, 1) > 0.5f, rnd(5, 59), rnd(5, 59));
            }
        }
        if (rnd(0, 1) > 0.6f) {
            embi_path_close(p);
        }
        const float w = rnd(0.5f, 30.0f);
        embi_stroke_style st = {.width = w, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        {
            uint32_t nout = 0;
            (void)embi_path_segments(s, &nout);
            if (nout > 2000u) {
                SDL_Log("random path %d: %u output quads (width %.2f)", it, nout, w);
            }
        }
        const int bad = compare_round(p, s, w * 0.5f, -20, -20, 84, 84, 2.1f, BAND);
        if (bad != 0) {
            SDL_Log("random path %d (width %.2f): %d bad", it, w, bad);
            uint32_t nq = 0;
            const embi_qcurve *qq = embi_path_segments(p, &nq);
            for (uint32_t i = 0; i < nq; i++) {
                SDL_Log("  q %.4f %.4f  %.4f %.4f  %.4f %.4f", qq[i].x0, qq[i].y0, qq[i].cx, qq[i].cy,
                        qq[i].x1, qq[i].y1);
            }
        }
        total_bad += bad;
        embi_path_destroy(s);
        embi_path_destroy(p);
    }
    EMB_CHECK_EQ_INT(total_bad, 0);
}

EMB_TEST(stroke_random_polylines_against_polygon_reference)
{
    rng_seed_from_env();
    int total_bad = 0;
    for (int it = 0; it < (EMB_TEST_ASAN ? 16 : rnd_iters(120)); it++) {
        float pt[12];
        const int n = 2 + (int)rnd(0, 4);
        for (int i = 0; i < 2 * n; i++) {
            pt[i] = rnd(6, 58);
        }
        /* Keep consecutive points apart so the reference's directions are well defined. */
        bool ok = true;
        for (int i = 1; i < n; i++) {
            ok = ok && hypotf(pt[2 * i] - pt[2 * i - 2], pt[2 * i + 1] - pt[2 * i - 1]) > 1.0f;
        }
        const bool closed = n >= 3 && rnd(0, 1) > 0.5f;
        ok = ok && (!closed || hypotf(pt[0] - pt[2 * n - 2], pt[1] - pt[2 * n - 1]) > 1.0f);
        if (!ok) {
            continue;
        }
        const float w = rnd(1.0f, 14.0f);
        const embi_stroke_cap cap = (embi_stroke_cap)(int)rnd(0, 3);
        const embi_stroke_join join = (embi_stroke_join)(int)rnd(0, 3);
        const float ml = rnd(1.0f, 8.0f);
        embi_path *p = poly_path(pt, n, closed);
        embi_stroke_style st = {.width = w, .cap = cap, .join = join, .miter_limit = ml};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        const int bad = compare_poly(s, pt, n, closed, w * 0.5f, cap, join, ml, -10, -10, 75, 75, 1.3f);
        if (bad != 0) {
            SDL_Log("random polyline %d: %d bad (w %.2f cap %d join %d ml %.2f closed %d n %d)", it,
                    bad, w, (int)cap, (int)join, ml, closed, n);
        }
        total_bad += bad;
        embi_path_destroy(s);
        embi_path_destroy(p);
    }
    EMB_CHECK_EQ_INT(total_bad, 0);
}

/* Zero length subpaths and guards ---------------------------------------------- */

EMB_TEST(stroke_zero_length_subpaths_with_dashes)
{
    /* A dot only when the pattern, after the offset, starts on a zero-length dash. {5,5}: offset
     * 0 is a positive dash, offset 5 is a gap: no dot. {0,5} (period 5): offset 0 and 5 land on
     * the zero dash (dot), offset 2 is in the gap (none). */
    const float pats[2][2] = {{5, 5}, {0, 5}};
    const float offs[3] = {0, 2, 5};
    const bool want[2][3] = {{false, false, false}, {true, false, true}};
    for (int kind = 0; kind < 2; kind++) {
        embi_path *p = embi_path_create(TOL);
        embi_path_move_to(p, 20, 20);
        if (kind == 0) {
            embi_path_line_to(p, 20, 20);
        } else {
            embi_path_close(p);
        }
        for (int pi = 0; pi < 2; pi++) {
            for (int oi = 0; oi < 3; oi++) {
                embi_stroke_style st = {.width = 10, .cap = EMBI_CAP_ROUND, .dash = pats[pi],
                                        .dash_count = 2, .dash_offset = offs[oi]};
                embi_path *s = embi_path_stroke(p, &st, TOL);
                EMB_REQUIRE(s != nullptr);
                EMB_CHECK_EQ_INT(winding_at(s, 20, 20) != 0, want[pi][oi]);
                embi_path_destroy(s);
            }
        }
        embi_path_destroy(p);
    }
}

EMB_TEST(stroke_zero_length_subpaths)
{
    for (int kind = 0; kind < 3; kind++) {
        embi_path *p = embi_path_create(TOL);
        if (kind == 0) {
            embi_path_move_to(p, 20, 20);
            embi_path_line_to(p, 20, 20);
        } else if (kind == 1) {
            embi_path_move_to(p, 20, 20);
            embi_path_close(p);
        } else {
            embi_path_move_to(p, 20, 20);
            embi_path_line_to(p, 20, 20);
            embi_path_close(p);
        }
        for (int cap = 0; cap < 3; cap++) {
            embi_stroke_style st = {.width = 10, .cap = (embi_stroke_cap)cap};
            embi_path *s = embi_path_stroke(p, &st, TOL);
            EMB_REQUIRE(s != nullptr);
            if (cap == EMBI_CAP_BUTT) {
                EMB_CHECK(embi_path_is_empty(s));
            } else if (cap == EMBI_CAP_ROUND) {
                EMB_CHECK(winding_at(s, 20, 20) != 0);
                EMB_CHECK(winding_at(s, 24.5f, 20) != 0);
                EMB_CHECK(winding_at(s, 24.0f, 24.0f) == 0); /* a disc, not a square */
                EMB_CHECK(winding_at(s, 25.5f, 20) == 0);
            } else {
                EMB_CHECK(winding_at(s, 24.5f, 24.5f) != 0);
                EMB_CHECK(winding_at(s, 25.5f, 20) == 0);
                EMB_CHECK(winding_at(s, 20, 25.5f) == 0);
            }
            embi_path_destroy(s);
        }
        embi_path_destroy(p);
    }
    /* A bare moveto draws nothing. */
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 5, 5);
    embi_stroke_style st = {.width = 10, .cap = EMBI_CAP_ROUND};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(embi_path_is_empty(s));
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_nan_and_degenerate_guards)
{
    embi_path *p = curve_quad();
    const float bad_widths[6] = {0.0f, -3.0f, test_nan(), test_inf(), -test_inf(), 0.0f};
    for (int i = 0; i < 6; i++) {
        embi_stroke_style st = {.width = bad_widths[i], .cap = EMBI_CAP_ROUND};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        EMB_CHECK(embi_path_is_empty(s));
        embi_path_destroy(s);
    }
    /* Bad miter limits, tolerances, dashes and offsets fall back to defaults. */
    const float dash_bad[2] = {5.0f, test_nan()};
    const float dash_neg[2] = {5.0f, -1.0f};
    const float dash_zero[2] = {0.0f, 0.0f};
    const float *dashes[3] = {dash_bad, dash_neg, dash_zero};
    for (int i = 0; i < 3; i++) {
        embi_stroke_style st = {.width = 4,
                                .miter_limit = test_nan(),
                                .dash = dashes[i],
                                .dash_count = 2,
                                .dash_offset = test_nan(),
                                .scale = test_nan(),
                                .min_device_width = test_nan()};
        const float tols[3] = {test_nan(), -1.0f, test_inf()};
        embi_path *s = embi_path_stroke(p, &st, tols[i]);
        EMB_REQUIRE(s != nullptr);
        EMB_CHECK(winding_at(s, 32, 16.0f) != 0); /* solid at the apex */
        EMB_CHECK(winding_at(s, 20, 20.0f) != 0 || winding_at(s, 20, 19.0f) != 0 ||
                  winding_at(s, 20, 21.0f) != 0);
        embi_path_destroy(s);
    }
    EMB_CHECK(embi_path_stroke(nullptr, &(embi_stroke_style){.width = 1}, TOL) == nullptr);
    EMB_CHECK(embi_path_stroke(p, nullptr, TOL) == nullptr);
    /* The path builder itself refuses non-finite points, so the stroker never sees them. */
    EMB_CHECK(!embi_path_line_to(p, test_nan(), 1));
    embi_path_destroy(p);
}

EMB_TEST(stroke_hairlines)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 10, 10.5f);
    embi_path_line_to(p, 50, 10.5f);
    embi_stroke_style st = {.width = 0.1f, .scale = 1.0f, .min_device_width = 1.0f};
    float cov = 0;
    embi_path *s = embi_path_stroke_ex(p, &st, TOL, &cov);
    EMB_CHECK(fabsf(cov - 0.1f) < 1e-5f);
    EMB_CHECK(winding_at(s, 30, 10.1f) != 0);
    EMB_CHECK(winding_at(s, 30, 10.9f) != 0);
    EMB_CHECK(winding_at(s, 30, 9.9f) == 0);
    EMB_CHECK(winding_at(s, 30, 11.1f) == 0);
    embi_path_destroy(s);
    /* A strong transform: scale 0.05 makes a 4 px stroke 0.2 device px: widened to 1 device px,
     * which is 20 local px. */
    st = (embi_stroke_style){.width = 4, .scale = 0.05f, .min_device_width = 1.0f};
    s = embi_path_stroke_ex(p, &st, TOL, &cov);
    EMB_CHECK(fabsf(cov - 0.2f) < 1e-5f);
    EMB_CHECK(winding_at(s, 30, 10.5f + 9.5f) != 0);
    EMB_CHECK(winding_at(s, 30, 10.5f + 10.5f) == 0);
    embi_path_destroy(s);
    /* Wide enough: untouched. */
    st = (embi_stroke_style){.width = 4, .scale = 1.0f, .min_device_width = 1.0f};
    s = embi_path_stroke_ex(p, &st, TOL, &cov);
    EMB_CHECK(cov == 1.0f);
    EMB_CHECK(winding_at(s, 30, 12.4f) != 0);
    EMB_CHECK(winding_at(s, 30, 12.6f) == 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

/* Dashes ------------------------------------------------------------------------ */

static bool dash_on(float s, const float *pat, int m, float off)
{
    float period = 0.0f;
    for (int i = 0; i < m; i++) {
        period += pat[i];
    }
    float v = fmodf(s + off, period);
    if (v < 0) {
        v += period;
    }
    for (int i = 0; i < m; i++) {
        if (v < pat[i]) {
            return (i & 1) == 0;
        }
        v -= pat[i];
    }
    return false;
}

EMB_TEST(stroke_dashes_on_a_line)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 0, 20);
    embi_path_line_to(p, 100, 20);
    const float pat[3] = {10, 5, 3};
    const float pat_even[4] = {10, 5, 3, 5};
    struct {
        const float *d;
        uint32_t n;
        float off;
        const float *eff;
        int effn;
    } cases[] = {
        {pat_even, 4, 0, pat_even, 4},   {pat_even, 4, 3, pat_even, 4},
        {pat_even, 4, -2.5f, pat_even, 4}, {pat_even, 4, 61, pat_even, 4},
        {pat, 3, 0, pat, 3},             /* odd: repeated, checked below */
    };
    const float pat_odd2[6] = {10, 5, 3, 10, 5, 3};
    for (int c = 0; c < 5; c++) {
        embi_stroke_style st = {.width = 4, .dash = cases[c].d, .dash_count = cases[c].n,
                                .dash_offset = cases[c].off};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        EMB_REQUIRE(s != nullptr);
        const float *eff = c == 4 ? pat_odd2 : cases[c].eff;
        const int effn = c == 4 ? 6 : cases[c].effn;
        int bad = 0;
        for (float x = 0.3f; x < 99.8f; x += 0.17f) {
            const bool a = dash_on(x - 0.06f, eff, effn, cases[c].off);
            const bool b = dash_on(x + 0.06f, eff, effn, cases[c].off);
            if (a != b) {
                continue;
            }
            if ((winding_at(s, x, 20) != 0) != a) {
                bad++;
            }
            if (winding_at(s, x, 23.0f) != 0) {
                bad++;
            }
        }
        EMB_CHECK_EQ_INT(bad, 0);
        embi_path_destroy(s);
    }
    /* Round caps extend every dash by the half width. */
    const float pat2[2] = {10, 10};
    embi_stroke_style st = {.width = 4, .cap = EMBI_CAP_ROUND, .dash = pat2, .dash_count = 2};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 11.5f, 20) != 0); /* gap 10..20, covered by the caps up to 12 */
    EMB_CHECK(winding_at(s, 15, 20) == 0);
    EMB_CHECK(winding_at(s, 18.5f, 20) != 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_zero_length_dashes_make_dots)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 10, 20);
    embi_path_line_to(p, 90, 20);
    const float pat[2] = {0, 20};
    for (int cap = 1; cap < 3; cap++) {
        embi_stroke_style st = {.width = 6, .cap = (embi_stroke_cap)cap, .dash = pat,
                                .dash_count = 2};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        for (int i = 0; i < 5; i++) {
            EMB_CHECK(winding_at(s, 10.0f + 20.0f * (float)i, 20) != 0);
            EMB_CHECK(winding_at(s, 10.0f + 20.0f * (float)i + 10.0f, 20) == 0);
            EMB_CHECK(winding_at(s, 10.0f + 20.0f * (float)i + 2.8f, 22.8f) != 0 ||
                      cap == EMBI_CAP_ROUND);
        }
        embi_path_destroy(s);
    }
    embi_path_destroy(p);
}

EMB_TEST(stroke_dashes_on_a_curve)
{
    embi_path *p = curve_quad();
    dense d = dense_of(p);
    float *cum = SDL_malloc(sizeof(float) * (d.n + 1u));
    cum[0] = 0;
    for (uint32_t i = 1; i < d.n; i++) {
        cum[i] = cum[i - 1] + hypotf(d.pts[2 * i] - d.pts[2 * i - 2], d.pts[2 * i + 1] - d.pts[2 * i - 1]);
    }
    const float pat[2] = {9, 6};
    embi_stroke_style st = {.width = 2, .dash = pat, .dash_count = 2, .dash_offset = 4};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    int bad = 0, tested = 0;
    for (uint32_t i = 0; i < d.n; i += 3) {
        const bool a = dash_on(cum[i] - 0.35f, pat, 2, 4);
        const bool b = dash_on(cum[i] + 0.35f, pat, 2, 4);
        if (a != b) {
            continue;
        }
        tested++;
        if ((winding_at(s, d.pts[2 * i], d.pts[2 * i + 1]) != 0) != a) {
            bad++;
        }
    }
    EMB_CHECK(tested > 100);
    EMB_CHECK_EQ_INT(bad, 0);
    SDL_free(cum);
    dense_free(&d);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_dashes_closed_subpath_merge_first_and_last)
{
    const float sq[8] = {20, 20, 60, 20, 60, 60, 20, 60};
    embi_path *p = poly_path(sq, 4, true);
    /* Period 80, phase 5 at the start of the path: the last dash runs into the first through the
     * corner at (20, 20), which is then a real mitered join. */
    const float pat[2] = {70, 10};
    embi_stroke_style st = {.width = 4, .join = EMBI_JOIN_MITER, .miter_limit = 4, .dash = pat,
                            .dash_count = 2, .dash_offset = 5};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 18.5f, 18.5f) != 0); /* the miter at the start corner */
    EMB_CHECK(winding_at(s, 20, 22) != 0);
    embi_path_destroy(s);
    /* Without the offset the dash starts at the corner: a butt end, no miter. */
    st.dash_offset = 0;
    s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 18.5f, 18.5f) == 0);
    embi_path_destroy(s);
    /* One dash longer than the path: the whole closed outline, joined all round. */
    const float one[1] = {1000};
    st.dash = one;
    st.dash_count = 1;
    s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 18.5f, 18.5f) != 0);
    EMB_CHECK(winding_at(s, 61.5f, 61.5f) != 0);
    EMB_CHECK(winding_at(s, 40, 40) == 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_too_many_dashes_fall_back_to_solid)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 0, 20);
    embi_path_line_to(p, 1000, 20);
    const float pat[2] = {1e-4f, 1e-4f};
    embi_stroke_style st = {.width = 4, .dash = pat, .dash_count = 2};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    uint32_t n = 0;
    (void)embi_path_segments(s, &n);
    EMB_CHECK(n < 100u);
    EMB_CHECK(winding_at(s, 500, 20) != 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_dense_tiny_segments_keep_the_whole_stroke)
{
    /* 3000 segments of 5e-4 px: 1.5 px long in total, each far below the tolerance. */
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 10, 10);
    for (int i = 1; i <= 3000; i++) {
        embi_path_line_to(p, 10.0f + (float)i * 5e-4f, 10.0f);
    }
    embi_stroke_style st = {.width = 4, .cap = EMBI_CAP_BUTT};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(winding_at(s, 10.2f, 10.5f) != 0);
    EMB_CHECK(winding_at(s, 10.75f, 10.5f) != 0);
    EMB_CHECK(winding_at(s, 11.3f, 10.5f) != 0);
    EMB_CHECK(winding_at(s, 9.5f, 10.5f) == 0);
    EMB_CHECK(winding_at(s, 11.8f, 10.5f) == 0);
    embi_path_destroy(s);

    st.cap = EMBI_CAP_ROUND;
    s = embi_path_stroke(p, &st, TOL);
    EMB_CHECK(winding_at(s, 10.75f, 10.5f) != 0);
    EMB_CHECK(winding_at(s, 11.4f, 10.5f) != 0);
    embi_path_destroy(s);

    const float pat[2] = {0.5f, 0.5f};
    st.cap = EMBI_CAP_BUTT;
    st.dash = pat;
    st.dash_count = 2;
    s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(winding_at(s, 10.25f, 10.5f) != 0);
    EMB_CHECK(winding_at(s, 10.75f, 10.5f) == 0);
    EMB_CHECK(winding_at(s, 11.25f, 10.5f) != 0);
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_output_budget_is_bounded)
{
    /* A tiny tolerance on a wide curved stroke asks for far more than the 2^21 budget. */
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 0, 0);
    embi_path_cubic_to(p, 3000, -2000, -2000, 3000, 1000, 1000);
    embi_stroke_style st = {.width = 400, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
    embi_path *s = embi_path_stroke(p, &st, 1e-7f);
    EMB_REQUIRE(s != nullptr);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(s, &n);
    EMB_CHECK(n > 1000000u);
    EMB_CHECK(n <= (1u << 21) + 4096u);
    for (uint32_t i = 0; i < n; i += 97) {
        EMB_CHECK(isfinite(q[i].x0) && isfinite(q[i].y0) && isfinite(q[i].cx) && isfinite(q[i].cy));
    }
    embi_path_destroy(s);
    embi_path_destroy(p);
}

EMB_TEST(stroke_huge_coordinates_do_not_corrupt_output)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, -1e30f, 0);
    embi_path_line_to(p, 1e30f, 5);
    embi_path_line_to(p, 10, 10);
    embi_stroke_style st = {.width = 3, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    EMB_REQUIRE(s != nullptr);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(s, &n);
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(isfinite(q[i].x0) && isfinite(q[i].y0) && isfinite(q[i].cx) && isfinite(q[i].cy) &&
                  isfinite(q[i].x1) && isfinite(q[i].y1));
    }
    embi_path_destroy(s);
    embi_path_destroy(p);
}

/* Memory and cost ----------------------------------------------------------------- */

EMB_TEST(stroke_returns_memory)
{
    const size_t base = embi_mem_live();
    for (int i = 0; i < 20; i++) {
        embi_path *p = curve_s();
        const float pat[2] = {5, 3};
        embi_stroke_style st = {.width = 6, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND,
                                .dash = pat, .dash_count = 2};
        embi_path *s = embi_path_stroke(p, &st, TOL);
        embi_path_destroy(s);
        st.dash = nullptr;
        s = embi_path_stroke(p, &st, TOL);
        embi_path_destroy(s);
        embi_path_destroy(p);
    }
    EMB_CHECK(embi_mem_live() == base);
}

/* A 24 px icon in the usual style: a rounded box, a check mark, and a curve; 2 px strokes with
 * round caps and joins. */
static embi_path *icon_path(void)
{
    embi_path *p = embi_path_create(TOL);
    embi_path_move_to(p, 5, 3);
    embi_path_line_to(p, 19, 3);
    embi_path_arc_to(p, 2, 2, 0, false, true, 21, 5);
    embi_path_line_to(p, 21, 19);
    embi_path_arc_to(p, 2, 2, 0, false, true, 19, 21);
    embi_path_line_to(p, 5, 21);
    embi_path_arc_to(p, 2, 2, 0, false, true, 3, 19);
    embi_path_line_to(p, 3, 5);
    embi_path_arc_to(p, 2, 2, 0, false, true, 5, 3);
    embi_path_close(p);
    embi_path_move_to(p, 7, 12);
    embi_path_line_to(p, 10.5f, 15.5f);
    embi_path_line_to(p, 17, 8.5f);
    embi_path_move_to(p, 7, 18);
    embi_path_cubic_to(p, 10, 14, 14, 21, 17, 17);
    return p;
}

EMB_TEST(stroke_icon_cost_and_size)
{
    embi_path *p = icon_path();
    uint32_t in_segs = 0;
    (void)embi_path_segments(p, &in_segs);
    embi_stroke_style st = {.width = 2, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
    embi_path *s = embi_path_stroke(p, &st, TOL);
    uint32_t out_segs = 0;
    (void)embi_path_segments(s, &out_segs);
    EMB_CHECK(out_segs > in_segs);
    /* Coverage sanity: the center of the check mark is stroked, the middle of the box is not. */
    EMB_CHECK(winding_at(s, 8.75f, 13.75f) != 0);
    EMB_CHECK(winding_at(s, 12, 6) == 0);
    embi_path_destroy(s);

    st.join = EMBI_JOIN_MITER;
    st.cap = EMBI_CAP_BUTT;
    s = embi_path_stroke(p, &st, TOL);
    uint32_t out_miter = 0;
    (void)embi_path_segments(s, &out_miter);
    embi_path_destroy(s);

    const int reps = EMB_TEST_ASAN ? 100 : 2000;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < reps; i++) {
        s = embi_path_stroke(p, &st, TOL);
        embi_path_destroy(s);
    }
    const uint64_t t1 = SDL_GetPerformanceCounter();
    const double us = (double)(t1 - t0) * 1e6 / (double)SDL_GetPerformanceFrequency() / reps;
    SDL_Log("stroke icon: %u quads in, %u out (round) / %u out (miter+butt), %.1f out per in; %.1f us per icon",
            in_segs, out_segs, out_miter, (double)out_segs / (double)in_segs, us);
    embi_path_destroy(p);
}


EMB_TEST_MAIN()
