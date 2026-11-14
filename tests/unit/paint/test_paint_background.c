/*
 * Backgrounds, gradients, replaced images, outlines and line-box text geometry (W4c section 3), on the display
 * list. Image sizes come from a test callback: the URL "ok.png" is 10 x 10, anything else is unknown.
 */
#include "paint_harness.h"

#include "layout/box.h"
#include "layout/fragment.h"

#include <math.h>

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

static embi_paint_cfg image_cfg(void)
{
    embi_paint_cfg cfg = {nullptr, test_image_size, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    return cfg;
}

EMB_TEST(background_image_no_repeat_is_one_tile_at_its_size)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div style=\"width:40px;height:20px;background-image:url(ok.png);background-repeat:no-repeat;background-size:20px 20px\"></div>",
                       "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 1);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[2] == 20.0f && h.dl.items.data[i].rect[3] == 20.0f);
    EMB_CHECK(embi_dl_url(&h.dl, h.dl.items.data[i].aux).len == 6);
    ph_free(&h);
}

EMB_TEST(background_image_repeats_across_the_padding_box)
{
    embi_paint_cfg cfg = image_cfg();
    /* a 40 x 20 box with the natural 10 x 10 image: one tiled item covers the box (4 across and 2 down) */
    ph h = ph_make_cfg("<div style=\"width:40px;height:20px;background-image:url(ok.png)\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 1);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK((it->flags & EMBI_DF_TILE) != 0);
    EMB_CHECK(it->rect[0] == 0.0f && it->rect[1] == 0.0f && it->rect[2] == 40.0f && it->rect[3] == 20.0f);
    EMB_CHECK(it->f[2] == 10.0f && it->f[3] == 10.0f && it->f[4] == 10.0f && it->f[5] == 10.0f);
    /* the layer has a clip of the padding box */
    EMB_CHECK(it->clip != 0);
    ph_free(&h);
}

EMB_TEST(background_position_from_the_far_edge)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div style=\"width:40px;height:20px;background-image:url(ok.png);background-repeat:no-repeat;background-position:right bottom\"></div>", "", &cfg);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[0] == 30.0f);
    EMB_CHECK(h.dl.items.data[i].rect[1] == 10.0f);
    ph_free(&h);
}

EMB_TEST(background_space_spreads_whole_tiles)
{
    embi_paint_cfg cfg = image_cfg();
    /* 35 px fits 3 tiles of 10 with 2.5 px between them: one tiled item whose step is 12.5 px */
    ph h = ph_make_cfg("<div style=\"width:35px;height:10px;background-image:url(ok.png);background-repeat:space\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 1);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK((it->flags & EMBI_DF_TILE) != 0);
    EMB_CHECK(fabsf(it->f[0]) < 0.01f);
    EMB_CHECK(fabsf(it->f[4] - 12.5f) < 0.01f);
    /* the tiles end at the last one: 2 steps and a tile */
    EMB_CHECK(fabsf(it->rect[2] - 35.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(background_image_of_unknown_size_paints_nothing)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div style=\"width:40px;height:20px;background-image:url(missing.png)\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    ph_free(&h);
}

EMB_TEST(linear_gradient_to_right_spans_the_box)
{
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,#f00,#00f)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(it->sub == EMBI_GRAD_LINEAR_DL);
    /* the line runs from the left edge to the right edge at mid height */
    EMB_CHECK(fabsf(it->f[0]) < 0.01f);
    EMB_CHECK(fabsf(it->f[2] - 100.0f) < 0.01f);
    EMB_CHECK(fabsf(it->f[1] - 5.0f) < 0.01f);
    EMB_CHECK(fabsf(it->f[3] - 5.0f) < 0.01f);
    EMB_CHECK_EQ_INT(it->aux2, 2);
    const embi_dstop *st = &h.dl.stops.data[it->aux];
    EMB_CHECK(st[0].offset == 0.0f && st[0].color == PH_RED);
    EMB_CHECK(st[1].offset == 1.0f && st[1].color == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(linear_gradient_to_top_right_is_a_corner_line)
{
    /* the corner gradient line is perpendicular to the other diagonal: for a square it is 45 degrees up-right */
    ph h = ph_make("<div style=\"width:10px;height:10px;background-image:linear-gradient(to top right,#f00,#00f)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* the start is bottom-left, the end top-right */
    EMB_CHECK(it->f[0] < 5.0f && it->f[1] > 5.0f);
    EMB_CHECK(it->f[2] > 5.0f && it->f[3] < 5.0f);
    ph_free(&h);
}

EMB_TEST(radial_gradient_ellipse_farthest_corner)
{
    ph h = ph_make("<div style=\"width:40px;height:20px;background-image:radial-gradient(ellipse farthest-corner,red,blue)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(it->sub == EMBI_GRAD_RADIAL_DL);
    /* centered; the farthest corner from (20, 10) is 20 * sqrt(2) and 10 * sqrt(2) away */
    EMB_CHECK(fabsf(it->f[0] - 20.0f) < 0.01f && fabsf(it->f[1] - 10.0f) < 0.01f);
    EMB_CHECK(fabsf(it->f[2] - 20.0f * 1.41421356f) < 0.01f);
    EMB_CHECK(fabsf(it->f[3] - 10.0f * 1.41421356f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(radial_gradient_circle_farthest_corner_is_one_radius)
{
    ph h = ph_make("<div style=\"width:40px;height:20px;background-image:radial-gradient(circle farthest-corner,red,blue)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* the distance from (20, 10) to the far corner is sqrt(20^2 + 10^2) */
    EMB_CHECK(fabsf(it->f[2] - hypotf(20.0f, 10.0f)) < 0.01f);
    EMB_CHECK(it->f[2] == it->f[3]);
    ph_free(&h);
}

EMB_TEST(conic_gradient_keeps_its_start_angle)
{
    ph h = ph_make("<div style=\"width:20px;height:20px;background-image:conic-gradient(from 90deg,red,blue)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(it->sub == EMBI_GRAD_CONIC_DL);
    /* 90 degrees in radians */
    EMB_CHECK(fabsf(it->f[2] - 1.5707963f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(repeating_gradient_sets_the_repeat_flag)
{
    ph h = ph_make("<div style=\"width:10px;height:10px;background-image:repeating-linear-gradient(red 0px,blue 5px)\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK((h.dl.items.data[i].flags & EMBI_DF_REPEAT) != 0);
    ph_free(&h);
}

EMB_TEST(img_fills_its_content_box_by_default)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<img src=\"ok.png\" width=\"30\" height=\"15\">", "", &cfg);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[2] == 30.0f && h.dl.items.data[i].rect[3] == 15.0f);
    EMB_CHECK(h.dl.items.data[i].colors[0] == PH_WHITE);
    ph_free(&h);
}

EMB_TEST(img_object_fit_contain_centers_the_image)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<img src=\"ok.png\" width=\"30\" height=\"10\" style=\"object-fit:contain\">", "", &cfg);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* a 10 x 10 image in a 30 x 10 box: 10 x 10 at x = 10 */
    EMB_CHECK(fabsf(it->rect[2] - 10.0f) < 0.01f && fabsf(it->rect[3] - 10.0f) < 0.01f);
    EMB_CHECK(fabsf(it->rect[0] - 10.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(broken_img_paints_nothing)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<img src=\"missing.png\" width=\"30\" height=\"10\">", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    ph_free(&h);
}

EMB_TEST(outline_is_drawn_outside_the_border_box)
{
    ph h = ph_make("<div style=\"width:10px;height:10px;outline:2px solid #f00;outline-offset:1px\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* the outer edge is 1 + 2 px out from the border box, which is 10 x 10 at the origin */
    EMB_CHECK(it->rect[0] == -3.0f && it->rect[1] == -3.0f);
    EMB_CHECK(it->rect[2] == 16.0f && it->rect[3] == 16.0f);
    EMB_CHECK(it->widths[0] == 2.0f && it->widths[3] == 2.0f);
    EMB_CHECK(it->colors[1] == PH_RED);
    ph_free(&h);
}

EMB_TEST(outline_of_a_square_box_keeps_square_corners)
{
    /* a plain box: the outline's outer edge has square corners (its radii stay 0), as in Chromium */
    ph h = ph_make("<div style=\"width:10px;height:10px;outline:2px solid #f00\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    for (int k = 0; k < 4; k++) {
        EMB_CHECK(it->radii[k] == 0.0f);
    }
    ph_free(&h);
}

EMB_TEST(outline_of_a_rounded_box_grows_only_its_rounded_corners)
{
    /* top-left rounded by 4 px: with offset 1 px and width 2 px the outer edge is 3 px out, so the rounded corner is
     * 4 + 3 = 7 px and the square corners stay 0 */
    ph h = ph_make("<div style=\"width:10px;height:10px;border-top-left-radius:4px;outline:2px solid #f00;outline-offset:1px\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(fabsf(it->radii[0] - 7.0f) < 0.001f);
    EMB_CHECK(it->radii[1] == 0.0f && it->radii[2] == 0.0f && it->radii[3] == 0.0f);
    ph_free(&h);
}

EMB_TEST(outline_auto_is_the_focus_ring)
{
    ph h = ph_make("<div style=\"width:10px;height:10px;outline:auto\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK(it->widths[0] == 2.0f);
    EMB_CHECK(it->colors[0] == 0xFF1A73E8u);
    ph_free(&h);
}

EMB_TEST(lines_of_a_block_have_their_own_baselines)
{
    /* three lines of 20 px: each text item's baseline moves down by its line's top */
    ph h = ph_make("<div style=\"width:30px;line-height:20px;font-family:Ahem\">aaaa bbbb cccc</div>", "");
    uint32_t n = ph_count(&h.dl, EMBI_DI_TEXT);
    EMB_REQUIRE(n >= 2);
    int first = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(first >= 0);
    const embi_ditem *a = &h.dl.items.data[first];
    int second = -1;
    for (uint32_t i = (uint32_t)first + 1; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_TEXT && h.dl.items.data[i].rect[1] != a->rect[1]) {
            second = (int)i;
            break;
        }
    }
    EMB_REQUIRE(second >= 0);
    const embi_ditem *b = &h.dl.items.data[second];
    /* the offset of the baseline from the text top is the same on every line */
    EMB_CHECK(fabsf((a->f[1] - a->rect[1]) - (b->f[1] - b->rect[1])) < 0.01f);
    EMB_CHECK(fabsf((b->rect[1] - a->rect[1]) - (b->f[1] - a->f[1])) < 0.01f);
    ph_free(&h);
}

/* The fragment of the box of element el in the tree below f, or nullptr. */
static const embi_frag *frag_of(const embi_frag *f, const embi_element *el)
{
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el == el && k->box->pe == 0 && k->inl == 0) {
            return k;
        }
        if (k->kind != EMBI_FK_TEXT) {
            const embi_frag *r = frag_of(k, el);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return nullptr;
}

EMB_TEST(gradient_repeated_along_its_axis_is_one_item)
{
    /* a 10 x 10 tile of a left-to-right gradient repeated across a 40 x 10 box: one item over the box, repeat set */
    ph h = ph_make("<div style=\"width:40px;height:10px;background-image:linear-gradient(to right,#f00,#00f);background-size:10px 10px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_GRADIENT), 1);
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK((it->flags & EMBI_DF_REPEAT) != 0);
    EMB_CHECK(it->rect[0] == 0.0f && it->rect[2] == 40.0f && it->rect[3] == 10.0f);
    /* the line is one tile long: 10 px from the left edge of the first tile */
    EMB_CHECK(fabsf(it->f[2] - it->f[0] - 10.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(small_tiles_over_a_large_area_are_one_item_not_dropped)
{
    /* a 2 x 2 px image over 1280 x 720 px is 230,400 tiles: one tiled item covers the area */
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div style=\"width:1280px;height:720px;background-image:url(ok.png);background-size:2px 2px\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 1);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[2] == 1280.0f && h.dl.items.data[i].rect[3] == 720.0f);
    EMB_CHECK(h.dl.items.data[i].f[4] == 2.0f);
    ph_free(&h);
}

EMB_TEST(dense_radial_tiles_past_the_cap_are_drawn_not_dropped)
{
    /* a radial gradient in 2 x 2 px tiles over 1280 x 720 px is past the per-tile cap: one coarse item remains */
    ph h = ph_make("<div style=\"width:1280px;height:720px;background-image:radial-gradient(red,blue);background-size:2px 2px\"></div>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_GRADIENT) >= 1);
    ph_free(&h);
}

EMB_TEST(gradient_tiles_share_one_stop_range)
{
    /* 4 x 4 radial tiles over an 8 x 8 box: four items, one stop range */
    ph h = ph_make("<div style=\"width:8px;height:8px;background-image:radial-gradient(red,blue);background-size:4px 4px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_GRADIENT), 4);
    int i = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(i >= 0);
    uint32_t first = h.dl.items.data[i].aux;
    for (uint32_t k = 0; k < h.dl.items.len; k++) {
        const embi_ditem *it = &h.dl.items.data[k];
        if (it->kind == EMBI_DI_GRADIENT) {
            EMB_CHECK(it->aux == first && it->aux2 == 2);
        }
    }
    EMB_CHECK_EQ_INT((int)h.dl.stops.len, 2);
    ph_free(&h);
}

EMB_TEST(background_attachment_local_moves_with_the_scroll_offset)
{
    /* the origin of a local background scrolls with the content of its scroll container */
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div id=s style=\"width:20px;height:20px;overflow:auto;background-image:url(ok.png);background-repeat:no-repeat;background-attachment:local\"></div>", "", &cfg);
    embi_element *el = ph_id(&h, "s");
    EMB_REQUIRE(el != nullptr);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[1] == 0.0f);
    embi_frag *f = (embi_frag *)frag_of(embi_layout_root(h.eng, h.doc), el);
    EMB_REQUIRE(f != nullptr);
    embi_layout_frag_set_scroll(h.eng, h.doc, f, 0.0f, 4.0f);
    embi_dl_reset(&h.dl);
    embi_paint_document(&h.dl, h.eng, h.doc, &cfg);
    i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(fabsf(h.dl.items.data[i].rect[1] + 4.0f) < 0.01f);
    ph_free(&h);
}

EMB_TEST(background_attachment_scroll_does_not_move)
{
    embi_paint_cfg cfg = image_cfg();
    ph h = ph_make_cfg("<div id=s style=\"width:20px;height:20px;overflow:auto;background-image:url(ok.png);background-repeat:no-repeat\"></div>", "", &cfg);
    embi_element *el = ph_id(&h, "s");
    EMB_REQUIRE(el != nullptr);
    embi_frag *f = (embi_frag *)frag_of(embi_layout_root(h.eng, h.doc), el);
    EMB_REQUIRE(f != nullptr);
    embi_layout_frag_set_scroll(h.eng, h.doc, f, 0.0f, 4.0f);
    embi_dl_reset(&h.dl);
    embi_paint_document(&h.dl, h.eng, h.doc, &cfg);
    int i = ph_find(&h.dl, 0, EMBI_DI_IMAGE);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[1] == 0.0f);
    ph_free(&h);
}

EMB_TEST_MAIN()
