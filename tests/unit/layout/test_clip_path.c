/*
 * R3b clip-path geometry (src/layout/clip_path.c) and its hit testing: SVG path data, rounded rects with elliptical
 * corners, the point test of an outline, and a point that a clip-path cuts away is not a hit.
 */
#include "layout_harness.h"

#include "layout/clip_path.h"

#include <math.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.05f;
}

static embi_element *hit_at(lh *h, float x, float y)
{
    return embi_layout_hit_test(h->eng, h->doc, x, y);
}

EMB_TEST(svg_path_of_lines_is_closed_into_one_outline)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    EMB_REQUIRE(embi_lay_svg_path(EMBI_STR("M 0 0 L 10 0 L 10 10 Z"), 100.0f, 200.0f, &cv));
    EMB_CHECK_EQ_INT(cv.len, 3);
    EMB_CHECK(near(cv.data[0].c[0], 100) && near(cv.data[0].c[1], 200));
    EMB_CHECK(near(cv.data[1].c[4], 110) && near(cv.data[1].c[5], 210));
    /* The last curve returns to the start. */
    EMB_CHECK(near(cv.data[2].c[4], 100) && near(cv.data[2].c[5], 200));
    embi_vec_free(&cv);
}

EMB_TEST(svg_path_accepts_relative_horizontal_and_vertical_commands)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    EMB_REQUIRE(embi_lay_svg_path(EMBI_STR("m10 10h20v20h-20z"), 0.0f, 0.0f, &cv));
    EMB_CHECK_EQ_INT(cv.len, 4);
    EMB_CHECK(near(cv.data[0].c[4], 30) && near(cv.data[0].c[5], 10));
    EMB_CHECK(near(cv.data[1].c[4], 30) && near(cv.data[1].c[5], 30));
    embi_vec_free(&cv);
}

EMB_TEST(svg_path_cubic_and_arc_stay_on_the_curve)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    /* A half circle of radius 50 from (0, 50) to (100, 50). */
    EMB_REQUIRE(embi_lay_svg_path(EMBI_STR("M0 50 A50 50 0 0 1 100 50"), 0.0f, 0.0f, &cv));
    EMB_REQUIRE(cv.len >= 3);
    for (uint32_t i = 0; i < cv.len; i++) {
        float x = cv.data[i].c[4] - 50.0f, y = cv.data[i].c[5] - 50.0f;
        EMB_CHECK(fabsf(sqrtf(x * x + y * y) - 50.0f) < 0.1f);
    }
    embi_vec_free(&cv);
}

EMB_TEST(svg_path_rejects_bad_data)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    EMB_CHECK(!embi_lay_svg_path(EMBI_STR("M 0 0 L 10"), 0.0f, 0.0f, &cv));
    cv.len = 0;
    EMB_CHECK(!embi_lay_svg_path(EMBI_STR("X 1 2"), 0.0f, 0.0f, &cv));
    embi_vec_free(&cv);
}

EMB_TEST(cp_contains_follows_the_winding_rule)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    /* Two nested squares in the same direction: nonzero fills the middle, evenodd does not. */
    EMB_REQUIRE(embi_lay_svg_path(EMBI_STR("M0 0H100V100H0ZM25 25H75V75H25Z"), 0.0f, 0.0f, &cv));
    embi_lay_cp cp;
    memset(&cp, 0, sizeof cp);
    cp.kind = EMBI_LCP_PATH;
    cp.first = 0;
    cp.n = cv.len;
    cp.bounds[2] = cp.bounds[3] = 100.0f;
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 50, 50));
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 10, 10));
    EMB_CHECK(!embi_lay_cp_contains(&cp, cv.data, 150, 50));
    cp.evenodd = true;
    EMB_CHECK(!embi_lay_cp_contains(&cp, cv.data, 50, 50));
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 10, 10));
    embi_vec_free(&cv);
}

EMB_TEST(rrect_with_elliptical_corners_becomes_a_path)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    embi_lay_cp cp;
    const float rect[4] = {0, 0, 100, 60};
    const float rx[4] = {20, 20, 20, 20}, ry[4] = {10, 10, 10, 10};
    embi_lay_cp_rrect(&cp, &cv, rect, rx, ry);
    EMB_CHECK_EQ_INT(cp.kind, EMBI_LCP_PATH);
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 50, 30));
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 50, 1));
    /* The corner of the box is outside the elliptical corner. */
    EMB_CHECK(!embi_lay_cp_contains(&cp, cv.data, 1, 1));
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 20, 3));
    embi_vec_free(&cv);
}

EMB_TEST(rrect_with_circular_corners_stays_a_rounded_rect)
{
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    embi_lay_cp cp;
    const float rect[4] = {10, 10, 100, 60};
    const float rx[4] = {8, 8, 8, 8}, ry[4] = {8, 8, 8, 8};
    embi_lay_cp_rrect(&cp, &cv, rect, rx, ry);
    EMB_CHECK_EQ_INT(cp.kind, EMBI_LCP_RRECT);
    EMB_CHECK_EQ_INT(cv.len, 0);
    EMB_CHECK(embi_lay_cp_contains(&cp, cv.data, 60, 40));
    EMB_CHECK(!embi_lay_cp_contains(&cp, cv.data, 10.5f, 10.5f));
    embi_vec_free(&cv);
}

EMB_TEST(a_point_cut_away_by_clip_path_is_not_a_hit)
{
    lh h = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;background:red;clip-path:circle(40px at 50px 50px)'>"
                   "</div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 50, 50) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 2, 2) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 97, 97) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(a_clipped_context_lets_the_hit_through_to_what_is_behind)
{
    lh h = lh_make("<body style='margin:0'><div id=b style='position:absolute;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=a style='position:absolute;left:0;top:0;width:100px;height:100px;clip-path:inset(0 50px 0 0)'>"
                   "</div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 20, 50) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 80, 50) == lh_id(&h, "b"));
    lh_free(&h);
}

EMB_TEST(polygon_hit_test_uses_the_outline)
{
    lh h = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;clip-path:polygon(0 0, 100% 0, 0 100%)'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 20, 20) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 80, 80) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(geometry_box_alone_clips_to_the_box)
{
    lh h = lh_make("<body style='margin:0'><div id=a style='width:60px;height:60px;padding:20px;background:red;clip-path:content-box'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 50, 50) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 5, 5) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(shape_function_lines_and_keywords)
{
    /* A triangle (0,0) (100,0) (100,50): from top left, hline to the right edge, vline to the center, close. */
    lh h = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;"
                   "clip-path:shape(from top left, hline to right, vline to center, close)'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 80, 10) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 90, 40) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 10, 40) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 10, 80) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 90, 48) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(shape_function_curve_and_arc)
{
    /* A half disk: an arc from the left to the right edge of the box, closed by a line. */
    lh h = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;"
                   "clip-path:shape(evenodd from 0px 50px, arc to 100px 50px of 50px cw, close)'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 50, 20) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 50, 80) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 5, 10) != lh_id(&h, "a"));
    lh_free(&h);
    /* A quadratic curve with relative control point: by 100px 0 with 50px 100px bulges down. */
    lh h2 = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;"
                    "clip-path:shape(from 0px 0px, curve by 100px 0px with 50px 100px, close)'></div></body>",
                    "");
    EMB_CHECK(hit_at(&h2, 50, 30) == lh_id(&h2, "a"));
    EMB_CHECK(hit_at(&h2, 50, 70) != lh_id(&h2, "a"));
    lh_free(&h2);
}

EMB_TEST(shape_function_rejects_bad_syntax)
{
    /* The declaration is dropped: no clip, the element is hit everywhere. */
    lh h = lh_make("<body style='margin:0'><div id=a style='width:100px;height:100px;clip-path:shape(from 0 0, jump to 5px 5px)'>"
                   "</div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 90, 90) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST_MAIN()
