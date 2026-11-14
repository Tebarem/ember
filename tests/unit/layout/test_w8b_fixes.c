/*
 * Regression tests for the layout fix of W8b: a block with width: auto and auto margins whose width min-width or max-width
 * changes is laid out again with that width (CSS 2.1 section 10.4), so its auto margins resolve (it is centred). The
 * TodoMVC apps of tests/apps centre their body this way (body { max-width: 560px; margin: 0 auto }). The viewport of the
 * harness is 800px wide.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static bool border_rect(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

EMB_TEST(max_width_with_auto_margins_centres)
{
    lh h = lh_make("<body><div id=d style='max-width: 200px; margin: 0 auto; padding: 0 10px'></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.w, 220));
    EMB_CHECK(near(r.x, 290));
    lh_free(&h);
}

EMB_TEST(max_width_with_one_auto_margin_pushes_right)
{
    lh h = lh_make("<body><div id=d style='max-width: 200px; margin-left: auto'></div></body>", "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.w, 200));
    EMB_CHECK(near(r.x, 600));
    lh_free(&h);
}

EMB_TEST(max_width_on_the_body_centres_its_content)
{
    /* The TodoMVC body: max-width 560px, padding 16px, margin 0 auto, in an 800px viewport. */
    lh h = lh_make("<body id=b><div id=app style='height: 10px'></div></body>",
                   "body { margin: 0 auto; padding: 0 16px; max-width: 560px }");
    embi_rectf b, a;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_REQUIRE(border_rect(&h, "app", &a));
    EMB_CHECK(near(b.w, 592));
    EMB_CHECK(near(b.x, 104));
    EMB_CHECK(near(a.x, 120));
    EMB_CHECK(near(a.w, 560));
    lh_free(&h);
}

EMB_TEST(min_width_wider_than_the_container_keeps_the_start_edge)
{
    /* Over-constrained: the box is wider than its container, so the auto margins are 0 and it overflows to the right
     * (ltr). */
    lh h = lh_make("<body><div id=d style='min-width: 900px; margin: 0 auto'></div></body>", "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.w, 900));
    EMB_CHECK(near(r.x, 0));
    lh_free(&h);
}

EMB_TEST(no_clamp_keeps_zero_margins)
{
    /* Not clamped: the fill width, auto margins 0 (the common case is unchanged). */
    lh h = lh_make("<body><div id=d style='max-width: 2000px; margin: 0 auto'></div></body>", "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.w, 800));
    EMB_CHECK(near(r.x, 0));
    lh_free(&h);
}

EMB_TEST_MAIN()
