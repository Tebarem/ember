/*
 * Flex containers (W3e): fragment geometry for the flexible lengths, min and max violations, wrap-reverse,
 * auto margins, gap, space-evenly, baselines, the automatic minimum, and aspect-ratio items. Expected values
 * are derived by hand from CSS Flexbox 1 (sections 9.2 to 9.7 and 8) and match Chromium for these cases.
 * The UA body margin (8px) is checked too, since the align-content WPT tests depend on it.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

/* Every test builds and frees its own engine: the live allocation count must return to what it was before. */
static size_t g_mem_before;

static lh fx_make(const char *html, const char *css)
{
    static bool warmed;
    if (!warmed) {
        /* The first engine loads process-wide font data that stays allocated: do that first. */
        lh w = lh_make("<body><div style='display:flex'>x <b>y</b></div></body>", "");
        lh_free(&w);
        warmed = true;
    }
    g_mem_before = embi_mem_live();
    return lh_make(html, css);
}

static void fx_free(lh *h)
{
    lh_free(h);
    EMB_CHECK(embi_mem_live() == g_mem_before);
}

static bool rect_of(lh *h, const char *id, embi_rectf *out)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, out);
}

EMB_TEST(flex_ua_body_margin_is_eight_pixels)
{
    lh_keep_ua_body_margin = true;
    lh h = fx_make("<body><div id=d style='height:10px'></div></body>", "");
    lh_keep_ua_body_margin = false;
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "d", &r));
    EMB_CHECK(near(r.x, 8));
    EMB_CHECK(near(r.y, 8));
    fx_free(&h);
}

EMB_TEST(flex_grow_distributes_free_space_by_factor)
{
    lh h = fx_make("<body><div id=c style='display:flex;width:300px;height:10px'>"
                   "<div id=a style='flex:1 1 0;height:10px'></div>"
                   "<div id=b style='flex:2 1 0;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 100));
    EMB_CHECK(near(b.w, 200));
    EMB_CHECK(near(b.x, 100));
    fx_free(&h);
}

EMB_TEST(flex_shrink_scales_by_basis_times_factor)
{
    /* Two items of 80px in a 100px line: each shrinks by 30 (equal scaled factors). */
    lh h = fx_make("<body><div id=c style='display:flex;width:100px'>"
                   "<div id=a style='width:80px;height:10px;flex-shrink:1'></div>"
                   "<div id=b style='width:80px;height:10px;flex-shrink:1'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 50));
    EMB_CHECK(near(b.w, 50));
    fx_free(&h);
}

EMB_TEST(flex_max_violation_freezes_item_and_gives_rest_to_others)
{
    /* a has max-width 50: it is frozen at 50 and b takes the remaining 250. */
    lh h = fx_make("<body><div id=c style='display:flex;width:300px'>"
                   "<div id=a style='flex:1 1 0;max-width:50px;height:10px'></div>"
                   "<div id=b style='flex:1 1 0;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 50));
    EMB_CHECK(near(b.w, 250));
    fx_free(&h);
}

EMB_TEST(flex_min_violation_on_negative_free_space)
{
    /* Free space -60 is shared equally, but a cannot go below its min-width 70: it is frozen there and b
     * shrinks to what is left (100 - 70 = 30). */
    lh h = fx_make("<body><div id=c style='display:flex;width:100px'>"
                   "<div id=a style='width:80px;min-width:70px;height:10px'></div>"
                   "<div id=b style='width:80px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 70));
    EMB_CHECK(near(b.w, 30));
    fx_free(&h);
}

EMB_TEST(flex_zero_free_space_with_inflexible_items_keeps_sizes)
{
    /* Items with flex: 0 never grow or shrink; free space 0 leaves them at 50 each. */
    lh h = fx_make("<body><div id=c style='display:flex;width:100px'>"
                   "<div id=a style='flex:0 0 auto;width:50px;height:10px'></div>"
                   "<div id=b style='flex:0 0 auto;width:50px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 50));
    EMB_CHECK(near(b.w, 50));
    EMB_CHECK(near(b.x, 50));
    fx_free(&h);
}

EMB_TEST(flex_wrap_reverse_stacks_first_line_at_the_bottom)
{
    /* Two 30px items in a 50px row wrap into two lines. wrap-reverse puts line 1 at the bottom of the
     * 100px container (flex-start is the bottom); each line is 50px after stretch, and its item sits at
     * the line's bottom edge. */
    lh h = fx_make("<body><div id=c style='display:flex;flex-wrap:wrap-reverse;width:50px;height:100px;"
                   "align-items:flex-start'>"
                   "<div id=a style='width:30px;height:10px'></div>"
                   "<div id=b style='width:30px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.y, 90));
    EMB_CHECK(near(b.y, 40));
    fx_free(&h);
}

EMB_TEST(flex_auto_margin_absorbs_free_space)
{
    /* a's auto left margin takes the 100px of free space; b follows it. */
    lh h = fx_make("<body><div id=c style='display:flex;width:200px'>"
                   "<div id=a style='width:50px;height:10px;margin-left:auto'></div>"
                   "<div id=b style='width:50px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 100));
    EMB_CHECK(near(b.x, 150));
    fx_free(&h);
}

EMB_TEST(flex_gap_separates_items)
{
    lh h = fx_make("<body><div id=c style='display:flex;width:200px;column-gap:10px'>"
                   "<div id=a style='width:50px;height:10px'></div>"
                   "<div id=b style='width:50px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.x, 60));
    fx_free(&h);
}

EMB_TEST(flex_space_evenly_splits_free_space_into_n_plus_one_gaps)
{
    /* 100px free in 200px for 2 items: 3 gaps of 33.33. */
    lh h = fx_make("<body><div id=c style='display:flex;width:200px;justify-content:space-evenly'>"
                   "<div id=a style='width:50px;height:10px'></div>"
                   "<div id=b style='width:50px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(fabs(a.x - 100.0 / 3) < 0.05);
    EMB_CHECK(fabs(b.x - (200.0 / 3 + 50)) < 0.05);
    fx_free(&h);
}

EMB_TEST(flex_row_reverse_starts_at_the_end)
{
    lh h = fx_make("<body><div id=c style='display:flex;flex-direction:row-reverse;width:200px'>"
                   "<div id=a style='width:50px;height:10px'></div>"
                   "<div id=b style='width:50px;height:10px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 150));
    EMB_CHECK(near(b.x, 100));
    fx_free(&h);
}

EMB_TEST(flex_baseline_without_text_uses_the_bottom_edge)
{
    /* Items with no baseline synthesize one from their border box bottom: the bottoms line up. */
    lh h = fx_make("<body><div id=c style='display:flex;align-items:baseline;width:200px'>"
                   "<div id=a style='width:10px;height:20px'></div>"
                   "<div id=b style='width:10px;height:40px'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.y, 0));
    EMB_CHECK(near(a.y, 20));
    fx_free(&h);
}

EMB_TEST(flex_automatic_minimum_keeps_content_size)
{
    /* The automatic minimum is min(content size, specified size) = 80, so the item does not shrink to 50. */
    lh h = fx_make("<body><div id=c style='display:flex;width:50px'>"
                   "<div id=a style='width:80px;height:10px'><div style='width:100px;height:5px'></div></div>"
                   "</div></body>",
                   "body{margin:0}");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 80));
    fx_free(&h);
}

EMB_TEST(flex_aspect_ratio_item_derives_cross_size)
{
    /* width 40 with aspect-ratio 2/1 gives height 20 (the cross size with flex-start alignment). */
    lh h = fx_make("<body><div id=c style='display:flex;height:100px;align-items:flex-start;width:200px'>"
                   "<div id=a style='width:40px;aspect-ratio:2 / 1'></div></div></body>",
                   "body{margin:0}");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 40));
    EMB_CHECK(near(a.h, 20));
    fx_free(&h);
}

EMB_TEST(br_clear_moves_the_next_line_below_the_floats)
{
    /* A <br clear: both> after a float ends its line; the next float starts below the first one (10px),
     * not beside it: the float is 50px tall, so the next line starts at 50 (the br line itself is 22px). Shared
     * inline code (inline.c), covered here because flex tests use it. */
    lh h = fx_make("<body><div id=f style='float:left;width:10px;height:50px'></div><br style='clear:both'>"
                   "<div id=g style='float:left;width:10px;height:10px'></div></body>",
                   "body{margin:0}");
    embi_rectf f, g;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_REQUIRE(rect_of(&h, "g", &g));
    EMB_CHECK(near(f.y, 0));
    EMB_CHECK(near(g.y, 50));
    EMB_CHECK(near(g.x, 0));
    fx_free(&h);
}

EMB_TEST_MAIN()
