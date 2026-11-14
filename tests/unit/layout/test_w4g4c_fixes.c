/*
 * Regression tests for the layout fixes of the fourth round of the layout reftest burn-down (W4g4c): the source of an img
 * from srcset density candidates, a fieldset with a table-internal display, the static position of a block-level abspos
 * box inside inline content and of an inline-level one in text against the paragraph direction, the containing block of
 * an abspos box inside a positioned inline split around a block, the collapsed border halves of an rtl table, flex struts,
 * the height of a wrapping column, percentage heights in intrinsic sizes, the min-content size of a flex container,
 * captions of a column table item and the ratio height of a row flex container.
 * Expected values are worked out by hand from the specs.
 */
#include "layout_harness.h"

#include <math.h>
#include <string.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

static bool src_is(const char *src, const char *srcset, const char *want, float want_density)
{
    float d = 0.0f;
    embi_str got = embi_lay_img_source((embi_str){src, (uint32_t)strlen(src)}, (embi_str){srcset, (uint32_t)strlen(srcset)},
                                       &d);
    return got.len == strlen(want) && (got.len == 0 || memcmp(got.ptr, want, got.len) == 0) && near(d, want_density);
}

EMB_TEST(img_source_prefers_the_1x_candidate)
{
    EMB_CHECK(src_is("a.png", "", "a.png", 1.0f));
    /* src is the 1x candidate when srcset has none; 1x beats every other density at a device pixel ratio of 1. */
    EMB_CHECK(src_is("a.png", "b.png 2x, c.png 0.5x", "a.png", 1.0f));
    /* A 1x candidate in srcset replaces src. */
    EMB_CHECK(src_is("a.png", "b.png 1x", "b.png", 1.0f));
    /* Without src: the lowest density at or above 1, else the highest below it. */
    EMB_CHECK(src_is("", "b.png 3x, c.png 2x", "c.png", 2.0f));
    EMB_CHECK(src_is("", "b.png 0.25x, c.png 0.5x", "c.png", 0.5f));
    /* A candidate without a descriptor is 1x; trailing commas end the URL. */
    EMB_CHECK(src_is("", "b.png,", "b.png", 1.0f));
    EMB_CHECK(src_is("", "  b.png  ", "b.png", 1.0f));
}

EMB_TEST(img_source_skips_width_and_invalid_descriptors)
{
    /* A width descriptor needs sizes (not supported): its candidate is skipped and src is not added (HTML 4.8.4.3.10). */
    EMB_CHECK(src_is("a.png", "b.png 100w", "a.png", 1.0f));
    EMB_CHECK(src_is("", "b.png 2q, c.png 2x", "c.png", 2.0f));
    EMB_CHECK(src_is("", "b.png 1x 2x, c.png 3x", "c.png", 3.0f));
}

EMB_TEST(fieldset_with_a_table_internal_display_is_a_block)
{
    /* HTML 15.3.13: a fieldset with display: table-row (or another table-internal value) behaves as flow-root, so it
     * fills the width of its container instead of getting an anonymous table around it. */
    lh h = lh_make("<body><div style='width:300px'><fieldset id=f style='display:table-row; margin:0; padding:0;"
                   " border:0'>x</fieldset></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "f", &r));
    EMB_CHECK(near(r.w, 300.0f));
    lh_free(&h);
}

EMB_TEST(block_level_abspos_in_inline_content_starts_a_line_of_its_own)
{
    /* CSS 2.1 10.3.7 and 10.6.4: the hypothetical box of a block-level abspos box after text in a line is a block, below
     * that line, at the content box's start edge (Chromium ignores the text before it). An inline-level one stays on the
     * line, after the text. */
    lh h = lh_make("<body><div style='width:300px; font: 20px/20px monospace'>xx<div id=a style='position:absolute;"
                   " width:10px; height:10px'></div><span id=b style='position:absolute; width:10px; height:10px'></span>"
                   "</div></body>",
                   "body { margin: 0 }");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 0.0f));
    EMB_CHECK(near(a.y, 20.0f));
    EMB_CHECK(b.x > 0.0f);
    EMB_CHECK(near(b.y, 0.0f));
    lh_free(&h);
}

EMB_TEST(abspos_in_a_positioned_inline_split_by_a_block_starts_at_its_first_piece)
{
    /* The inline containing block of a positioned span that starts with a block is the span's first and last pieces
     * (CSS 2.1 10.1 item 4): the empty piece on the line before the block starts it, so top: 0 is on that line. */
    lh h = lh_make("<body><div style='width:300px; font: 20px/20px monospace'>B <span style='position:relative'>"
                   "<div style='height:30px'></div>AA<span id=a style='position:absolute; top:0; left:0; width:10px;"
                   " height:10px'></span></span></div></body>",
                   "body { margin: 0 }");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(a.y < 10.0f); /* the first line's piece (its content area top), not the AA line at y = 50 */
    lh_free(&h);
}

EMB_TEST(rtl_collapsed_table_mirrors_the_halves_of_its_borders)
{
    /* An rtl table with collapsed borders is the mirror image of the ltr one (WPT row-margin-border-padding): each cell
     * takes the floor half of its logical start edge (the physical right one) and the rest of its end edge. With 3px
     * borders the ltr contents are at x 3 and 16, so the rtl ones are at 16 (the first cell, on the right) and 3. */
    lh h = lh_make("<body><table style='direction:rtl; border-collapse:collapse; border:3px solid'><tr>"
                   "<td style='border:3px solid; padding:0'><div id=a style='width:10px; height:10px'></div></td>"
                   "<td style='border:3px solid; padding:0'><div id=b style='width:10px; height:10px'></div></td>"
                   "</tr></table></body>",
                   "body { margin: 0 }");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 16.0f));
    EMB_CHECK(near(b.x, 3.0f));
    lh_free(&h);
}

EMB_TEST(collapsed_flex_item_is_a_strut_with_its_first_line_cross_size)
{
    /* CSS Flexbox 1 section 9.4 step 10: the collapsed item (25px tall) shares its first line with the 15px item, so its
     * strut is 25px; the 30px item wraps to a second line that starts below the strut. */
    lh h = lh_make("<body><div style='display:flex; flex-wrap:wrap; width:30px; align-content:flex-start;"
                   " align-items:flex-start'><div style='visibility:collapse; width:15px; height:25px'></div>"
                   "<div style='width:15px; height:15px'></div><div id=f style='width:30px; height:20px'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "f", &r));
    EMB_CHECK(near(r.y, 25.0f));
    lh_free(&h);
}

EMB_TEST(wrapping_column_with_max_height_is_as_tall_as_its_longest_line)
{
    lh h = lh_make("<body><div id=c style='display:flex; flex-flow:column wrap; width:200px; max-height:100px'>"
                   "<div style='width:50px; height:40px'></div><div style='width:50px; height:80px'></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.h, 80.0f));
    lh_free(&h);
}

EMB_TEST(percentage_heights_resolve_in_intrinsic_sizes)
{
    /* CSS Sizing 3 section 5.2.1: the definite height of the min-content box is the basis of its children's percentage
     * heights, so the ratio box inside is 100px wide and so is the min-content box. */
    lh h = lh_make("<body><div id=o style='width:min-content; height:100px'><div style='height:100%'>"
                   "<div style='height:100%; aspect-ratio:1/1'></div></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "o", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h);
}

EMB_TEST(inflexible_content_sized_flex_item_adds_its_min_content)
{
    /* Two 10px inline-blocks (min-content 10, max-content 20) and a 10px item with an 80px gap: the min-content width of
     * the container is 10 + 80 + 10 (Chromium; WPT css-flexbox gap-015). */
    lh h = lh_make("<body><div id=c style='display:flex; column-gap:80px; width:min-content; height:10px'>"
                   "<div style='flex:0 0 auto; line-height:0'><div style='display:inline-block; width:10px'></div>"
                   "<div style='display:inline-block; width:10px'></div></div><div style='flex:0 0 auto; width:10px'></div>"
                   "</div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.w, 100.0f));
    lh_free(&h);
}

EMB_TEST(column_table_item_adds_its_captions_to_its_height)
{
    lh h = lh_make("<body><div style='display:flex; flex-direction:column; width:100px'><table id=t style='border-spacing:0;"
                   " height:90px'><caption style='height:10px'></caption><tr><td></td></tr></table></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(near(r.h, 100.0f));
    lh_free(&h);
}

EMB_TEST(row_flex_container_with_a_ratio_stretches_its_items_to_the_ratio_height)
{
    lh h = lh_make("<body><div style='display:flex; width:400px; aspect-ratio:2/1'><div id=i style='aspect-ratio:1/1'>"
                   "</div></div></body>",
                   "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "i", &r));
    EMB_CHECK(near(r.h, 200.0f));
    EMB_CHECK(near(r.w, 200.0f));
    lh_free(&h);
}

EMB_TEST_MAIN()
