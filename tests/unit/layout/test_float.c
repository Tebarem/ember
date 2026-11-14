/*
 * Floats and BFC interactions (W3b): placement of floats, the flow around them, clearance, boxes
 * that establish a BFC beside floats, and a BFC root's height. Expected values are derived by hand
 * from CSS 2.1 sections 9.5, 9.5.1, 9.5.2 and 10.6.7. The WPT and fixture checks run in
 * test_layout_wpt.c (tests/layout/float/).
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

/* A container of width 200 with the given inner HTML, body margin 0, and its position relative so
 * that the children's coordinates are relative to it (its own is 0, 0). */
static lh float_doc(const char *inner, const char *extra_css)
{
    static char html[1024];
    SDL_snprintf(html, sizeof html, "<body><div id=c>%s</div></body>", inner);
    char css[512];
    SDL_snprintf(css, sizeof css,
                 "body { margin: 0 } #c { position: relative; width: 200px } %s", extra_css);
    return lh_make(html, css);
}

EMB_TEST(float_left_stacks_beside_an_earlier_left_float)
{
    lh h = float_doc("<div id=a style='float:left; width:50px; height:20px'></div>"
                     "<div id=b style='float:left; width:50px; height:20px'></div>",
                     "");
    embi_rectf a, b;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(a.x, 0) && near(a.y, 0));
    EMB_CHECK(near(b.x, 50) && near(b.y, 0));
    lh_free(&h);
}

EMB_TEST(float_that_does_not_fit_moves_below_the_earlier_floats)
{
    lh h = float_doc("<div id=a style='float:left; width:150px; height:20px'></div>"
                     "<div id=b style='float:left; width:100px; height:20px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* 150 + 100 > 200: the second float goes to the first position below the first float's bottom. */
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 20));
    lh_free(&h);
}

EMB_TEST(right_float_sits_at_the_right_edge)
{
    lh h = float_doc("<div id=a style='float:right; width:60px; height:20px'></div>", "");
    embi_rectf a;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_CHECK(near(a.x, 140));
    EMB_CHECK(near(a.y, 0));
    lh_free(&h);
}

EMB_TEST(float_does_not_take_room_in_the_flow)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:100px'></div>"
                     "<div id=b style='height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* The block ignores the float: it starts at the top and fills the width (CSS 2.1 9.5). */
    EMB_CHECK(near(b.y, 0));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.w, 200));
    lh_free(&h);
}

EMB_TEST(inline_start_float_is_on_the_right_in_rtl)
{
    lh h = float_doc("<div id=a style='float:inline-start; width:50px; height:20px'></div>",
                     "#c { direction: rtl }");
    embi_rectf a;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_CHECK(near(a.x, 150));
    lh_free(&h);
}

EMB_TEST(float_margins_widen_the_margin_box_it_is_placed_by)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:20px; margin:10px'></div>"
                     "<div id=b style='overflow:hidden; height:10px'></div>",
                     "");
    embi_rectf f, b;
    EMB_REQUIRE(border_rect(&h, "f", &f));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* The margin box is 70 wide: the BFC box starts at 70 and is 130 wide. */
    EMB_CHECK(near(f.x, 10) && near(f.y, 10));
    EMB_CHECK(near(b.x, 70));
    EMB_CHECK(near(b.w, 130));
    lh_free(&h);
}

EMB_TEST(clear_left_moves_the_block_below_the_left_float)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:40px'></div>"
                     "<div id=b style='clear:left; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.y, 40));
    lh_free(&h);
}

EMB_TEST(clear_absorbs_the_margin_above_the_block)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:40px'></div>"
                     "<div id=b style='clear:left; margin-top:10px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* The hypothetical position is 10 (its margin); the clearance takes it to the float's bottom
     * at 40, and the margin does not add to that. */
    EMB_CHECK(near(b.y, 40));
    lh_free(&h);
}

EMB_TEST(clear_right_ignores_left_floats)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:40px'></div>"
                     "<div id=b style='clear:right; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.y, 0));
    lh_free(&h);
}

/* clear on a float (CSS 2.1 9.5.2): the border top goes below the floats on the cleared sides. */
EMB_TEST(float_clear_left_goes_below_an_earlier_left_float)
{
    lh h = float_doc("<div id=a style='float:left; width:50px; height:20px'></div>"
                     "<div id=b style='float:left; clear:left; width:50px; height:20px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 20));
    lh_free(&h);
}

EMB_TEST(float_clear_left_ignores_right_floats)
{
    lh h = float_doc("<div id=a style='float:right; width:50px; height:40px'></div>"
                     "<div id=b style='float:left; clear:left; width:50px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 0));
    lh_free(&h);
}

EMB_TEST(float_clear_right_goes_below_an_earlier_right_float)
{
    lh h = float_doc("<div id=a style='float:right; width:50px; height:20px'></div>"
                     "<div id=b style='float:right; clear:right; width:50px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 150));
    EMB_CHECK(near(b.y, 20));
    lh_free(&h);
}

/* The clearance bound is the margin box top of a float (Chromium: the border top is 10 below the
 * float's bottom at 20, because of the 10px margin-top). */
EMB_TEST(float_clear_puts_the_margin_box_below_the_float)
{
    lh h = float_doc("<div id=a style='float:left; width:50px; height:20px'></div>"
                     "<div id=b style='float:left; clear:left; margin-top:10px; width:50px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 30));
    lh_free(&h);
}

EMB_TEST(float_clear_both_goes_below_the_lower_of_both_sides)
{
    lh h = float_doc("<div id=a style='float:left; width:50px; height:30px'></div>"
                     "<div id=r style='float:right; width:50px; height:50px'></div>"
                     "<div id=b style='float:left; clear:both; width:50px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* The left float ends at 30, the right one at 50: the border top goes to 50, at the left edge. */
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 50));
    lh_free(&h);
}

/* An absorbed child (first in-flow child, its top margin inside the parent's) that clears a float
 * taller than the margin: the parent's margin does not collapse through a box with clearance
 * (CSS 2.1 9.5.2, "clearance inhibits margin collapsing"), so the parent and the float stay at 0 and
 * the child's border top goes to the float's bottom. It must not overlap the float. */
EMB_TEST(clearing_first_child_with_a_shorter_float_than_its_margin_does_not_overlap)
{
    lh h = float_doc("<div id=f style='float:left; width:50px; height:5px'></div>"
                     "<div id=b style='clear:left; margin-top:10px; height:10px'></div>",
                     "");
    embi_rectf f, b;
    EMB_REQUIRE(border_rect(&h, "f", &f));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(f.y, 0));
    EMB_CHECK(near(b.y, 5));
    EMB_CHECK(b.y >= f.y + f.h - 0.01f);
    lh_free(&h);
}

/* A later sibling whose top margin lies below a short float: the float is no obstacle, so the
 * margin stays as it is (no clearance) and the box is at its hypothetical position, 20. */
EMB_TEST(clear_sibling_below_a_short_float_keeps_its_margin)
{
    lh h = float_doc("<div style='height:10px'></div>"
                     "<div id=f style='float:left; width:50px; height:5px'></div>"
                     "<div id=b style='clear:left; margin-top:10px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.y, 20));
    lh_free(&h);
}

/* The same as above with a float that reaches past the margin: the box goes to the float's bottom. */
EMB_TEST(clear_sibling_above_a_tall_float_goes_to_its_bottom)
{
    lh h = float_doc("<div style='height:10px'></div>"
                     "<div id=f style='float:left; width:50px; height:30px'></div>"
                     "<div id=b style='clear:left; margin-top:10px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.y, 40));
    lh_free(&h);
}

EMB_TEST(bfc_box_takes_the_space_beside_a_float)
{
    lh h = float_doc("<div id=f style='float:left; width:100px; height:50px'></div>"
                     "<div id=b style='overflow:hidden; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* Its auto width fills the free interval 100..200. */
    EMB_CHECK(near(b.x, 100));
    EMB_CHECK(near(b.w, 100));
    EMB_CHECK(near(b.y, 0));
    lh_free(&h);
}

EMB_TEST(bfc_box_too_wide_for_the_free_space_moves_below_the_float)
{
    lh h = float_doc("<div id=f style='float:left; width:100px; height:50px'></div>"
                     "<div id=b style='overflow:hidden; width:150px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 50));
    lh_free(&h);
}

EMB_TEST(zero_width_bfc_box_fits_between_two_floats)
{
    lh h = float_doc("<div id=a style='float:left; width:100px; height:200px'></div>"
                     "<div id=r style='float:right; width:100px; height:200px'></div>"
                     "<div id=b style='overflow:hidden; width:0; height:200px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 100));
    EMB_CHECK(near(b.y, 0));
    lh_free(&h);
}

EMB_TEST(bfc_root_height_contains_its_floats)
{
    lh h = lh_make("<body><div id=b style='overflow:hidden; width:200px'>"
                   "<div style='float:left; width:50px; height:40px'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.h, 40));
    lh_free(&h);
}

EMB_TEST(plain_block_root_height_ignores_its_floats)
{
    lh h = lh_make("<body><div id=b style='width:200px'>"
                   "<div style='float:left; width:50px; height:40px'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.h, 0));
    lh_free(&h);
}

EMB_TEST(float_inside_a_plain_block_shares_the_bfc_of_its_parent)
{
    lh h = float_doc("<div id=in><div id=f style='float:left; width:100px; height:20px'></div></div>"
                     "<div id=b style='overflow:hidden; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    /* The float inside #in is in the BFC of #c (the root of the flow here), so the BFC box beside
     * it starts at 100. */
    EMB_CHECK(near(b.x, 100));
    EMB_CHECK(near(b.w, 100));
    lh_free(&h);
}

EMB_TEST(float_percentage_width_is_of_the_containing_block)
{
    lh h = float_doc("<div id=a style='float:left; width:25%; height:20px'></div>", "");
    embi_rectf a;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_CHECK(near(a.w, 50));
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_float_takes_its_content_width)
{
    lh h = float_doc("<div id=a style='float:left'><div style='width:30px; height:5px'></div></div>",
                     "");
    embi_rectf a;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_CHECK(near(a.w, 30));
    lh_free(&h);
}

/* Seventy floats stacked one per row, each too wide to sit beside the one above it, then a BFC box
 * that is too wide beside any of them. Each placement step passes one float, so the box runs past
 * the step cap and goes below every float at full width: row 70, at the left edge. */
EMB_TEST(bfc_box_past_the_placement_cap_goes_below_every_float)
{
    static char html[16384];
    size_t n = (size_t)SDL_snprintf(html, sizeof html, "<body><div id=c>");
    for (int i = 0; i < 70; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n,
                                  "<div style='float:left; width:150px; height:1px'></div>");
    }
    SDL_snprintf(html + n, sizeof html - n,
                 "<div id=b style='overflow:hidden; width:100px; height:10px'></div></div></body>");
    lh h = lh_make(html, "body { margin: 0 } #c { position: relative; width: 200px }");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 0));
    EMB_CHECK(near(b.y, 70));
    lh_free(&h);
}

/* An empty block that clears (the clearfix pattern) does not collapse through (CSS 2.1 section 8.3.1):
 * the next sibling follows the clearance, and the parent's top does not take the empty box's margins.
 * Expected values are Chromium's: k2 = float bottom - margin-top + max(margin-top, margin-bottom). */
static void cleared_empty(const char *margin, double k2y, double cy)
{
    char inner[512];
    SDL_snprintf(inner, sizeof inner,
                 "<div style='float:left; width:50px; height:30px'></div>"
                 "<div id=k1 style='clear:both; %s'></div>"
                 "<div id=k2 style='height:10px'></div>",
                 margin);
    lh h = float_doc(inner, "");
    embi_rectf k2, c;
    EMB_REQUIRE(border_rect(&h, "k2", &k2));
    EMB_REQUIRE(border_rect(&h, "c", &c));
    EMB_CHECK(near(k2.y, k2y));
    EMB_CHECK(near(c.y, cy));
    lh_free(&h);
}

EMB_TEST(empty_cleared_block_advances_the_flow)
{
    cleared_empty("", 30, 0);
}

EMB_TEST(empty_cleared_block_bottom_margin_follows_the_clearance)
{
    cleared_empty("margin-bottom:6px", 36, 0);
}

EMB_TEST(empty_cleared_block_margins_collapse_together_inside_the_clearance)
{
    cleared_empty("margin:5px 0 8px", 33, 0);
}

EMB_TEST(empty_cleared_block_large_top_margin_does_not_move_the_parent)
{
    cleared_empty("margin-top:50px", 30, 0);
}

EMB_TEST(parent_height_includes_a_trailing_empty_cleared_block)
{
    lh h = float_doc("<div style='float:left; width:50px; height:30px'></div>"
                     "<div style='clear:both'></div>",
                     "#c { overflow: hidden }");
    embi_rectf c;
    EMB_REQUIRE(border_rect(&h, "c", &c));
    EMB_CHECK(near(c.h, 30));
    lh_free(&h);
}

/* A trailing empty cleared block in a non-BFC parent: its margins collapse with its own top margin
 * that has clearance, so they do not collapse through the parent's bottom (CSS 2.1 section 8.3.1).
 * The parent ends at the strut, and the sibling that follows starts there. Chromium's values. */
static void trailing_cleared(const char *margin, double ch, double ny)
{
    char html[512];
    SDL_snprintf(html, sizeof html,
                 "<body><div id=c><div style='float:left; width:50px; height:30px'></div>"
                 "<div style='clear:both; %s'></div></div><div id=n style='height:10px'></div></body>",
                 margin);
    lh h = lh_make(html, "body { margin: 0 } #c { width: 200px }");
    embi_rectf c, n;
    EMB_REQUIRE(border_rect(&h, "c", &c));
    EMB_REQUIRE(border_rect(&h, "n", &n));
    EMB_CHECK(near(c.h, ch));
    EMB_CHECK(near(n.y, ny));
    lh_free(&h);
}

EMB_TEST(trailing_cleared_block_bottom_margin_stays_inside_a_non_bfc_parent)
{
    trailing_cleared("margin-bottom:6px", 36, 36);
}

EMB_TEST(trailing_cleared_block_large_top_margin_stays_inside_a_non_bfc_parent)
{
    trailing_cleared("margin-top:50px", 30, 30);
}

EMB_TEST(trailing_cleared_block_collapsed_margins_stay_inside_a_non_bfc_parent)
{
    trailing_cleared("margin:5px 0 8px", 33, 33);
}

/* A BFC box keeps only its border box out of the float margin boxes (CSS 2.1 section 9.5): the
 * margin on the float's side is absorbed by the float band. Chromium's values. */
EMB_TEST(bfc_box_left_margin_beside_a_left_float_is_absorbed)
{
    lh h = float_doc("<div style='float:left; width:50px; height:30px'></div>"
                     "<div id=b style='overflow:hidden; margin:10px 5px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 50));
    EMB_CHECK(near(b.w, 145));
    lh_free(&h);
}

EMB_TEST(bfc_box_right_margin_beside_a_right_float_is_absorbed)
{
    lh h = float_doc("<div style='float:right; width:50px; height:30px'></div>"
                     "<div id=b style='overflow:hidden; margin:0 5px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 5));
    EMB_CHECK(near(b.w, 145));
    lh_free(&h);
}

EMB_TEST(bfc_box_negative_left_margin_does_not_overlap_the_float)
{
    lh h = float_doc("<div style='float:left; width:50px; height:30px'></div>"
                     "<div id=b style='overflow:hidden; margin-left:-20px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 50));
    EMB_CHECK(near(b.w, 150));
    lh_free(&h);
}

EMB_TEST(bfc_box_margin_larger_than_the_float_extends_past_it)
{
    lh h = float_doc("<div style='float:left; width:50px; height:30px'></div>"
                     "<div id=b style='overflow:hidden; margin-left:70px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, 70));
    EMB_CHECK(near(b.w, 130));
    lh_free(&h);
}

EMB_TEST(bfc_box_negative_margin_with_no_float_on_that_side_still_extends)
{
    lh h = float_doc("<div style='float:right; width:50px; height:30px'></div>"
                     "<div id=b style='overflow:hidden; margin-left:-20px; height:10px'></div>",
                     "");
    embi_rectf b;
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(b.x, -20));
    EMB_CHECK(near(b.w, 170));
    lh_free(&h);
}

EMB_TEST_MAIN()
