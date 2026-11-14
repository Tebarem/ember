/*
 * Layout fixes of the BUGBURN1 reftest burn-down (CSS2 tables and line boxes). Expected values are worked out by hand
 * from CSS 2.1; each test names the reftest that found the bug.
 */
#include "layout_harness.h"

#include <math.h>

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* table-anonymous-objects-211: an img with display: table-cell is an inline; it joins its siblings in an anonymous
 * cell and keeps its own height instead of filling the row. */
EMB_TEST(replaced_element_with_display_table_cell_is_inline)
{
    lh h = lh_make("<div style=\"display:table\"><div style=\"display:table-row\">"
                   "<img id=a width=20 height=10 style=\"display:table-cell\">"
                   "<div style=\"display:table-cell;height:50px\"></div></div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 20.0f));
    EMB_CHECK(near(a.h, 10.0f));
    lh_free(&h);
}

/* table-anonymous-objects-211: preserved white space ahead of the first inline of an anonymous cell is part of the
 * cell (white-space: pre): the image sits one space (the font's advance) to the right of the cell's edge. */
EMB_TEST(preserved_white_space_starts_the_anonymous_cell)
{
    lh h = lh_make("<div style=\"display:table;white-space:pre;font:10px sans-serif\"><div style=\"display:table-row\"> "
                   "<img id=a width=20 height=10></div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(a.x > 1.0f); /* the width of the space; without the fix the cell starts at the image */
    lh_free(&h);
}

/* line-height-129: an image whose margin box is zero high (margin-bottom: -100px on 100px), aligned to the bottom of a
 * line with line-height: 0, leaves the line box zero high. */
EMB_TEST(negative_margin_shrinks_the_atomic_inline_margin_box)
{
    lh h = lh_make("<div id=d style=\"line-height:0\"><img width=20 height=100 "
                   "style=\"margin-bottom:-100px;vertical-align:bottom\"></div>",
                   "");
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.h, 0.0f));
    lh_free(&h);
}

/* vertical-align-117a: text-bottom aligns the bottom of the inline box, half-leading included, with the bottom of the
 * parent's content area: the box reaches half-leading (130px - content height) / 2 below its content area, so the
 * span's baseline rises by that much against the surrounding text. */
EMB_TEST(text_bottom_aligns_the_inline_box_with_its_leading)
{
    lh h = lh_make("<div style=\"font:40px/130px sans-serif\">T<span id=s style=\"vertical-align:text-bottom\">M</span>"
                   "<span id=t>M</span></div>",
                   "");
    embi_rectf s, t;
    EMB_REQUIRE(rect_of(&h, "s", &s) && rect_of(&h, "t", &t));
    EMB_CHECK(near(t.y - s.y, (130.0f - s.h) / 2.0f));
    lh_free(&h);
}

/* baseline-block-with-overflow-001: an inline-block holding a block that clips its overflow takes its baseline from the
 * clipping block's bottom margin edge, not from the text inside it. */
EMB_TEST(clipping_block_child_gives_its_bottom_margin_edge_as_baseline)
{
    lh h = lh_make("<div style=\"font:10px sans-serif\"><div id=o style=\"display:inline-block;padding-bottom:20px\">"
                   "<div id=i style=\"width:30px;height:30px;overflow:hidden\">XX</div></div><span id=t>X</span></div>",
                   "");
    embi_rectf i, t;
    EMB_REQUIRE(rect_of(&h, "i", &i) && rect_of(&h, "t", &t));
    /* the text's baseline is the bottom of the clipping block (30px), so the text sits just above and below that line
     * (without the fix the baseline is the 50px bottom of the padding box) */
    EMB_CHECK(t.y + t.h < i.y + i.h + 10.0f);
    lh_free(&h);
}

/* margin-collapse-037: a min-height below the content height leaves the last child's bottom margin collapsing with the
 * parent's: the parent is 100px high and its 100px margin separates it from the next box. */
EMB_TEST(min_height_below_the_content_keeps_the_bottom_margin_collapsing)
{
    lh h = lh_make("<div id=p style=\"min-height:50px\"><div style=\"height:100px;margin-bottom:100px\"></div></div>"
                   "<div id=n style=\"height:10px\"></div>",
                   "");
    embi_rectf p, n;
    EMB_REQUIRE(rect_of(&h, "p", &p) && rect_of(&h, "n", &n));
    EMB_CHECK(near(p.h, 100.0f));
    EMB_CHECK(near(n.y, 200.0f));
    lh_free(&h);
}

/* A min-height in effect (above the content) keeps the margin inside: 150px of height, no margin below. */
EMB_TEST(min_height_in_effect_puts_the_bottom_margin_inside)
{
    lh h = lh_make("<div id=p style=\"min-height:150px\"><div style=\"height:100px;margin-bottom:100px\"></div></div>"
                   "<div id=n style=\"height:10px\"></div>",
                   "");
    embi_rectf p, n;
    EMB_REQUIRE(rect_of(&h, "p", &p) && rect_of(&h, "n", &n));
    EMB_CHECK(near(p.h, 200.0f));
    EMB_CHECK(near(n.y, 200.0f));
    lh_free(&h);
}

/* margin-collapse-038: a max-height does not stop the margin collapsing: the parent is 99px high and the 100px margin
 * is outside it. */
EMB_TEST(max_height_in_effect_keeps_the_bottom_margin_collapsing)
{
    lh h = lh_make("<div id=p style=\"max-height:99px\"><div style=\"height:300px;margin-bottom:100px\"></div></div>"
                   "<div id=n style=\"height:10px\"></div>",
                   "");
    embi_rectf p, n;
    EMB_REQUIRE(rect_of(&h, "p", &p) && rect_of(&h, "n", &n));
    EMB_CHECK(near(p.h, 99.0f));
    EMB_CHECK(near(n.y, 199.0f));
    lh_free(&h);
}

/* margin-auto-on-block-box: in an over-constrained ltr block an auto margin-left is zero, and margin-right gives way. */
EMB_TEST(auto_margin_left_does_not_go_negative_in_an_overconstrained_block)
{
    lh h = lh_make("<div style=\"width:100px\"><div id=a style=\"width:200px;height:5px;margin:auto\"></div></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.x, 0.0f));
    lh_free(&h);
}

/* floats-029: a float after inline text, in a block that also has a block child, sits at the top of the line it comes
 * in (beside the text), not below the line. */
EMB_TEST(float_after_inline_text_joins_the_line_before_a_block_child)
{
    lh h = lh_make("<div><span>Filler Text</span> <span id=f style=\"float:left;width:96px;height:96px\"></span>"
                   "<div style=\"clear:both\"></div></div>",
                   "");
    embi_rectf f;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_CHECK(near(f.y, 0.0f));
    lh_free(&h);
}

/* floats-rule3-outside-left-002 / -001: a left float wider than its containing block may stick out of it, but not into a
 * right float that is outside the block; 425px fits before the right float at 450px, 475px goes below it. */
EMB_TEST(float_wider_than_its_block_stops_at_a_float_outside_the_block)
{
    lh h = lh_make("<div style=\"float:left;width:500px;height:500px\"><div style=\"float:right;width:50px;height:300px\">"
                   "</div><div style=\"margin-right:100px\"><div id=a style=\"float:left;width:425px;height:10px\"></div>"
                   "<div id=b style=\"float:left;clear:left;width:475px;height:10px\"></div></div></div>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "b", &b));
    EMB_CHECK(near(a.y, 0.0f));
    EMB_CHECK(near(b.y, 300.0f)); /* 475 > 450: it cannot sit beside the right float */
    lh_free(&h);
}

/* floats-rule7-outside-left-001: with another left float to its start side, a float that is wider than its block may not
 * stick out at the right edge: it goes below that float. */
EMB_TEST(float_wider_than_its_block_goes_below_a_float_on_its_own_side)
{
    lh h = lh_make("<div style=\"float:left;width:500px;height:500px\"><div style=\"float:left;width:50px;height:300px\">"
                   "</div><div style=\"margin-left:100px\"><div id=a style=\"float:left;width:425px;height:10px\"></div>"
                   "</div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.y, 300.0f));
    lh_free(&h);
}

/* grid-aspect-ratio-038: a grid item with an aspect ratio and auto sizes in a fixed track stretches its width to the
 * 100px column and takes its height from the ratio (not the 50px row). */
EMB_TEST(grid_item_with_aspect_ratio_stretches_its_width_and_follows_the_ratio)
{
    lh h = lh_make("<div style=\"display:grid;grid-template:50px / 100px\"><div id=a style=\"aspect-ratio:1/1\"></div></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100.0f));
    EMB_CHECK(near(a.h, 100.0f));
    lh_free(&h);
}

/* grid-aspect-ratio-001: with a definite height the width follows the ratio instead of stretching. */
EMB_TEST(grid_item_with_aspect_ratio_and_definite_height_keeps_the_ratio_width)
{
    lh h = lh_make("<div style=\"display:grid;grid-template-columns:200px\"><div id=a style=\"height:100px;aspect-ratio:1/1\">"
                   "</div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100.0f));
    lh_free(&h);
}

/* grid-abspos-staticpos-align-self-last-baseline-002: an out-of-flow grid child with align-self: last baseline falls back
 * to end alignment in its static-position rectangle, the grid's content box (the grid is not its containing block). */
EMB_TEST(abspos_static_position_last_baseline_falls_back_to_end)
{
    lh h = lh_make("<div style=\"display:grid;width:22px;height:30px;grid:2px 20px 2px / 3px 14px 3px\">"
                   "<div id=a style=\"position:absolute;grid-area:2/2/3/3;width:8px;height:6px;align-self:last baseline\"></div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.y, 30.0f - 6.0f)); /* the bottom of the content box */
    lh_free(&h);
}

/* box.c wrap_runs: a float joins the inline run of a block, but a flex or grid container makes each child an item: text and
 * the float are two items. */
EMB_TEST(float_in_a_flex_container_is_its_own_item_not_part_of_the_text_run)
{
    lh h = lh_make("<div id=c style=\"display:flex;width:200px\">abc<div id=f style=\"float:left;width:30px;height:10px\">x</div></div>", "");
    embi_rectf c, f;
    EMB_REQUIRE(rect_of(&h, "c", &c) && rect_of(&h, "f", &f));
    EMB_CHECK(near(f.w, 30.0f));
    EMB_CHECK(f.x >= c.x + 10.0f); /* after the text item, not beside it in a line */
    lh_free(&h);
}

EMB_TEST(float_in_a_grid_container_is_its_own_item_not_part_of_the_text_run)
{
    lh h = lh_make("<div id=c style=\"display:grid;grid-template-columns:100px 100px;width:200px\">abc"
                   "<div id=f style=\"float:left;height:10px\">x</div></div>",
                   "");
    embi_rectf f;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_CHECK(near(f.w, 100.0f)); /* a grid item in the second column, stretched */
    lh_free(&h);
}

/* A clipping child gives the bottom margin edge as baseline only inside an inline-block; in a flex item or table cell
 * the first line of its text stays the baseline. */
EMB_TEST(clipping_child_keeps_its_text_baseline_in_a_table_cell)
{
    lh h = lh_make("<div style=\"font:10px sans-serif\"><span id=o style=\"display:inline-table\"><span style=\"display:table-row\">"
                   "<span id=k style=\"display:table-cell\"><div id=i style=\"height:50px;overflow:hidden\">XX</div></span></span></span>"
                   "<span id=t>X</span></div>",
                   "");
    embi_rectf i, t;
    EMB_REQUIRE(rect_of(&h, "i", &i) && rect_of(&h, "t", &t));
    /* the baseline is the text's, near the top of the 50px clipping block, not its bottom edge */
    EMB_CHECK(t.y + t.h < i.y + 25.0f);
    lh_free(&h);
}

EMB_TEST(clipping_child_keeps_its_text_baseline_in_a_flex_item)
{
    lh h = lh_make("<div style=\"font:10px sans-serif;display:flex;align-items:baseline\"><div><div id=i style=\"height:50px;overflow:hidden\">XX</div></div>"
                   "<span id=t>X</span></div>",
                   "");
    embi_rectf i, t;
    EMB_REQUIRE(rect_of(&h, "i", &i) && rect_of(&h, "t", &t));
    EMB_CHECK(near(t.y, i.y)); /* both first baselines are at the top line */
    lh_free(&h);
}

/* In an rtl block an over-constrained auto margin-right is zero and margin-left gives way. */
EMB_TEST(auto_margin_right_does_not_go_negative_in_an_overconstrained_rtl_block)
{
    lh h = lh_make("<div style=\"width:100px;direction:rtl\"><div id=a style=\"width:200px;height:5px;margin:auto\"></div></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.x, -100.0f)); /* right edges meet at 100 */
    lh_free(&h);
}

EMB_TEST_MAIN()
