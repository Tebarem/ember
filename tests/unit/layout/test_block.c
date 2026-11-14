/*
 * Block flow and sizing (W3a): width and margin rules, margin collapsing, min and max sizes. The
 * hand-derived block fixtures in tests/layout/block run in test_layout_wpt.c (fixtures.txt).
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static bool border_rect(lh *h, const char *id, embi_rectf *out)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, out);
}

static bool edges_of(lh *h, const char *id, float bdr[4], float pad[4], float mg[4])
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_box_edges(h->eng, el, bdr, pad, mg);
}

EMB_TEST(block_auto_width_fills_the_containing_block)
{
    lh h = lh_make("<body><div id=d></div></body>", "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.w, 800));
    EMB_CHECK(near(r.x, 0));
    lh_free(&h);
}

EMB_TEST(block_auto_margins_center_a_fixed_width_box)
{
    lh h = lh_make("<body><div id=d style='width: 200px; margin: 0 auto'></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_CHECK(near(r.x, 300));
    EMB_CHECK(near(r.w, 200));
    lh_free(&h);
}

EMB_TEST(block_sibling_margins_collapse_to_the_larger)
{
    lh h = lh_make("<body><div id=a style='height: 10px; margin: 20px 0'></div>"
                   "<div id=b style='height: 10px; margin: 30px 0'></div></body>",
                   "body { margin: 0 }");
    embi_rectf a, b;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(a.y, 20));
    /* 20px below a, collapsed with b's 30px top margin to 30px. */
    EMB_CHECK(near(b.y, 60));
    lh_free(&h);
}

EMB_TEST(block_child_margin_collapses_through_parent)
{
    lh h = lh_make("<body><div id=p><div id=c style='margin-top: 40px; height: 10px'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf p, c;
    EMB_REQUIRE(border_rect(&h, "p", &p));
    EMB_REQUIRE(border_rect(&h, "c", &c));
    EMB_CHECK(near(p.y, 40));
    EMB_CHECK(near(c.y, 40));
    lh_free(&h);
}

EMB_TEST(block_min_and_max_width_clamp_the_used_width)
{
    lh h = lh_make("<body><div id=m style='width: 500px; max-width: 200px'></div>"
                   "<div id=n style='width: 10px; min-width: 50px'></div></body>",
                   "body { margin: 0 }");
    embi_rectf m, n;
    EMB_REQUIRE(border_rect(&h, "m", &m));
    EMB_REQUIRE(border_rect(&h, "n", &n));
    EMB_CHECK(near(m.w, 200));
    EMB_CHECK(near(n.w, 50));
    lh_free(&h);
}

EMB_TEST(block_padding_adds_to_the_border_box)
{
    lh h = lh_make("<body><div id=p style='width: 100px; padding: 5px; border: 2px solid'></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    float bdr[4], pad[4], mg[4];
    EMB_REQUIRE(border_rect(&h, "p", &r));
    EMB_REQUIRE(edges_of(&h, "p", bdr, pad, mg));
    EMB_CHECK(near(r.w, 114));
    EMB_CHECK(near(bdr[0], 2));
    EMB_CHECK(near(pad[0], 5));
    lh_free(&h);
}

EMB_TEST(block_percent_height_of_a_fixed_parent)
{
    lh h = lh_make("<body><div id=o style='height: 100px'><div id=i style='height: 50%'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf i;
    EMB_REQUIRE(border_rect(&h, "i", &i));
    EMB_CHECK(near(i.h, 50));
    lh_free(&h);
}

EMB_TEST_MAIN()
