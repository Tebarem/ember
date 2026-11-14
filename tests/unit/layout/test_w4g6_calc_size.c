/*
 * calc-size() in layout (CSS Values 5 section 10.1, W4g6): the calc-sum of the sizing properties sees the
 * basis size as `size`, a flex container's main size stays indefinite while its lines are built (CSS
 * Flexbox 1 section 9.2 step 3, reftests calc-size-flex-008 and -009), and the box-sizing of the size.
 * Expected values are worked out by hand.
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

/* calc-size-flex-008: a column wrap container with an auto basis height. Line breaking sees an
 * indefinite main size, so both 60px items share one line; the used height is the content height
 * (120px) and the calc-sum clamps it to 100px; the items then shrink to fit it. */
EMB_TEST(flex_column_wrap_height_is_indefinite_while_lines_are_built)
{
    lh h = lh_make("<div id=f style=\"display:flex;flex-direction:column;flex-wrap:wrap;width:100px;"
                   "height:calc-size(auto, min(size, 100px))\"><div id=a style=\"width:100px;height:60px\"></div>"
                   "<div id=b style=\"width:100px;height:60px\"></div></div>",
                   "");
    embi_rectf f, a, b;
    EMB_REQUIRE(rect_of(&h, "f", &f) && rect_of(&h, "a", &a) && rect_of(&h, "b", &b));
    EMB_CHECK(near(f.h, 100.0f));
    EMB_CHECK(near(b.x, a.x));  /* one line: the second item is below the first */
    /* the final main size (100px) is definite when the flexible lengths resolve: both items shrink to 50px */
    EMB_CHECK(near(a.h, 50.0f));
    EMB_CHECK(near(b.h, 50.0f));
    EMB_CHECK(near(b.y - a.y, 50.0f));
    lh_free(&h);
}

/* calc-size-flex-009: the automatic minimum of a flex item (50px, its min-content) is the size the
 * calc-sum doubles: min-width is 100px, so the item is 100px wide in a 100px row. */
EMB_TEST(flex_item_auto_min_width_is_the_size_of_the_calc_sum)
{
    lh h = lh_make("<div id=f style=\"display:flex;width:100px;height:100px\">"
                   "<div id=a style=\"min-width:calc-size(auto, size * 2);width:80px\">"
                   "<div style=\"width:50px\"></div></div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100.0f));
    lh_free(&h);
}

/* A definite basis: calc-size(100px, size + 10px) is 110px. */
EMB_TEST(width_with_a_length_basis_applies_the_calc_sum)
{
    lh h = lh_make("<div id=a style=\"width:calc-size(100px, size + 10px)\"></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 110.0f));
    lh_free(&h);
}

/* An auto basis in a block: the auto width (the 200px container) is halved. */
EMB_TEST(auto_width_basis_is_the_used_width_before_the_calc_sum)
{
    lh h = lh_make("<div style=\"width:200px\"><div id=a style=\"width:calc-size(auto, size / 2)\"></div></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100.0f));
    lh_free(&h);
}

/* box-sizing: border-box makes the basis and the result border-box sizes. The auto border-box width
 * is 200px; halved it is 100px, which leaves 80px of content between 10px paddings. */
EMB_TEST(border_box_size_is_the_basis_of_the_calc_sum)
{
    lh h = lh_make("<div style=\"width:200px\"><div id=a style=\"box-sizing:border-box;padding:0 10px;"
                   "width:calc-size(auto, size / 2)\"></div></div>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100.0f));
    lh_free(&h);
}

/* min-height on an empty block: the basis is 0, so the calc-sum gives 50px. */
EMB_TEST(min_height_calc_size_applies_to_an_empty_block)
{
    lh h = lh_make("<div id=a style=\"min-height:calc-size(auto, max(size, 50px))\"></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 50.0f));
    lh_free(&h);
}

/* A height with an auto basis follows the content, then the calc-sum: 40px of content, doubled. */
EMB_TEST(auto_height_basis_applies_the_calc_sum_to_the_content_height)
{
    lh h = lh_make("<div id=a style=\"height:calc-size(auto, size * 2)\"><div style=\"height:40px\"></div></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 80.0f));
    lh_free(&h);
}

/* The max-width of a block: 300px cap, from a length basis. */
EMB_TEST(max_width_calc_size_caps_the_width)
{
    lh h = lh_make("<div id=a style=\"width:500px;max-width:calc-size(300px, size)\"></div>", "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 300.0f));
    lh_free(&h);
}
/* ---- W4g6b ---- */

/* One box by id: its border-box width (w) or height (!w) against the expected value. */
static bool size_is(const char *html, bool w, float want)
{
    lh h = lh_make(html, "");
    embi_rectf a;
    bool ok = rect_of(&h, "a", &a) && near(w ? a.w : a.h, want);
    lh_free(&h);
    return ok;
}

/* A row flex container takes the calc-size() minimum height like a block does. */
EMB_TEST(row_flex_container_min_height_calc_size)
{
    EMB_CHECK(size_is("<div id=a style=\"display:flex;min-height:calc-size(auto, max(size, 24px))\">"
                      "<div style=\"height:10px\"></div></div>",
                      false, 24.0f));
}

/* The UA rules of select (html.css): no min-block-size (the 24px minimum belongs to appearance: base-select). A
 * drop-down select is a replaced box of 19px, as an inline box, as a block and as a flex item. */
EMB_TEST(select_ua_min_block_size_leaves_the_replaced_box_at_19px)
{
    EMB_CHECK(size_is("<div><select id=a style=\"margin:0\"><option>Hi</option></select></div>", false, 19.0f));
    EMB_CHECK(size_is("<div><select id=a style=\"margin:0;display:block\"></select></div>", false, 19.0f));
    EMB_CHECK(size_is("<div style=\"display:flex\"><select id=a style=\"margin:0\"></select></div>", false, 19.0f));
}

/* select-element-zero-height-001: in a zero-height flex column the select keeps its minimum, and matches the
 * select in a zero-height block. */
EMB_TEST(select_in_zero_height_column_matches_the_block_case)
{
    EMB_CHECK(size_is("<div style=\"display:flex;flex-direction:column;height:0\"><select id=a style=\"margin:0\">"
                      "<option>Hi</option></select></div>",
                      false, 19.0f));
    EMB_CHECK(size_is("<div style=\"height:0\"><select id=a style=\"margin:0;width:100%\"><option>Hi</option></select>"
                      "</div>",
                      false, 19.0f));
}

/* Replaced boxes: a length basis, the auto basis, min and max. */
EMB_TEST(replaced_box_width_calc_size)
{
    EMB_CHECK(size_is("<div><img id=a width=40 height=20 style=\"width:calc-size(30px, size * 2)\"></div>", true, 60.0f));
    EMB_CHECK(size_is("<div><img id=a width=40 height=20 style=\"width:calc-size(auto, size * 2)\"></div>", true, 80.0f));
    EMB_CHECK(size_is("<div><img id=a width=40 height=20 style=\"min-width:calc-size(auto, max(size, 100px))\"></div>",
                      true, 100.0f));
    EMB_CHECK(size_is("<div><img id=a width=40 height=20 style=\"max-width:calc-size(30px, size / 2)\"></div>", true,
                      15.0f));
    EMB_CHECK(size_is("<div><img id=a width=40 height=20 style=\"max-width:calc-size(auto, size / 2)\"></div>", true,
                      20.0f));
}

/* Gap 1: calc-size(auto, ...) on max-width and max-height: the calc-sum sees the used size. */
EMB_TEST(max_width_with_an_auto_basis)
{
    EMB_CHECK(size_is("<div style=\"width:200px\"><div id=a style=\"max-width:calc-size(auto, size / 2)\"></div></div>",
                      true, 100.0f));
    EMB_CHECK(size_is("<div style=\"width:200px\"><div id=a style=\"width:150px;max-width:calc-size(auto, size - 50px)\">"
                      "</div></div>",
                      true, 100.0f));
}

EMB_TEST(max_height_with_an_auto_basis)
{
    EMB_CHECK(size_is("<div id=a style=\"max-height:calc-size(auto, size / 2)\"><div style=\"height:80px\"></div></div>",
                      false, 40.0f));
    EMB_CHECK(size_is("<div id=a style=\"max-height:calc-size(auto, size + 50px)\"><div style=\"height:80px\"></div>"
                      "</div>",
                      false, 80.0f));
}

EMB_TEST(flex_item_max_main_with_an_auto_basis)
{
    EMB_CHECK(size_is("<div style=\"display:flex;width:400px\"><div id=a style=\"flex:none;max-width:calc-size(auto, "
                      "size / 2)\"><div style=\"width:80px;height:10px\"></div></div></div>",
                      true, 40.0f));
}

/* Gap 2: a keyword basis on a flex item's main size. */
EMB_TEST(flex_item_width_with_a_content_keyword_basis)
{
    EMB_CHECK(size_is("<div style=\"display:flex;width:400px\"><div id=a style=\"width:calc-size(min-content, size + "
                      "10px)\"><div style=\"width:50px;height:10px\"></div></div></div>",
                      true, 60.0f));
    EMB_CHECK(size_is("<div style=\"display:flex;width:400px\"><div id=a style=\"width:calc-size(max-content, size * "
                      "2)\"><div style=\"width:50px;height:10px\"></div></div></div>",
                      true, 100.0f));
    /* fit-content in a 400px row: min(max-content, max(min-content, 400px)) = 50px, plus 5px */
    EMB_CHECK(size_is("<div style=\"display:flex;width:400px\"><div id=a style=\"width:calc-size(fit-content, size + "
                      "5px)\"><div style=\"width:50px;height:10px\"></div></div></div>",
                      true, 55.0f));
    /* border-box: the basis includes the padding */
    EMB_CHECK(size_is("<div style=\"display:flex;width:400px\"><div id=a style=\"box-sizing:border-box;padding:0 5px;"
                      "width:calc-size(max-content, size + 10px)\"><div style=\"width:50px;height:10px\"></div></div>"
                      "</div>",
                      true, 70.0f));
}

/* Gap 3: a percentage in the calc-sum of a column flex container's height. */
EMB_TEST(column_flex_height_calc_sum_with_a_percentage)
{
    EMB_CHECK(size_is("<div style=\"height:200px\"><div id=a style=\"display:flex;flex-direction:column;"
                      "height:calc-size(auto, size + 10%)\"><div style=\"height:30px\"></div></div></div>",
                      false, 50.0f));
    /* an indefinite containing block: the percentage does not resolve, the height is the content height */
    EMB_CHECK(size_is("<div><div id=a style=\"display:flex;flex-direction:column;height:calc-size(auto, size + 10%)\">"
                      "<div style=\"height:30px\"></div></div></div>",
                      false, 30.0f));
    /* the items flex to the final size */
    EMB_CHECK(size_is("<div style=\"height:200px\"><div style=\"display:flex;flex-direction:column;"
                      "height:calc-size(auto, size + 10%)\"><div id=a style=\"height:30px;flex:1 1 auto\"></div></div></div>",
                      false, 50.0f));
}

/* A column container's height keeps the calc-sum when its percentage resolves against a definite block, in a row flex
 * container too (the container is not stretched: its height is not auto), and when it is clamped. */
EMB_TEST(column_flex_height_calc_sum_clamps_against_a_percentage)
{
    EMB_CHECK(size_is("<div style=\"height:200px\"><div id=a style=\"display:flex;flex-direction:column;"
                      "height:calc-size(auto, min(size, 50%))\"><div style=\"height:300px\"></div></div></div>",
                      false, 100.0f));
    EMB_CHECK(size_is("<div style=\"height:200px;display:flex\"><div id=a style=\"display:flex;flex-direction:column;"
                      "height:calc-size(auto, min(size, 50%))\"><div style=\"height:300px\"></div></div></div>",
                      false, 100.0f));
    EMB_CHECK(size_is("<div style=\"height:200px\"><div id=a style=\"display:flex;flex-direction:column;"
                      "flex-wrap:wrap;height:calc-size(auto, min(size, 50%));padding:10px;box-sizing:border-box\">"
                      "<div style=\"height:300px\"></div></div></div>",
                      false, 100.0f));
}

/* Gap 4: grid items, table cells and rows. */
EMB_TEST(grid_item_stretch_applies_the_calc_sum_to_the_stretched_size)
{
    EMB_CHECK(size_is("<div style=\"display:grid;width:400px\"><div id=a style=\"width:calc-size(auto, size / 2)\"></div>"
                      "</div>",
                      true, 200.0f));
    EMB_CHECK(size_is("<div style=\"display:grid;width:400px;grid-template-rows:100px\"><div id=a style=\"height:"
                      "calc-size(auto, size / 2)\"></div></div>",
                      false, 50.0f));
}

EMB_TEST(grid_item_min_and_max_with_a_length_basis)
{
    EMB_CHECK(size_is("<div style=\"display:grid;width:400px\"><div id=a style=\"max-width:calc-size(100px, size * 2)\">"
                      "</div></div>",
                      true, 200.0f));
    EMB_CHECK(size_is("<div style=\"display:grid;width:400px;grid-template-columns:50px\"><div id=a style=\"min-width:"
                      "calc-size(100px, size * 2)\"></div></div>",
                      true, 200.0f));
}

EMB_TEST(grid_container_auto_height_basis)
{
    EMB_CHECK(size_is("<div id=a style=\"display:grid;height:calc-size(auto, size * 2)\"><div style=\"height:30px\"></div>"
                      "</div>",
                      false, 60.0f));
}

EMB_TEST(table_cell_width_and_height_with_a_length_basis)
{
    EMB_CHECK(size_is("<table style=\"border-spacing:0\"><tr><td id=a style=\"padding:0;width:calc-size(100px, size * 2)\">"
                      "</td></tr></table>",
                      true, 200.0f));
    EMB_CHECK(size_is("<table style=\"border-spacing:0\"><tr><td id=a style=\"padding:0;height:calc-size(100px, size * 2)\">"
                      "</td></tr></table>",
                      false, 200.0f));
}

EMB_TEST(table_row_height_with_a_length_basis)
{
    EMB_CHECK(size_is("<table style=\"border-spacing:0\"><tr id=a style=\"height:calc-size(50px, size * 2)\"><td "
                      "style=\"padding:0\"></td></tr></table>",
                      false, 100.0f));
}

EMB_TEST(table_box_calc_size)
{
    EMB_CHECK(size_is("<table id=a style=\"border-spacing:0;height:calc-size(100px, size * 2)\"><tr><td style=\"padding:0;"
                      "height:30px\"></td></tr></table>",
                      false, 200.0f));
    EMB_CHECK(size_is("<table id=a style=\"border-spacing:0;height:calc-size(auto, size * 2)\"><tr><td style=\"padding:0;"
                      "height:30px\"></td></tr></table>",
                      false, 60.0f));
    EMB_CHECK(size_is("<table id=a style=\"border-spacing:0;width:calc-size(100px, size * 2)\"><tr><td style=\"padding:0\">"
                      "</td></tr></table>",
                      true, 200.0f));
}

EMB_TEST_MAIN()
