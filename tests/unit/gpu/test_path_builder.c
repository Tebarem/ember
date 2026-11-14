#include "emb_test.h"

#include <stdint.h>

#include <SDL3/SDL_stdinc.h>

#include "gpu/path.h"

/* CPU-side checks of the path builder: the segments it makes, not pixels. The flattening bound
 * is checked by sampling each quadratic and measuring its distance to a dense sampling of the
 * cubic it replaces. */

#define DENSE 4096

static float test_nan(void)
{
    return __builtin_nanf("");
}

static float test_inf(void)
{
    return __builtin_inff();
}

static void eval_quad(const embi_qcurve *q, float t, float *x, float *y)
{
    const float mt = 1.0f - t;
    *x = mt * mt * q->x0 + 2.0f * mt * t * q->cx + t * t * q->x1;
    *y = mt * mt * q->y0 + 2.0f * mt * t * q->cy + t * t * q->y1;
}

static void eval_cubic(const float P[8], float t, float *x, float *y)
{
    const float mt = 1.0f - t;
    const float a = mt * mt * mt;
    const float b = 3.0f * mt * mt * t;
    const float c = 3.0f * mt * t * t;
    const float d = t * t * t;
    *x = a * P[0] + b * P[2] + c * P[4] + d * P[6];
    *y = a * P[1] + b * P[3] + c * P[5] + d * P[7];
}

/* Dense polyline of a cubic, DENSE + 1 points. */
static void dense_cubic(const float P[8], float *pts)
{
    for (int i = 0; i <= DENSE; i++) {
        eval_cubic(P, (float)i / (float)DENSE, &pts[2 * i], &pts[2 * i + 1]);
    }
}

static float dist_to_polyline(const float *pts, float x, float y)
{
    float best = 1e30f;
    for (int i = 0; i <= DENSE; i++) {
        const float dx = pts[2 * i] - x;
        const float dy = pts[2 * i + 1] - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
        }
    }
    return SDL_sqrtf(best);
}

/* Largest distance from the quadratics' samples to the sampled cubic. */
static float max_flatten_error(const embi_qcurve *q, uint32_t n, const float *pts)
{
    float worst = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k <= 16; k++) {
            float x = 0.0f;
            float y = 0.0f;
            eval_quad(&q[i], (float)k / 16.0f, &x, &y);
            const float d = dist_to_polyline(pts, x, y);
            if (d > worst) {
                worst = d;
            }
        }
    }
    return worst;
}

EMB_TEST(path_starts_empty_and_rejects_non_finite_arguments)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_path_is_empty(p));
    EMB_CHECK(!embi_path_line_to(p, test_nan(), 0.0f));
    EMB_CHECK(!embi_path_move_to(p, 0.0f, test_inf()));
    EMB_CHECK(!embi_path_cubic_to(p, 0.0f, 0.0f, 1.0f, test_inf(), 2.0f, 2.0f));
    EMB_CHECK(!embi_path_arc_to(p, 5.0f, 5.0f, 0.0f, false, true, test_nan(), 0.0f));
    EMB_CHECK(embi_path_is_empty(p));
    EMB_CHECK(embi_path_subpath_count(p) == 0u);
    embi_path_destroy(p);
}

EMB_TEST(path_line_is_one_quad_with_its_midpoint_as_control)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 1.0f, 2.0f);
    embi_path_line_to(p, 5.0f, 6.0f);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_REQUIRE(n == 1u);
    EMB_CHECK(q[0].x0 == 1.0f && q[0].y0 == 2.0f);
    EMB_CHECK(q[0].cx == 3.0f && q[0].cy == 4.0f);
    EMB_CHECK(q[0].x1 == 5.0f && q[0].y1 == 6.0f);
    EMB_CHECK(embi_path_subpath_count(p) == 1u);
    embi_path_destroy(p);
}

EMB_TEST(path_segment_without_a_subpath_starts_one_at_its_point)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    /* A line to the first point is zero length: it starts the subpath and adds nothing. */
    embi_path_line_to(p, 5.0f, 5.0f);
    uint32_t n = 0;
    embi_path_segments(p, &n);
    EMB_CHECK(n == 0u);
    embi_path_line_to(p, 10.0f, 5.0f);
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_REQUIRE(n == 1u);
    EMB_CHECK(q[0].x0 == 5.0f && q[0].y0 == 5.0f && q[0].x1 == 10.0f && q[0].y1 == 5.0f);
    embi_path_destroy(p);
}

EMB_TEST(path_quad_segment_is_kept_exactly)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_quad_to(p, 3.0f, 9.0f, 6.0f, 0.0f);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_REQUIRE(n == 1u);
    EMB_CHECK(q[0].cx == 3.0f && q[0].cy == 9.0f && q[0].x1 == 6.0f && q[0].y1 == 0.0f);
    embi_path_destroy(p);
}

EMB_TEST(path_straight_cubic_is_a_single_quad)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_cubic_to(p, 10.0f, 0.0f, 20.0f, 0.0f, 30.0f, 0.0f);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_CHECK(n == 1u);
    if (n == 1u) {
        EMB_CHECK(q[0].cy == 0.0f && q[0].cx == 15.0f);
    }
    embi_path_destroy(p);
}

EMB_TEST(path_cubic_flattening_stays_within_its_tolerance)
{
    const float tol = 0.05f;
    embi_path *p = embi_path_create(tol);
    EMB_REQUIRE(p != nullptr);
    const float P[8] = {0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f};
    embi_path_move_to(p, P[0], P[1]);
    embi_path_cubic_to(p, P[2], P[3], P[4], P[5], P[6], P[7]);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_CHECK(n > 1u);
    EMB_REQUIRE(n > 0u);
    EMB_CHECK(q[0].x0 == P[0] && q[0].y0 == P[1]);
    EMB_CHECK(q[n - 1u].x1 == P[6] && q[n - 1u].y1 == P[7]);
    for (uint32_t i = 0; i + 1u < n; i++) {
        EMB_CHECK(q[i].x1 == q[i + 1u].x0 && q[i].y1 == q[i + 1u].y0);
    }
    float *pts = SDL_malloc(sizeof(float) * 2 * (DENSE + 1));
    EMB_REQUIRE(pts != nullptr);
    dense_cubic(P, pts);
    const float worst = max_flatten_error(q, n, pts);
    SDL_free(pts);
    EMB_CHECK(worst <= tol + 1e-3f);
    embi_path_destroy(p);
}

/* A deterministic spread of cubics: each one flattened at a tolerance, measured against the cubic.
 * The bound is (sqrt(3) / 36) |P0 - 3P1 + 3P2 - P3| per piece, so the measured error must stay
 * under the tolerance everywhere. */
EMB_TEST(path_cubic_flattening_holds_for_many_cubics)
{
    const float tol = 0.02f;
    float *pts = SDL_malloc(sizeof(float) * 2 * (DENSE + 1));
    EMB_REQUIRE(pts != nullptr);
    uint32_t seed = 12345u;
    float worst_all = 0.0f;
    for (int c = 0; c < 40; c++) {
        float P[8];
        for (int k = 0; k < 8; k++) {
            seed = seed * 1664525u + 1013904223u;
            P[k] = (float)(seed >> 8) * (1.0f / 16777216.0f) * 200.0f - 100.0f;
        }
        embi_path *p = embi_path_create(tol);
        EMB_REQUIRE(p != nullptr);
        embi_path_move_to(p, P[0], P[1]);
        embi_path_cubic_to(p, P[2], P[3], P[4], P[5], P[6], P[7]);
        uint32_t n = 0;
        const embi_qcurve *q = embi_path_segments(p, &n);
        EMB_REQUIRE(n > 0u);
        dense_cubic(P, pts);
        const float worst = max_flatten_error(q, n, pts);
        if (worst > worst_all) {
            worst_all = worst;
        }
        embi_path_destroy(p);
    }
    SDL_free(pts);
    EMB_CHECK(worst_all <= tol + 1e-3f);
}

/* The quarter-turn check on arcs: every sample lies on the circle of the arc within tolerance. */
static float max_radius_error(const embi_qcurve *q, uint32_t n, float cx, float cy, float r)
{
    float worst = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k <= 8; k++) {
            float x = 0.0f;
            float y = 0.0f;
            eval_quad(&q[i], (float)k / 8.0f, &x, &y);
            const float d = SDL_fabsf(SDL_sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r);
            if (d > worst) {
                worst = d;
            }
        }
    }
    return worst;
}

EMB_TEST(path_semicircle_arc_lies_on_its_circle)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    EMB_CHECK(embi_path_arc_to(p, 10.0f, 10.0f, 0.0f, false, true, 20.0f, 0.0f));
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_REQUIRE(n >= 4u);
    EMB_CHECK(q[n - 1u].x1 == 20.0f && q[n - 1u].y1 == 0.0f);
    EMB_CHECK(max_radius_error(q, n, 10.0f, 0.0f, 10.0f) <= 1.0f / 64.0f + 1e-3f);
    embi_path_destroy(p);
}

/* Chord 20 with radius 20: the centers are (10, +-17.32). The small arc bulges 20 - 17.32 = 2.68
 * from the chord, the large one 20 + 17.32 = 37.32. */
static float max_abs_y(const embi_qcurve *q, uint32_t n)
{
    float worst = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k <= 8; k++) {
            float x = 0.0f;
            float y = 0.0f;
            eval_quad(&q[i], (float)k / 8.0f, &x, &y);
            if (SDL_fabsf(y) > worst) {
                worst = SDL_fabsf(y);
            }
        }
    }
    return worst;
}

EMB_TEST(path_arc_flags_choose_the_small_or_large_circle)
{
    embi_path *small = embi_path_create(0.0f);
    embi_path *large = embi_path_create(0.0f);
    EMB_REQUIRE(small != nullptr && large != nullptr);
    embi_path_move_to(small, 0.0f, 0.0f);
    embi_path_arc_to(small, 20.0f, 20.0f, 0.0f, false, true, 20.0f, 0.0f);
    embi_path_move_to(large, 0.0f, 0.0f);
    embi_path_arc_to(large, 20.0f, 20.0f, 0.0f, true, true, 20.0f, 0.0f);
    uint32_t ns = 0;
    uint32_t nl = 0;
    const embi_qcurve *qs = embi_path_segments(small, &ns);
    const embi_qcurve *ql = embi_path_segments(large, &nl);
    EMB_CHECK(SDL_fabsf(max_abs_y(qs, ns) - 2.68f) < 0.1f);
    EMB_CHECK(SDL_fabsf(max_abs_y(ql, nl) - 37.32f) < 0.1f);
    embi_path_destroy(small);
    embi_path_destroy(large);
}

EMB_TEST(path_arc_with_small_radii_scales_them_up)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    /* Radius 1 cannot span 20 px, so SVG scales it to 10: a semicircle around (10, 0). */
    embi_path_arc_to(p, 1.0f, 1.0f, 0.0f, false, true, 20.0f, 0.0f);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_REQUIRE(n >= 1u);
    EMB_CHECK(max_radius_error(q, n, 10.0f, 0.0f, 10.0f) <= 1.0f / 64.0f + 1e-3f);
    embi_path_destroy(p);
}

EMB_TEST(path_arc_with_a_zero_radius_is_a_line)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_arc_to(p, 0.0f, 5.0f, 0.0f, false, false, 10.0f, 0.0f);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_CHECK(n == 1u);
    if (n == 1u) {
        EMB_CHECK(q[0].cx == 5.0f && q[0].cy == 0.0f);
    }
    embi_path_destroy(p);
}

EMB_TEST(path_close_adds_the_closing_line_and_the_next_segment_starts_at_the_start)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_line_to(p, 10.0f, 0.0f);
    embi_path_line_to(p, 10.0f, 10.0f);
    embi_path_close(p);
    uint32_t n = 0;
    const embi_qcurve *q = embi_path_segments(p, &n);
    EMB_CHECK(n == 3u);
    if (n == 3u) {
        EMB_CHECK(q[2].x1 == 0.0f && q[2].y1 == 0.0f);
    }
    EMB_CHECK(embi_path_subpath_count(p) == 1u);
    embi_path_line_to(p, 5.0f, 5.0f);
    q = embi_path_segments(p, &n);
    EMB_CHECK(n == 4u);
    if (n == 4u) {
        EMB_CHECK(q[3].x0 == 0.0f && q[3].y0 == 0.0f && q[3].x1 == 5.0f && q[3].y1 == 5.0f);
    }
    EMB_CHECK(embi_path_subpath_count(p) == 2u);
    embi_path_destroy(p);
}

EMB_TEST(path_clear_removes_everything)
{
    embi_path *p = embi_path_create(0.0f);
    EMB_REQUIRE(p != nullptr);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_line_to(p, 1.0f, 1.0f);
    embi_path_clear(p);
    EMB_CHECK(embi_path_is_empty(p));
    EMB_CHECK(embi_path_subpath_count(p) == 0u);
    /* After a clear there is no current point, so a line only starts a subpath. */
    embi_path_line_to(p, 3.0f, 3.0f);
    uint32_t n = 0;
    embi_path_segments(p, &n);
    EMB_CHECK(n == 0u);
    embi_path_destroy(p);
}

EMB_TEST_MAIN()
