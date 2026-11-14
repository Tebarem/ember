/*
 * Painting fixes of the CSS2 reftest burn-down (W4g1), on the display list: the root's background on the canvas (the
 * positioning area is the root element's box, the painting area the viewport, an absolutely positioned body propagates,
 * display: none on the root still paints the canvas), anonymous boxes that paint nothing (so they do not cover floats),
 * pixel snapping of box edges, column and column-group backgrounds (also right to left), and the corner of collapsed
 * borders.
 */
#include "paint_harness.h"

#include "layout/box.h"
#include "layout/fragment.h"

#include <math.h>
#include <stdio.h>

static bool test_image_size(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    (void)user;
    if (url.len == 6 && memcmp(url.ptr, "ok.png", 6) == 0) {
        *w = 10.0f;
        *h = 10.0f;
        *has_ratio = true;
        return true;
    }
    return false;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* The number of FILL items of one color. */
static uint32_t fills_of(const embi_dl *d, uint32_t rgba)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        n += d->items.data[i].kind == EMBI_DI_FILL && d->items.data[i].colors[0] == rgba ? 1u : 0u;
    }
    return n;
}

EMB_TEST(root_background_positions_from_the_root_box_and_paints_the_whole_canvas)
{
    /* html has a 50 px margin and a repeat-x image: the first tile starts at the html box (50, 50) while the tiles cover
     * the canvas (the viewport) from edge to edge (CSS Backgrounds 3 section 2.11.2). */
    embi_paint_cfg cfg = {nullptr, test_image_size, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    ph h = ph_make_cfg("<html style=\"margin:50px;height:100px;background:url(ok.png) repeat-x\"><body></body></html>", "", &cfg);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(it->rect[0] <= 0.0f && it->rect[0] + it->rect[2] >= 800.0f); /* the viewport's width */
    EMB_CHECK(near(it->rect[1], 50.0f) && near(it->rect[3], 10.0f));       /* the row of the root box's top edge */
    EMB_CHECK(near(fmodf(it->f[0] - 50.0f, 10.0f), 0.0f) || near(fmodf(it->f[0] - 50.0f, 10.0f), 10.0f));
    ph_free(&h);
}

EMB_TEST(body_background_propagates_even_when_the_body_is_absolutely_positioned)
{
    /* The body is out of the root's flow (a child of the initial containing block, not of html's fragment), but its
     * background still goes to the canvas: a viewport-sized fill, and the body's own box paints no second fill. */
    ph h = ph_make("<body style=\"position:absolute;left:100px;top:100px;width:100px;height:100px;background:#00f\">"
                   "</body>",
                   "");
    int i = ph_fill_of(&h.dl, PH_BLUE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(near(h.dl.items.data[i].rect[2], 800.0f) && near(h.dl.items.data[i].rect[3], 600.0f));
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_BLUE), 1);
    ph_free(&h);
}

EMB_TEST(root_with_display_none_does_not_propagate_its_background)
{
    /* CSS Backgrounds 3 section 2.11.2: a root with display: none generates no box and its background does not reach the
     * canvas (the page stays blank). */
    ph h = ph_make("<html style=\"display:none;background:#0f0\"><body>x</body></html>", "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_GREEN), 0);
    ph_free(&h);
}

EMB_TEST(anonymous_blocks_do_not_repaint_the_background_of_their_parent)
{
    /* The block box with an inline-block child is wrapped in an anonymous block next to the other block; the anonymous
     * block shares the parent's style but must paint no background, or it would sit over a float painted before it. */
    ph h = ph_make("<div style=\"width:100px;height:60px;background:#f00\"><div style=\"height:10px\"></div>"
                   "<span style=\"display:inline-block;width:10px;height:10px\"></span></div>",
                   "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 1);
    ph_free(&h);
}

EMB_TEST(box_edges_snap_to_device_pixels)
{
    /* x = 10.4 and right edge 30.6 round to 10 and 31: the fill is 21 wide, not 20.2 (Blink's pixel snapping). */
    ph h = ph_make("<div style=\"margin-left:10.4px;width:20.2px;height:10px;background:#00f\"></div>", "");
    int i = ph_fill_of(&h.dl, PH_BLUE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(near(h.dl.items.data[i].rect[0], 10.0f));
    EMB_CHECK(near(h.dl.items.data[i].rect[2], 21.0f));
    ph_free(&h);
}

EMB_TEST(box_edges_snap_at_scale_two_to_device_pixels)
{
    /* At 2 device px per CSS px the grid is half a CSS px: 10.4 -> 10.5, 30.6 -> 30.5. */
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 2.0f};
    ph h = ph_make_cfg("<div style=\"margin-left:10.4px;width:20.2px;height:10px;background:#00f\"></div>", "", &cfg);
    int i = ph_fill_of(&h.dl, PH_BLUE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(near(h.dl.items.data[i].rect[0], 10.5f));
    EMB_CHECK(near(h.dl.items.data[i].rect[2], 20.0f));
    ph_free(&h);
}

EMB_TEST(column_backgrounds_paint_the_column_boxes_left_to_right)
{
    ph h = ph_make("<table style=\"border-spacing:0\"><colgroup><col style=\"width:30px;background:#f00\">"
                   "<col style=\"width:50px;background:#0f0\"></colgroup>"
                   "<tr><td style=\"height:20px;padding:0\"></td><td style=\"padding:0\"></td></tr></table>",
                   "");
    int r = ph_fill_of(&h.dl, PH_RED), g = ph_fill_of(&h.dl, PH_GREEN);
    EMB_REQUIRE(r >= 0 && g >= 0);
    EMB_CHECK(near(h.dl.items.data[r].rect[0], 0.0f) && near(h.dl.items.data[r].rect[2], 30.0f));
    EMB_CHECK(near(h.dl.items.data[g].rect[0], 30.0f) && near(h.dl.items.data[g].rect[2], 50.0f));
    EMB_CHECK(near(h.dl.items.data[r].rect[3], 20.0f)); /* as tall as the table's rows */
    ph_free(&h);
}

EMB_TEST(column_backgrounds_in_a_right_to_left_table_start_at_the_right_edge)
{
    /* The first column of an rtl table is the rightmost: the 80 px wide table sits at the right edge of the 800 px
     * viewport (x = 720..800), the red first column is 770..800 and the green second column is 720..770. */
    ph h = ph_make("<table dir=rtl style=\"border-spacing:0\"><colgroup><col style=\"width:30px;background:#f00\">"
                   "<col style=\"width:50px;background:#0f0\"></colgroup>"
                   "<tr><td style=\"height:20px;padding:0\"></td><td style=\"padding:0\"></td></tr></table>",
                   "body{direction:rtl}");
    int r = ph_fill_of(&h.dl, PH_RED), g = ph_fill_of(&h.dl, PH_GREEN);
    EMB_REQUIRE(r >= 0 && g >= 0);
    EMB_CHECK(near(h.dl.items.data[r].rect[0], 770.0f) && near(h.dl.items.data[r].rect[2], 30.0f));
    EMB_CHECK(near(h.dl.items.data[g].rect[0], 720.0f) && near(h.dl.items.data[g].rect[2], 50.0f));
    ph_free(&h);
}

EMB_TEST(collapsed_border_corner_golden)
{
    /* A one-cell collapsed table with a 10 px solid border: red on top and bottom, blue left and right, 40 x 40 in all.
     * Golden list of border items, in paint order: the red top and bottom edges span the full width (they own the corner squares'
     * outer halves), and the blue side edges run from the middle of the top joint to the middle of the bottom joint
     * (y = 5 to 35), so each corner square is red in its outer half and blue in its inner half. */
    ph h = ph_make("<table style=\"border-collapse:collapse\"><tr><td style=\"width:20px;height:20px;padding:0;"
                   "border:10px solid;border-color:#f00 #00f #f00 #00f\"></td></tr></table>",
                   "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 4);
    static const struct {
        uint32_t rgba;
        float r[4];
    } gold[4] = {
        {PH_RED, {0, 0, 40, 10}},
        {PH_RED, {0, 30, 40, 10}},
        {PH_BLUE, {0, 5, 10, 30}},
        {PH_BLUE, {30, 5, 10, 30}},
    };
    int n = 0;
    for (uint32_t i = 0; i < h.dl.items.len && n < 4; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind != EMBI_DI_BORDER) {
            continue;
        }
        EMB_CHECK(it->colors[0] == gold[n].rgba);
        EMB_CHECK(near(it->rect[0], gold[n].r[0]) && near(it->rect[1], gold[n].r[1]));
        EMB_CHECK(near(it->rect[2], gold[n].r[2]) && near(it->rect[3], gold[n].r[3]));
        n++;
    }
    EMB_CHECK_EQ_INT(n, 4);
    ph_free(&h);
}

EMB_TEST_MAIN()
