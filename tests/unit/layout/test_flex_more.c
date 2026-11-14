/*
 * Flex containers, second part (W3e): intrinsic sizes, baselines, absolutely positioned children, and
 * the shared-code fixes that the flexbox WPT tests needed. Expected values are derived by hand from CSS
 * Flexbox 1 and the sections of CSS Sizing 3 they cite; where Chromium's behavior is what the WPT test
 * asserts, the comment names the test. Every test also checks that no layout memory is left behind.
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

EMB_TEST(floats_in_a_shrink_to_fit_box_add_up_in_max_content)
{
    /* Two 75px floats that follow each other sit side by side when the width is unbounded. */
    lh h = fx_make("<body><div id=o style='float:left'>"
                   "<div style='float:left;width:75px;height:10px'></div>"
                   "<div style='float:left;width:75px;height:10px'></div></div></body>",
                   "");
    embi_rectf o;
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_CHECK(near(o.w, 150));
    fx_free(&h);
}

EMB_TEST(last_baseline_lines_the_item_bottoms_up_at_the_cross_end)
{
    /* Boxes without text have their border-box bottom as both baselines (CSS Align 3 section 9.1). Last
     * baseline alignment puts the group flush with the cross end of the 60px line: bottoms at 60. */
    lh h = fx_make("<body><div style='display:flex;align-items:last baseline;width:100px;height:60px'>"
                   "<div id=a style='width:10px;height:20px'></div>"
                   "<div id=b style='width:10px;height:30px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.y, 40));
    EMB_CHECK(near(b.y, 30));
    fx_free(&h);
}

EMB_TEST(nested_flex_container_baseline_comes_from_its_first_item)
{
    /* The inner container's first baseline is its first item's: the bottom edge of the 10px box (no text).
     * The 20px box has its bottom edge, 20, so the larger ascent is 20 and the inner container sits at
     * 20 - 10 = 10 (CSS Flexbox 1 section 8.5). */
    lh h = fx_make("<body><div style='display:flex;align-items:baseline;width:100px'>"
                   "<div id=a style='width:10px;height:20px'></div>"
                   "<div id=i style='display:flex;width:30px'>"
                   "<div style='width:10px;height:10px'></div><div style='width:10px;height:30px'></div>"
                   "</div></div></body>",
                   "");
    embi_rectf a, i;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "i", &i));
    EMB_CHECK(near(a.y, 0));
    EMB_CHECK(near(i.y, 10));
    fx_free(&h);
}

EMB_TEST(column_cross_axis_baseline_uses_the_start_edge)
{
    /* In a column the cross axis is inline: a box's baseline there is its border-box start edge, so
     * baseline alignment lines up the border boxes, not the margin boxes (WPT col-wrap-baseline-001). */
    lh h = fx_make("<body><div style='display:flex;flex-direction:column;align-items:baseline;width:100px'>"
                   "<div id=a style='margin-left:20px;width:10px;height:5px'></div>"
                   "<div id=b style='margin-right:20px;width:10px;height:5px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 20));
    EMB_CHECK(near(b.x, 20));
    fx_free(&h);
}

EMB_TEST(abspos_flex_container_with_both_insets_gives_its_items_the_definite_height)
{
    /* top:0 and bottom:0 on a positioned container make its height 100 (CSS 2.1 section 10.6.4), and that
     * height is definite for the column's flexible lengths: 100 - 2 * 10 padding - 30 = 50 for flex: 1. */
    lh h = fx_make("<body><div style='position:relative;height:100px;width:100px'>"
                   "<div style='display:flex;flex-direction:column;position:absolute;top:0;bottom:0;left:0;"
                   "right:0;padding:10px'><div id=a style='height:30px'></div>"
                   "<div id=b style='flex:1'></div></div></div></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.h, 50));
    EMB_CHECK(near(b.w, 80));
    fx_free(&h);
}

EMB_TEST(automatic_minimum_ignores_a_scroll_container_only_on_the_cross_axis)
{
    /* overflow-x: clip and overflow-y: hidden scrolls in y only, so in a row the automatic minimum
     * (the 100px content) still applies in x; with overflow-x: hidden it is 0 (CSS Flexbox 1 4.5). */
    lh h = fx_make("<body><div style='display:flex;width:0'>"
                   "<div id=a style='overflow-x:clip;overflow-y:hidden'><div style='width:100px;height:10px'></div></div>"
                   "</div><div style='display:flex;width:0'>"
                   "<div id=b style='overflow-x:hidden;overflow-y:clip'><div style='width:100px;height:10px'></div></div>"
                   "</div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 100));
    EMB_CHECK(near(b.w, 0));
    fx_free(&h);
}

EMB_TEST(min_height_max_content_means_the_content_height)
{
    /* height: 0 with min-height: max-content gives the height the box has with an auto height (CSS Sizing
     * 3 section 4: keywords behave as auto in the block axis): the 25px item. */
    lh h = fx_make("<body><div id=c style='display:flex;height:0;min-height:max-content;width:50px'>"
                   "<div style='width:10px;height:25px'></div></div></body>",
                   "");
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.h, 25));
    fx_free(&h);
}

EMB_TEST(min_width_fit_content_is_min_of_max_content_and_the_larger_of_min_content_and_available)
{
    /* Items 20px wide in a wrapping row: min-content 20, max-content 40, available 800: fit-content is 40,
     * so the 10px width is raised to 40. */
    lh h = fx_make("<body><div id=c style='display:flex;flex-wrap:wrap;width:10px;min-width:fit-content'>"
                   "<div style='width:20px;height:5px'></div><div style='width:20px;height:5px'></div>"
                   "</div></body>",
                   "");
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.w, 40));
    fx_free(&h);
}

EMB_TEST(button_with_display_flex_is_shrink_to_fit)
{
    /* A button's auto width is shrink-to-fit even when it is block-level: an empty flex button with
     * 100px of left padding is 100px wide (WPT gap-018). */
    lh h = fx_make("<body><button id=b style='display:flex;border:0;padding:0 0 0 100px;column-gap:100px'>"
                   "</button></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.w, 100));
    fx_free(&h);
}

EMB_TEST(stretched_flex_item_is_a_percentage_base_for_relative_offsets)
{
    /* The item is stretched to the 100px line, which is definite, so top: 50% is 50px. */
    lh h = fx_make("<body><div style='display:flex;height:100px'><div style='width:20px'>"
                   "<div id=r style='position:relative;top:50%;height:10px'></div></div></div></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "r", &r));
    EMB_CHECK(near(r.y, 50));
    fx_free(&h);
}

EMB_TEST(size_containment_ignores_the_content_height)
{
    /* contain: size: the auto height does not come from the 25px content (CSS Containment 2 section 3). */
    lh h = fx_make("<body><div id=a style='contain:size'><div style='height:25px'></div></div></body>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 0));
    fx_free(&h);
}

EMB_TEST(wrapping_column_container_is_as_wide_as_the_sum_of_its_lines)
{
    /* height: 100 and two 100px-tall items: one per line, 30px and 40px wide, so the floating container's
     * max-content width is 70 (CSS Flexbox 1 section 9.9.2 as Chromium does it). */
    lh h = fx_make("<body><div id=c style='float:left;display:flex;flex-flow:column wrap;height:100px'>"
                   "<div style='width:30px;flex:0 0 100px;min-height:0'></div>"
                   "<div style='width:40px;flex:0 0 100px;min-height:0'></div></div></body>",
                   "");
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.w, 70));
    fx_free(&h);
}

EMB_TEST(row_container_min_content_follows_the_contributions_of_items_that_can_shrink)
{
    /* WPT intrinsic-size/row-005 case 1: in a 0px parent a floating row container is min-content wide. Each
     * item can shrink and contributes its 100px min-content contribution: 200. */
    lh h = fx_make("<body><div style='width:0'><div id=c style='float:left;display:flex'>"
                   "<div style='flex:1 1 200px;width:50px;min-width:0'><div style='width:100px;height:5px'></div></div>"
                   "<div style='flex:1 1 400px;width:50px'><div style='width:100px;height:5px'></div></div>"
                   "</div></div></body>",
                   "");
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.w, 200));
    fx_free(&h);
}

EMB_TEST(canvas_with_a_ratio_in_a_stretched_row_gets_its_width_from_the_stretched_height)
{
    /* A canvas 20x10 (ratio 2) stretched to the 100px height is 200px wide: the stretched cross size is
     * definite (CSS Flexbox 1 section 9.8) and transfers through the ratio. */
    lh h = fx_make("<body><div style='display:flex;height:100px;width:1000px'>"
                   "<canvas id=i width=20 height=10></canvas></div></body>",
                   "");
    embi_rectf i;
    EMB_REQUIRE(rect_of(&h, "i", &i));
    EMB_CHECK(near(i.h, 100));
    EMB_CHECK(near(i.w, 200));
    fx_free(&h);
}

/* Collects the rectangles of the elements a, b, c, d (when present) and the container o. */
static bool rects(lh *h, const char *ids, embi_rectf *out)
{
    for (size_t i = 0; ids[i] != 0; i++) {
        char id[2] = {ids[i], 0};
        if (!rect_of(h, id, &out[i])) {
            return false;
        }
    }
    return true;
}

EMB_TEST(collapsed_row_item_is_a_strut_with_its_cross_size)
{
    /* CSS Flexbox 1 section 4.4: b has no main size but its 30px cross size still sizes the line, so the
     * container is 30px high and c sits right after a at x = 50. */
    lh h = fx_make("<body><div id=o style='display:flex;width:300px;align-items:flex-start'>"
                   "<div id=a style='flex:none;width:50px;height:10px'></div>"
                   "<div id=b style='flex:none;width:50px;height:30px;visibility:collapse'></div>"
                   "<div id=c style='flex:none;width:50px;height:10px'></div></div></body>",
                   "");
    embi_rectf r[4];
    EMB_REQUIRE(rects(&h, "oac", r));
    EMB_CHECK(near(r[0].h, 30));
    EMB_CHECK(near(r[1].x - r[0].x, 0));
    EMB_CHECK(near(r[2].x - r[0].x, 50));
    EMB_CHECK(near(r[2].w, 50));
    fx_free(&h);
}

EMB_TEST(collapsed_column_item_is_a_strut_with_its_cross_size)
{
    /* In a column the strut has no height but its 80px width still counts: the shrink-to-fit container is
     * 80px wide and 20px high, c at y = 10. */
    lh h = fx_make("<body><div id=o style='float:left;display:flex;flex-direction:column;align-items:flex-start'>"
                   "<div id=a style='width:20px;height:10px'></div>"
                   "<div id=b style='width:80px;height:30px;visibility:collapse'></div>"
                   "<div id=c style='width:30px;height:10px'></div></div></body>",
                   "");
    embi_rectf r[4];
    EMB_REQUIRE(rects(&h, "oac", r));
    EMB_CHECK(near(r[0].w, 80));
    EMB_CHECK(near(r[0].h, 20));
    EMB_CHECK(near(r[2].y - r[0].y, 10));
    fx_free(&h);
}

EMB_TEST(collapsed_item_in_a_wrapping_container_sizes_its_line_and_does_not_break_it)
{
    /* Row wrap, 100px wide, 60px items. a + b (a strut, 0 wide) fit on line 1, whose cross size is b's 30.
     * c is on line 2 at y = 30 (10 high) and d on line 3 at y = 40; the container is 50px high. */
    lh h = fx_make("<body><div id=o style='display:flex;flex-wrap:wrap;width:100px;align-items:flex-start'>"
                   "<div id=a style='flex:none;width:60px;height:10px'></div>"
                   "<div id=b style='flex:none;width:60px;height:30px;visibility:collapse'></div>"
                   "<div id=c style='flex:none;width:60px;height:10px'></div>"
                   "<div id=d style='flex:none;width:60px;height:10px'></div></div></body>",
                   "");
    embi_rectf r[5];
    EMB_REQUIRE(rects(&h, "oacd", r));
    EMB_CHECK(near(r[0].h, 50));
    EMB_CHECK(near(r[2].x - r[0].x, 0));
    EMB_CHECK(near(r[2].y - r[0].y, 30));
    EMB_CHECK(near(r[3].y - r[0].y, 40));
    fx_free(&h);
}

EMB_TEST(collapsed_item_contributes_no_main_size_to_the_intrinsic_width)
{
    /* A shrink-to-fit row of 50 + strut + 50 is 100px wide. */
    lh h = fx_make("<body><div id=o style='float:left;display:flex'>"
                   "<div style='width:50px;height:10px'></div>"
                   "<div style='width:70px;height:10px;visibility:collapse'></div>"
                   "<div style='width:50px;height:10px'></div></div></body>",
                   "");
    embi_rectf o;
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_CHECK(near(o.w, 100));
    fx_free(&h);
}

EMB_TEST(shrink_factor_sum_below_one_shrinks_only_that_fraction)
{
    /* CSS Flexbox 1 section 9.7 step 4c: two items of 200px with shrink 0.2 in 100px. The initial free
     * space is -300, times the factor sum 0.4 is -120, shared by the scaled shrink factors: 140 each. */
    lh h = fx_make("<body><div style='display:flex;width:100px'>"
                   "<div id=a style='flex:0 0.2 200px;height:10px'></div>"
                   "<div id=b style='flex:0 0.2 200px;height:10px'></div></div></body>",
                   "");
    embi_rectf r[2];
    EMB_REQUIRE(rects(&h, "ab", r));
    EMB_CHECK(near(r[0].w, 140));
    EMB_CHECK(near(r[1].w, 140));
    fx_free(&h);
}

EMB_TEST(grow_factor_sum_below_one_uses_the_initial_free_space_on_every_pass)
{
    /* Two flex:0.3 1 0 items in 100px, a capped at 10px. Pass 1: free space 100 * 0.6 = 60, 30 each, a
     * is clamped to 10 and frozen. Pass 2 is again 100 * 0.3 = 30 (the initial free space, not the
     * remaining 90 times 0.3 = 27): b is 30. */
    lh h = fx_make("<body><div style='display:flex;width:100px'>"
                   "<div id=a style='flex:0.3 1 0;max-width:10px;height:10px'></div>"
                   "<div id=b style='flex:0.3 1 0;height:10px'></div></div></body>",
                   "");
    embi_rectf r[2];
    EMB_REQUIRE(rects(&h, "ab", r));
    EMB_CHECK(near(r[0].w, 10));
    EMB_CHECK(near(r[1].w, 30));
    fx_free(&h);
}

EMB_TEST(justify_content_with_negative_free_space)
{
    /* Two 80px items that cannot shrink in 100px: -60 free. center overflows both ends equally, flex-end
     * to the start side only. */
    lh h = fx_make("<body><div id=o style='display:flex;width:100px;justify-content:center'>"
                   "<div id=a style='flex:none;width:80px;height:10px'></div>"
                   "<div id=b style='flex:none;width:80px;height:10px'></div></div>"
                   "<div id=p style='display:flex;width:100px;justify-content:flex-end'>"
                   "<div id=c style='flex:none;width:80px;height:10px'></div>"
                   "<div id=d style='flex:none;width:80px;height:10px'></div></div></body>",
                   "");
    embi_rectf r[2], q[2], o, p;
    EMB_REQUIRE(rects(&h, "ab", r));
    EMB_REQUIRE(rects(&h, "cd", q));
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_REQUIRE(rect_of(&h, "p", &p));
    EMB_CHECK(near(r[0].x - o.x, -30));
    EMB_CHECK(near(r[1].x - o.x, 50));
    EMB_CHECK(near(q[0].x - p.x, -60));
    EMB_CHECK(near(q[1].x - p.x, 20));
    fx_free(&h);
}

EMB_TEST(items_are_placed_by_order_and_ties_keep_the_document_order)
{
    /* order 3, 2, 2, 1, 0: the main order is e, d, b-c (the tie keeps b before c), a. */
    lh h = fx_make("<body><div id=o style='display:flex;width:500px'>"
                   "<div id=a style='order:3;flex:none;width:10px;height:5px'></div>"
                   "<div id=b style='order:2;flex:none;width:20px;height:5px'></div>"
                   "<div id=c style='order:2;flex:none;width:30px;height:5px'></div>"
                   "<div id=d style='order:1;flex:none;width:40px;height:5px'></div>"
                   "<div id=e style='order:0;flex:none;width:50px;height:5px'></div></div></body>",
                   "");
    embi_rectf r[6], o;
    EMB_REQUIRE(rects(&h, "abcde", r));
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_CHECK(near(r[4].x - o.x, 0));
    EMB_CHECK(near(r[3].x - o.x, 50));
    EMB_CHECK(near(r[1].x - o.x, 90));
    EMB_CHECK(near(r[2].x - o.x, 110));
    EMB_CHECK(near(r[0].x - o.x, 140));
    fx_free(&h);
}

EMB_TEST_MAIN()
