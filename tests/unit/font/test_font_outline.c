#include "font_fixture.h"

#include "font/font_internal.h"

/* Outline builder checks with analytic inputs. Expected values are computed by hand from the
 * curve formulas in the comments, not from the implementation. */

/* Dense polyline of a cubic, used as the reference for the error check. */
#define REF_STEPS 4000

static double dist2(embi_pt a, double x, double y)
{
    const double dx = a.x - x;
    const double dy = a.y - y;
    return dx * dx + dy * dy;
}

/* Quadratic at t: (1-t)^2 p0 + 2(1-t) t p1 + t^2 p2. */
static embi_pt quad_at(embi_quad q, double t)
{
    const double m = 1.0 - t;
    embi_pt r;
    r.x = (float)(m * m * q.p0.x + 2.0 * m * t * q.p1.x + t * t * q.p2.x);
    r.y = (float)(m * m * q.p0.y + 2.0 * m * t * q.p1.y + t * t * q.p2.y);
    return r;
}

/* Builds a closed triangle (0,0) (100,0) (50,80) into o. */
static void build_triangle(embi_outline *o)
{
    embi_olb b;
    embi_olb_init(&b, o);
    embi_olb_move(&b, (embi_pt){0.0f, 0.0f});
    embi_olb_line(&b, (embi_pt){100.0f, 0.0f});
    embi_olb_line(&b, (embi_pt){50.0f, 80.0f});
    embi_olb_close(&b);
    embi_olb_finish(&b);
}

/* One quad (0,0) control (50,100) to (100,0). The curve peaks at t = 0.5 where
 * y = 2 * 0.5 * 0.5 * 100 = 50, so the tight bbox is (0, 0, 100, 50). The closing line adds a
 * second quad back to the start. */
EMB_TEST(quad_bbox_is_tight_at_extremum)
{
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    embi_olb b;
    embi_olb_init(&b, &o);
    EMB_CHECK(embi_olb_move(&b, (embi_pt){0.0f, 0.0f}));
    EMB_CHECK(embi_olb_quad(&b, (embi_pt){50.0f, 100.0f}, (embi_pt){100.0f, 0.0f}));
    EMB_CHECK(embi_olb_close(&b));
    EMB_CHECK(embi_olb_finish(&b));

    EMB_CHECK_EQ_INT(o.quads.len, 2);
    EMB_CHECK_EQ_INT(o.contours.len, 1);
    EMB_CHECK_NEAR(o.xmin, 0.0, 1e-4);
    EMB_CHECK_NEAR(o.ymin, 0.0, 1e-4);
    EMB_CHECK_NEAR(o.xmax, 100.0, 1e-4);
    EMB_CHECK_NEAR(o.ymax, 50.0, 1e-4);
    embi_outline_free(&o);
}

/* Cubic (0,0) (0,100) (100,100) (100,0). Its y(t) = 300 t (1 - t) peaks at 75 for t = 0.5, so
 * the tight bbox is (0, 0, 100, 75). The conversion to quads is allowed to differ from the
 * curve by its tolerance, so the bbox check allows 0.5 units. */
EMB_TEST(cubic_to_quads_bbox_matches_curve_extremum)
{
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    embi_olb b;
    embi_olb_init(&b, &o);
    EMB_CHECK(embi_olb_move(&b, (embi_pt){0.0f, 0.0f}));
    EMB_CHECK(embi_olb_cubic(&b, (embi_pt){0.0f, 100.0f}, (embi_pt){100.0f, 100.0f},
                             (embi_pt){100.0f, 0.0f}, 0.25f));
    EMB_CHECK(embi_olb_finish(&b));

    EMB_CHECK(o.quads.len >= 1);
    EMB_CHECK_NEAR(o.xmin, 0.0, 0.5);
    EMB_CHECK_NEAR(o.ymin, 0.0, 0.5);
    EMB_CHECK_NEAR(o.xmax, 100.0, 0.5);
    EMB_CHECK_NEAR(o.ymax, 75.0, 0.5);
    embi_outline_free(&o);
}

/* Error check: every quad's sampled points (t = 0, 0.25, 0.5, 0.75, 1) lie within 2 * tol of the
 * true cubic, measured against a dense polyline of the cubic. The 2x factor covers the
 * between-sample error that the builder's three-point test does not measure directly. */
EMB_TEST(cubic_to_quads_stays_within_tolerance)
{
    const embi_pt p0 = {0.0f, 0.0f};
    const embi_pt p1 = {0.0f, 100.0f};
    const embi_pt p2 = {100.0f, 100.0f};
    const embi_pt p3 = {100.0f, 0.0f};
    const float tol = 0.25f;

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    embi_olb b;
    embi_olb_init(&b, &o);
    EMB_CHECK(embi_olb_move(&b, p0));
    EMB_CHECK(embi_olb_cubic(&b, p1, p2, p3, tol));
    EMB_CHECK(embi_olb_finish(&b));
    EMB_REQUIRE(o.quads.len >= 1);

    static embi_pt ref[REF_STEPS + 1];
    for (int i = 0; i <= REF_STEPS; i++) {
        const double s = (double)i / REF_STEPS;
        const double m = 1.0 - s;
        ref[i].x = (float)(m * m * m * p0.x + 3.0 * m * m * s * p1.x + 3.0 * m * s * s * p2.x +
                           s * s * s * p3.x);
        ref[i].y = (float)(m * m * m * p0.y + 3.0 * m * m * s * p1.y + 3.0 * m * s * s * p2.y +
                           s * s * s * p3.y);
    }

    const double limit = 2.0 * tol;
    /* finish closed the contour with one line quad from the cubic end back to its start. That
     * line lies 75 units from the curve, so it is excluded: only the cubic's own quads are
     * checked. The last quad must be the line (its control point is the midpoint). */
    EMB_REQUIRE(o.quads.len >= 2);
    const embi_quad close_q = o.quads.data[o.quads.len - 1];
    EMB_CHECK_NEAR(close_q.p1.x, (close_q.p0.x + close_q.p2.x) * 0.5, 1e-4);
    EMB_CHECK_NEAR(close_q.p1.y, (close_q.p0.y + close_q.p2.y) * 0.5, 1e-4);

    int worst_fail = 0;
    for (uint32_t q = 0; q + 1 < o.quads.len; q++) {
        static const double ts[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
        for (int k = 0; k < 5; k++) {
            const embi_pt pt = quad_at(o.quads.data[q], ts[k]);
            double best = 1e30;
            for (int i = 0; i <= REF_STEPS; i++) {
                const double d = dist2(ref[i], pt.x, pt.y);
                if (d < best) {
                    best = d;
                }
            }
            if (best > limit * limit) {
                worst_fail++;
            }
        }
    }
    EMB_CHECK_EQ_INT(worst_fail, 0);
    embi_outline_free(&o);
}

/* Transform append: dst receives src's quads mapped by x' = 2x + 10, y' = 2y + 20. */
EMB_TEST(append_xf_maps_points_and_bbox)
{
    embi_outline src;
    SDL_memset(&src, 0, sizeof src);
    build_triangle(&src);
    EMB_REQUIRE(src.quads.len == 3);

    embi_outline dst;
    SDL_memset(&dst, 0, sizeof dst);
    const float xf[6] = {2.0f, 0.0f, 0.0f, 2.0f, 10.0f, 20.0f};
    EMB_CHECK(embi_outline_append_xf(&dst, &src, xf));

    /* append_xf maps points and contour indices only. The bbox is recomputed by finish. */
    embi_olb b;
    embi_olb_init(&b, &dst);
    EMB_CHECK(embi_olb_finish(&b));

    EMB_CHECK_EQ_INT(dst.quads.len, src.quads.len);
    EMB_CHECK_EQ_INT(dst.contours.len, src.contours.len);
    for (uint32_t i = 0; i < src.quads.len; i++) {
        EMB_CHECK_NEAR(dst.quads.data[i].p0.x, 2.0 * src.quads.data[i].p0.x + 10.0, 1e-4);
        EMB_CHECK_NEAR(dst.quads.data[i].p0.y, 2.0 * src.quads.data[i].p0.y + 20.0, 1e-4);
        EMB_CHECK_NEAR(dst.quads.data[i].p2.x, 2.0 * src.quads.data[i].p2.x + 10.0, 1e-4);
        EMB_CHECK_NEAR(dst.quads.data[i].p2.y, 2.0 * src.quads.data[i].p2.y + 20.0, 1e-4);
    }
    /* Triangle bbox (0,0)-(100,80) maps to (10,20)-(210,180). */
    EMB_CHECK_NEAR(dst.xmin, 10.0, 1e-3);
    EMB_CHECK_NEAR(dst.ymin, 20.0, 1e-3);
    EMB_CHECK_NEAR(dst.xmax, 210.0, 1e-3);
    EMB_CHECK_NEAR(dst.ymax, 180.0, 1e-3);

    embi_outline_free(&src);
    embi_outline_free(&dst);
}

/* A contour with no segments is dropped: no quads and no contour record are added. A later
 * real contour is kept. */
EMB_TEST(empty_contour_is_dropped)
{
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    embi_olb b;
    embi_olb_init(&b, &o);
    EMB_CHECK(embi_olb_move(&b, (embi_pt){5.0f, 5.0f}));
    EMB_CHECK(embi_olb_close(&b));
    EMB_CHECK_EQ_INT(o.quads.len, 0);
    EMB_CHECK_EQ_INT(o.contours.len, 0);

    EMB_CHECK(embi_olb_move(&b, (embi_pt){0.0f, 0.0f}));
    EMB_CHECK(embi_olb_line(&b, (embi_pt){100.0f, 0.0f}));
    EMB_CHECK(embi_olb_line(&b, (embi_pt){50.0f, 80.0f}));
    EMB_CHECK(embi_olb_close(&b));
    EMB_CHECK(embi_olb_finish(&b));
    EMB_CHECK_EQ_INT(o.contours.len, 1);
    EMB_CHECK_EQ_INT(o.quads.len, 3);
    embi_outline_free(&o);
}

/* Every contour is closed: its last quad ends at the contour's first point. Two triangles give
 * two contour records, the second starting at quad 3. */
EMB_TEST(contours_are_closed_and_indexed)
{
    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    build_triangle(&o);
    embi_olb b;
    embi_olb_init(&b, &o);
    EMB_CHECK(embi_olb_move(&b, (embi_pt){200.0f, 0.0f}));
    EMB_CHECK(embi_olb_line(&b, (embi_pt){300.0f, 0.0f}));
    EMB_CHECK(embi_olb_line(&b, (embi_pt){250.0f, 80.0f}));
    EMB_CHECK(embi_olb_close(&b));
    EMB_CHECK(embi_olb_finish(&b));

    EMB_CHECK_EQ_INT(o.contours.len, 2);
    EMB_CHECK_EQ_INT(o.contours.data[0], 0);
    EMB_CHECK_EQ_INT(o.contours.data[1], 3);
    EMB_CHECK_EQ_INT(o.quads.len, 6);
    EMB_CHECK_EQ_INT(fx_count_open_contours(&o), 0);
    embi_outline_free(&o);
}

/* Freeing a zeroed outline is a no-op, and freeing twice is safe. Storage returns to the
 * allocator. */
EMB_TEST(outline_free_is_idempotent)
{
    const size_t before = embi_mem_live();
    embi_outline z;
    SDL_memset(&z, 0, sizeof z);
    embi_outline_free(&z);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);

    embi_outline o;
    SDL_memset(&o, 0, sizeof o);
    build_triangle(&o);
    EMB_CHECK(embi_mem_live() > before);
    embi_outline_free(&o);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
    embi_outline_free(&o);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
}

EMB_TEST_MAIN()
