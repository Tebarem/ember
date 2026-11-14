/*
 * Text decorations and list markers in the display list (W4c section 3): the decorations of block and inline
 * ancestors, the decorating element's color and style, the underline offset and position, and the disc, circle,
 * square and disclosure markers drawn as shapes.
 */
#include "paint_harness.h"

#include <math.h>

/* The index of the n-th item of kind k (0-based), or -1. */
static int nth_of(const embi_dl *d, uint8_t k, uint32_t n)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == k) {
            if (n == 0) {
                return (int)i;
            }
            n--;
        }
    }
    return -1;
}

EMB_TEST(decoration_of_a_block_ancestor_is_drawn_over_an_inline_child)
{
    /* the span's own style sets no decoration: the line comes from the block around it */
    ph h = ph_make("<div style=\"text-decoration:underline\"><span style=\"color:#f00\">text</span></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_DECORATION), 1);
    int d = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(h.dl.items.data[d].sub == EMBI_DECO_SOLID);
    ph_free(&h);
}

EMB_TEST(decoration_of_a_block_ancestor_through_nested_inline_boxes)
{
    ph h = ph_make("<div style=\"text-decoration:underline\"><span><span><b>t</b></span></span></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_DECORATION), 1);
    ph_free(&h);
}

EMB_TEST(decoration_color_comes_from_the_decorating_element)
{
    /* the decorating div is blue (currentcolor is its own color); the span inside is red and paints red text only */
    ph h = ph_make("<div style=\"text-decoration:underline;color:#00f\"><span style=\"color:#f00\">text</span></div>", "");
    int d = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(h.dl.items.data[d].colors[0] == PH_BLUE);
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(h.dl.items.data[t].colors[0] == PH_RED);
    ph_free(&h);
}

EMB_TEST(text_decoration_color_property_wins_over_the_text_color)
{
    ph h = ph_make("<div style=\"text-decoration:underline;text-decoration-color:#0f0;color:#000\">t</div>", "");
    int d = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(h.dl.items.data[d].colors[0] == PH_GREEN);
    ph_free(&h);
}

EMB_TEST(decoration_style_comes_from_the_decorating_element)
{
    ph h = ph_make("<div style=\"text-decoration:underline double;color:#000\"><span>t</span></div>", "");
    int d = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(h.dl.items.data[d].sub == EMBI_DECO_DOUBLE);
    ph_free(&h);
}

EMB_TEST(decoration_thickness_length_is_used)
{
    ph h = ph_make("<div style=\"text-decoration:underline;text-decoration-thickness:3px\">t</div>", "");
    int d = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    EMB_REQUIRE(d >= 0);
    EMB_CHECK(fabsf(h.dl.items.data[d].rect[3] - 3.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(underline_offset_moves_the_line_down_by_the_offset)
{
    /* two blocks, each with its own baseline: the distance from the baseline to the line grows by the offset */
    ph h = ph_make("<div style=\"text-decoration:underline;font-family:Ahem\">a</div>"
                   "<div style=\"text-decoration:underline;text-underline-offset:5px;font-family:Ahem\">b</div>", "");
    EMB_REQUIRE(ph_count(&h.dl, EMBI_DI_DECORATION) == 2);
    int t0 = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    int d0 = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    int t1 = ph_find(&h.dl, (uint32_t)t0 + 1, EMBI_DI_TEXT);
    int d1 = ph_find(&h.dl, (uint32_t)d0 + 1, EMBI_DI_DECORATION);
    EMB_REQUIRE(t0 >= 0 && t1 >= 0 && d0 >= 0 && d1 >= 0);
    (void)d0; (void)t0;
    float off1 = h.dl.items.data[d1].rect[1] - h.dl.items.data[t1].f[1];
    /* a length offset is measured from the alphabetic baseline (css-text-decor-4), not added to the font's position */
    EMB_CHECK(fabsf(off1 - 5.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(underline_position_under_is_not_above_the_font_position)
{
    ph h = ph_make("<div style=\"text-decoration:underline;font-family:Ahem\">a</div>"
                   "<div style=\"text-decoration:underline;text-underline-position:under;font-family:Ahem\">b</div>", "");
    EMB_REQUIRE(ph_count(&h.dl, EMBI_DI_DECORATION) == 2);
    int t0 = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    int d0 = ph_find(&h.dl, 0, EMBI_DI_DECORATION);
    int t1 = ph_find(&h.dl, (uint32_t)t0 + 1, EMBI_DI_TEXT);
    int d1 = ph_find(&h.dl, (uint32_t)d0 + 1, EMBI_DI_DECORATION);
    EMB_REQUIRE(t0 >= 0 && t1 >= 0 && d0 >= 0 && d1 >= 0);
    float auto_off = h.dl.items.data[d0].rect[1] - h.dl.items.data[t0].f[1];
    float under_off = h.dl.items.data[d1].rect[1] - h.dl.items.data[t1].f[1];
    EMB_CHECK(under_off >= auto_off);
    ph_free(&h);
}

EMB_TEST(overline_is_at_the_top_and_line_through_above_the_baseline)
{
    ph h = ph_make("<div id=t style=\"text-decoration:overline line-through;font-family:Ahem\">t</div>", "");
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(t >= 0);
    int o = nth_of(&h.dl, EMBI_DI_DECORATION, 0);
    int s = nth_of(&h.dl, EMBI_DI_DECORATION, 1);
    EMB_REQUIRE(o >= 0 && s >= 0);
    EMB_CHECK(h.dl.items.data[o].rect[1] == h.dl.items.data[t].rect[1]);
    EMB_CHECK(h.dl.items.data[s].rect[1] < h.dl.items.data[t].f[1]);
    ph_free(&h);
}

/* ------------------------------------------------------------------ list markers as shapes */

EMB_TEST(disc_marker_is_a_round_fill_not_text)
{
    /* the disc of a 16 px marker is 5 px (a third of the ascent, rounded as Chromium): one fill, and only the item
     * text is text */
    ph h = ph_make("<ul><li>one</li></ul>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    int f = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(f >= 0);
    const embi_ditem *it = &h.dl.items.data[f];
    EMB_CHECK(fabsf(it->rect[2] - 5.0f) < 0.001f && fabsf(it->rect[3] - 5.0f) < 0.001f);
    EMB_CHECK(fabsf(it->radii[0] - 2.5f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(disc_marker_size_grows_with_the_font)
{
    /* 32 px: the ascent is 29 px, so the disc is 10 px */
    ph h = ph_make("<ul style=\"font-size:32px\"><li>one</li></ul>", "");
    int f = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(f >= 0);
    EMB_CHECK(fabsf(h.dl.items.data[f].rect[2] - 10.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(circle_marker_is_a_ring)
{
    ph h = ph_make("<ul style=\"list-style-type:circle\"><li>one</li></ul>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    int b = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(b >= 0);
    const embi_ditem *it = &h.dl.items.data[b];
    EMB_CHECK(it->widths[0] == 1.0f && it->widths[3] == 1.0f);
    EMB_CHECK(fabsf(it->radii[0] - 2.5f) < 0.001f);
    EMB_CHECK(fabsf(it->rect[2] - 5.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(square_marker_is_a_square_fill)
{
    ph h = ph_make("<ul style=\"list-style-type:square\"><li>one</li></ul>", "");
    int f = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(f >= 0);
    const embi_ditem *it = &h.dl.items.data[f];
    EMB_CHECK(it->radii[0] == 0.0f && it->radii[2] == 0.0f);
    EMB_CHECK(fabsf(it->rect[2] - 5.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(disclosure_markers_are_triangles)
{
    ph closed = ph_make("<ul style=\"list-style-type:disclosure-closed\"><li>one</li></ul>", "");
    int c = ph_find(&closed.dl, 0, EMBI_DI_TRIANGLE);
    EMB_REQUIRE(c >= 0);
    EMB_CHECK(closed.dl.items.data[c].sub == EMBI_TRI_RIGHT);
    ph_free(&closed);

    ph open = ph_make("<ul style=\"list-style-type:disclosure-open\"><li>one</li></ul>", "");
    int o = ph_find(&open.dl, 0, EMBI_DI_TRIANGLE);
    EMB_REQUIRE(o >= 0);
    EMB_CHECK(open.dl.items.data[o].sub == EMBI_TRI_DOWN);
    ph_free(&open);

    ph rtl = ph_make("<ul dir=rtl style=\"list-style-type:disclosure-closed;direction:rtl\"><li>one</li></ul>", "");
    int r = ph_find(&rtl.dl, 0, EMBI_DI_TRIANGLE);
    EMB_REQUIRE(r >= 0);
    EMB_CHECK(rtl.dl.items.data[r].sub == EMBI_TRI_LEFT);
    ph_free(&rtl);
}

EMB_TEST(decimal_markers_stay_text)
{
    ph h = ph_make("<ol><li>a</li><li>b</li></ol>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TRIANGLE), 0);
    /* two items and two markers, all text */
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) >= 4);
    ph_free(&h);
}

EMB_TEST(marker_shape_takes_the_color_of_the_item)
{
    ph h = ph_make("<ul style=\"color:#00f\"><li>one</li></ul>", "");
    int f = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(f >= 0);
    EMB_CHECK(h.dl.items.data[f].colors[0] == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(text_shadow_is_drawn_below_the_text_in_the_shadow_color)
{
    ph h = ph_make("<div style=\"color:#000;text-shadow:3px 4px #00f\">t</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 2);
    int a = nth_of(&h.dl, EMBI_DI_TEXT, 0), b = nth_of(&h.dl, EMBI_DI_TEXT, 1);
    EMB_REQUIRE(a >= 0 && b >= 0);
    /* the shadow comes first, moved by the offset */
    EMB_CHECK(h.dl.items.data[a].colors[0] == PH_BLUE);
    EMB_CHECK(fabsf(h.dl.items.data[a].f[0] - h.dl.items.data[b].f[0] - 3.0f) < 0.01f);
    EMB_CHECK(fabsf(h.dl.items.data[a].f[1] - h.dl.items.data[b].f[1] - 4.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(first_shadow_is_on_top_of_the_later_ones)
{
    ph h = ph_make("<div style=\"color:#000;text-shadow:1px 0 #f00,2px 0 #0f0\">t</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 3);
    int a = nth_of(&h.dl, EMBI_DI_TEXT, 0), b = nth_of(&h.dl, EMBI_DI_TEXT, 1);
    EMB_REQUIRE(a >= 0 && b >= 0);
    EMB_CHECK(h.dl.items.data[a].colors[0] == PH_GREEN);
    EMB_CHECK(h.dl.items.data[b].colors[0] == PH_RED);
    ph_free(&h);
}

EMB_TEST(text_shadow_decorates_the_shadow_too)
{
    ph h = ph_make("<div style=\"text-decoration:underline;text-shadow:5px 0 #00f\">t</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_DECORATION), 2);
    int a = nth_of(&h.dl, EMBI_DI_DECORATION, 0);
    EMB_REQUIRE(a >= 0);
    EMB_CHECK(h.dl.items.data[a].colors[0] == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(first_line_color_applies_to_the_first_line_only)
{
    ph h = ph_make("<p style=\"width:20px;color:#000\">aa bb cc</p>", "p::first-line{color:#0f0}");
    int first = nth_of(&h.dl, EMBI_DI_TEXT, 0);
    int last = -1;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_TEXT) {
            last = (int)i;
        }
    }
    EMB_REQUIRE(first >= 0 && last > first);
    EMB_CHECK(h.dl.items.data[first].colors[0] == PH_GREEN);
    EMB_CHECK(h.dl.items.data[last].colors[0] == PH_BLACK);
    ph_free(&h);
}

EMB_TEST_MAIN()
