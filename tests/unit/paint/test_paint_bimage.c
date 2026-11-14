/*
 * Border images (CSS Backgrounds 3 section 6) on the display list: the slices, the widths and the outset, the repeat
 * modes of an edge, a gradient source, the sources that fall back to the border styles, and tables in the collapsing
 * border model. Image sizes come from a test callback: "ok.png" is 10 x 10, anything else is unknown.
 */
#include "paint_harness.h"

#include "layout/box.h"
#include "layout/fragment.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool bi_image_size(void *user, embi_str url, float *w, float *h, bool *has_ratio)
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

static embi_paint_cfg bi_cfg(void)
{
    embi_paint_cfg cfg = {nullptr, bi_image_size, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    return cfg;
}

static bool close_to(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* The index of the first item of kind k whose rect starts at (x, y), or -1. */
static int find_at(const embi_dl *d, uint8_t k, float x, float y)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == k && close_to(it->rect[0], x) && close_to(it->rect[1], y)) {
            return (int)i;
        }
    }
    return -1;
}

/* The tiles one IMAGE item draws: 1 for a plain image, else the tiles of a tiled item that meet its rect (as the renderer
 * counts them). */
static uint32_t item_tiles(const embi_ditem *it)
{
    if (!(it->flags & EMBI_DF_TILE)) {
        return 1;
    }
    uint32_t nx = 0, ny = 0;
    const float e = 0.01f, x1 = it->rect[0] + it->rect[2], y1 = it->rect[1] + it->rect[3];
    for (float x = it->f[0] + (floorf((it->rect[0] - it->f[0]) / it->f[4]) - 1.0f) * it->f[4]; x < x1; x += it->f[4]) {
        nx += x + it->f[2] > it->rect[0] + e && x < x1 - e;
    }
    for (float y = it->f[1] + (floorf((it->rect[1] - it->f[1]) / it->f[5]) - 1.0f) * it->f[5]; y < y1; y += it->f[5]) {
        ny += y + it->f[3] > it->rect[1] + e && y < y1 - e;
    }
    return nx * ny;
}

/* The number of tiles of the IMAGE items of the top band of a 10 px border (y 0, height 10) that lie in [x0, x1] (the band's
 * tiles, not the corners). */
static uint32_t band_tiles(const embi_dl *d, float x0, float x1)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_IMAGE && close_to(it->rect[1], 0.0f) && close_to(it->rect[3], 10.0f) &&
            it->rect[0] >= x0 - 0.01f && it->rect[0] + it->rect[2] <= x1 + 0.01f) {
            n += item_tiles(it);
        }
    }
    return n;
}

/* The tiles of the IMAGE items of a top band of height th (y 0) in [x0, x1]. */
static uint32_t band_tiles_h(const embi_dl *d, float x0, float x1, float th)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_IMAGE && close_to(it->rect[1], 0.0f) && close_to(it->rect[3], th) &&
            it->rect[0] >= x0 - 0.01f && it->rect[0] + it->rect[2] <= x1 + 0.01f) {
            n += item_tiles(it);
        }
    }
    return n;
}

/* A 40 x 40 border box at the origin (border-box sizing) with a 10 px border: the layout keeps the border width. */
#define BOX40 "position:absolute;left:0;top:0;box-sizing:border-box;width:40px;height:40px;border:10px solid;"

EMB_TEST(bimage_numbers_slice_into_eight_pieces_without_fill)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(ok.png) 2\"></div>", "", &cfg);
    /* the middle is discarded without fill: the four corners and four edges */
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 8);
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    const embi_ditem *it = &h.dl.items.data[tl];
    EMB_CHECK(close_to(it->rect[2], 10.0f) && close_to(it->rect[3], 10.0f));
    EMB_CHECK(close_to(it->f[0], 0.0f) && close_to(it->f[1], 0.0f) && close_to(it->f[2], 0.2f) && close_to(it->f[3], 0.2f));
    /* the top edge is stretched between the corners */
    int top = find_at(&h.dl, EMBI_DI_IMAGE, 10.0f, 0.0f);
    EMB_REQUIRE(top >= 0);
    EMB_CHECK(close_to(h.dl.items.data[top].rect[2], 20.0f) && close_to(h.dl.items.data[top].rect[3], 10.0f));
    EMB_CHECK(close_to(h.dl.items.data[top].f[0], 0.2f) && close_to(h.dl.items.data[top].f[2], 0.8f));
    ph_free(&h);
}

EMB_TEST(bimage_fill_draws_the_middle_slice)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(ok.png) 2 fill\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 9);
    int mid = find_at(&h.dl, EMBI_DI_IMAGE, 10.0f, 10.0f);
    EMB_REQUIRE(mid >= 0);
    const embi_ditem *it = &h.dl.items.data[mid];
    EMB_CHECK(close_to(it->rect[2], 20.0f) && close_to(it->rect[3], 20.0f));
    EMB_CHECK(close_to(it->f[0], 0.2f) && close_to(it->f[1], 0.2f) && close_to(it->f[2], 0.8f) && close_to(it->f[3], 0.8f));
    ph_free(&h);
}

EMB_TEST(bimage_percent_slice_is_of_the_image_size)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(ok.png) 50%\"></div>", "", &cfg);
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    /* 50% of the 10 px image is 5 px: the top-left corner is the top-left half of the image */
    EMB_CHECK(close_to(h.dl.items.data[tl].f[2], 0.5f) && close_to(h.dl.items.data[tl].f[3], 0.5f));
    ph_free(&h);
}

EMB_TEST(bimage_slices_that_overlap_are_scaled_down)
{
    embi_paint_cfg cfg = bi_cfg();
    /* Each slice is clamped to the 10 px image on its own, not scaled with its opposite: 7 stays 7. The top-left corner
     * is then image px 0..7, and the top-right corner (at x 30) is image px 3..10, so the corners overlap and the
     * middle is empty. */
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(ok.png) 7 fill\"></div>", "", &cfg);
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].f[2], 0.7f) && close_to(h.dl.items.data[tl].f[3], 0.7f));
    int tr = find_at(&h.dl, EMBI_DI_IMAGE, 30.0f, 0.0f);
    EMB_REQUIRE(tr >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tr].f[0], 0.3f) && close_to(h.dl.items.data[tr].f[2], 1.0f));
    EMB_CHECK(find_at(&h.dl, EMBI_DI_IMAGE, 10.0f, 10.0f) < 0);
    ph_free(&h);
}

EMB_TEST(bimage_width_number_multiplies_the_border_width)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:100px;height:100px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-width:2\"></div>",
                       "", &cfg);
    /* 2 x 10 px = 20 px corners */
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 20.0f) && close_to(h.dl.items.data[tl].rect[3], 20.0f));
    ph_free(&h);
}

EMB_TEST(bimage_width_auto_is_the_slice)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:100px;height:100px;"
                       "border:10px solid;border-image:url(ok.png) 3;border-image-width:auto\"></div>",
                       "", &cfg);
    /* the slice is 3 image px, so the corners are 3 px */
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 3.0f) && close_to(h.dl.items.data[tl].rect[3], 3.0f));
    ph_free(&h);
}

EMB_TEST(bimage_width_percent_is_of_the_border_image_area)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:100px;height:100px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-width:20%\"></div>",
                       "", &cfg);
    /* 20% of the 100 px border image area is 20 px */
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 20.0f) && close_to(h.dl.items.data[tl].rect[3], 20.0f));
    ph_free(&h);
}

EMB_TEST(bimage_overlapping_widths_scale_down_together)
{
    embi_paint_cfg cfg = bi_cfg();
    /* a 60 x 60 border box with 10 px borders and 40 px widths: the four 40 px widths overlap (80 > 60), so one factor
     * 60 / 80 scales them all to 30 px. (A box narrower than its borders grows to fit them, so the box is not 40.) */
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:60px;height:60px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-width:40px\"></div>",
                       "", &cfg);
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 30.0f) && close_to(h.dl.items.data[tl].rect[3], 30.0f));
    ph_free(&h);
}

EMB_TEST(bimage_outset_grows_the_area_outside_the_box)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"position:absolute;left:50px;top:50px;box-sizing:border-box;width:40px;height:40px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-outset:5px\"></div>",
                       "", &cfg);
    /* the corner is drawn 5 px outside the border box: from (45, 45) */
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 45.0f, 45.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 10.0f));
    ph_free(&h);
    /* a number is a multiple of the border width: 1 is 10 px */
    h = ph_make_cfg("<div style=\"position:absolute;left:50px;top:50px;box-sizing:border-box;width:40px;height:40px;"
                    "border:10px solid;border-image:url(ok.png) 2;border-image-outset:1\"></div>",
                    "", &cfg);
    EMB_CHECK(find_at(&h.dl, EMBI_DI_IMAGE, 40.0f, 40.0f) >= 0);
    ph_free(&h);
}

/* A 100 px content box with a 10 px border: the top band is 100 px long, between the corners at x 10 and 110. The
 * image is 10 px, its slice 2: the band's source is 6 px long and 2 px thick, so a tile is 30 px along the top. */
static uint32_t top_band_count(const char *repeat, const char *width)
{
    embi_paint_cfg cfg = bi_cfg();
    char html[512];
    snprintf(html, sizeof html,
             "<div style=\"position:absolute;left:0;top:0;width:%s;height:100px;border:10px solid;"
             "border-image:url(ok.png) 2;border-image-repeat:%s\"></div>",
             width, repeat);
    ph h = ph_make_cfg(html, "", &cfg);
    float band = (float)strtod(width, nullptr);
    uint32_t n = band_tiles(&h.dl, 10.0f, 10.0f + band);
    ph_free(&h);
    return n;
}

EMB_TEST(bimage_repeat_tiles_the_edge_centered_and_clipped)
{
    /* 100 px: tiles of 30 px centred on the middle: 3 whole tiles and two halves clipped at the ends (5 pieces) */
    EMB_CHECK_EQ_INT(top_band_count("repeat", "100px"), 5);
    /* the stretch of the same edge is one piece */
    EMB_CHECK_EQ_INT(top_band_count("stretch", "100px"), 1);
}

EMB_TEST(bimage_round_fits_a_whole_number_of_tiles)
{
    /* round(100 / 30) = 3 tiles, each 33.3 px: no clipping */
    EMB_CHECK_EQ_INT(top_band_count("round", "100px"), 3);
}

EMB_TEST(bimage_space_spreads_whole_tiles_with_gaps_at_the_ends)
{
    /* 100 px: 3 tiles of 30 px and 10 px of space in four gaps of 2.5 px */
    EMB_CHECK_EQ_INT(top_band_count("space", "100px"), 3);
    /* 65 px: 2 tiles and 5 px in three gaps: the first tile starts at 10 + 5/3 */
    EMB_CHECK_EQ_INT(top_band_count("space", "65px"), 2);
    /* 25 px fits no whole tile: nothing is drawn on this edge (the corners are) */
    EMB_CHECK_EQ_INT(top_band_count("space", "25px"), 0);
}

EMB_TEST(bimage_gradient_source_draws_gradient_pieces)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:linear-gradient(red,blue) 10\"></div>", "", &cfg);
    /* no image items; the eight pieces (corners and edges) are gradient items */
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_GRADIENT), 8);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 0);
    ph_free(&h);
}

EMB_TEST(bimage_none_paints_the_border_styles)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-color:red;border-image:none\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 1);
    ph_free(&h);
}

EMB_TEST(bimage_unloaded_image_paints_the_border_styles)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(missing.png) 2\"></div>", "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 1);
    ph_free(&h);
}

EMB_TEST(bimage_pixelated_samples_nearest)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:url(ok.png) 2;image-rendering:pixelated\"></div>", "", &cfg);
    int tl = find_at(&h.dl, EMBI_DI_IMAGE, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK((h.dl.items.data[tl].flags & EMBI_DF_NEAREST) != 0);
    ph_free(&h);
}

EMB_TEST(bimage_collapsed_table_ignores_border_image)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<table style=\"border-collapse:collapse;border:10px solid;border-image:url(ok.png) 2\">"
                       "<tr><td>x</td></tr></table>",
                       "", &cfg);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_IMAGE), 0);
    ph_free(&h);
    /* the separated model draws it */
    h = ph_make_cfg("<table style=\"border-collapse:separate;border:10px solid;border-image:url(ok.png) 2\">"
                    "<tr><td>x</td></tr></table>",
                    "", &cfg);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_IMAGE) > 0);
    ph_free(&h);
}

/* The number of tiles of the IMAGE items whose rect lies in the box [x0, x1] x [y0, y1]. */
static uint32_t count_box(const embi_dl *d, float x0, float x1, float y0, float y1)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_IMAGE && it->rect[0] >= x0 - 0.01f && it->rect[0] + it->rect[2] <= x1 + 0.01f &&
            it->rect[1] >= y0 - 0.01f && it->rect[1] + it->rect[3] <= y1 + 0.01f) {
            n += item_tiles(it);
        }
    }
    return n;
}

EMB_TEST(bimage_bottom_edge_uses_its_own_band_scale)
{
    embi_paint_cfg cfg = bi_cfg();
    /* a 120 px border box, 10 px borders; the bottom band is 30 px thick over a 2 px slice, so its tiles are 15 x 6 = 90 px
     * long (the top band, 10 px over 2 px, would give 30 px tiles). The 100 px edge holds three centred tiles, the first
     * and last clipped. */
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:120px;height:120px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-width:10px 10px 30px 10px;"
                       "border-image-repeat:repeat\"></div>",
                       "", &cfg);
    EMB_CHECK_EQ_INT(count_box(&h.dl, 10.0f, 110.0f, 90.0f, 120.0f), 3);
    /* one tiled item over the edge: tiles of 90 x 30 px from x -75, showing the slice (0.2 .. 0.8 x 0.8 .. 1) */
    int mid = find_at(&h.dl, EMBI_DI_IMAGE, 10.0f, 90.0f);
    EMB_REQUIRE(mid >= 0);
    const embi_ditem *it = &h.dl.items.data[mid];
    EMB_CHECK((it->flags & EMBI_DF_TILE) != 0);
    EMB_CHECK(close_to(it->f[0], -75.0f) && close_to(it->f[2], 90.0f) && close_to(it->f[3], 30.0f));
    EMB_CHECK(close_to(it->widths[0], 0.2f) && close_to(it->widths[1], 0.8f) && close_to(it->widths[2], 0.8f) &&
              close_to(it->widths[3], 1.0f));
    ph_free(&h);
}

EMB_TEST(bimage_right_edge_uses_its_own_band_scale)
{
    embi_paint_cfg cfg = bi_cfg();
    /* the right band is 30 px thick (scale 15, tiles 90 px tall); the left band is 10 px (scale 5). The 100 px edge holds
     * three centred tiles. */
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;box-sizing:border-box;width:120px;height:120px;"
                       "border:10px solid;border-image:url(ok.png) 2;border-image-width:10px 30px 10px 10px;"
                       "border-image-repeat:repeat\"></div>",
                       "", &cfg);
    EMB_CHECK_EQ_INT(count_box(&h.dl, 90.0f, 120.0f, 10.0f, 110.0f), 3);
    int mid = find_at(&h.dl, EMBI_DI_IMAGE, 90.0f, 10.0f);
    EMB_REQUIRE(mid >= 0);
    const embi_ditem *it = &h.dl.items.data[mid];
    EMB_CHECK((it->flags & EMBI_DF_TILE) != 0);
    EMB_CHECK(close_to(it->f[1], -75.0f) && close_to(it->f[2], 30.0f) && close_to(it->f[3], 90.0f));
    EMB_CHECK(close_to(it->widths[0], 0.8f) && close_to(it->widths[1], 0.2f) && close_to(it->widths[2], 1.0f) &&
              close_to(it->widths[3], 0.8f));
    ph_free(&h);
}

EMB_TEST(bimage_long_repeated_edge_is_one_item)
{
    embi_paint_cfg cfg = bi_cfg();
    /* a 1 px slice repeated along a 3000 px edge (tiles of 1 px, 3001 centred with the two ends half): one tiled item, not three thousand */
    ph h = ph_make_cfg("<div style=\"position:absolute;left:0;top:0;width:3000px;height:20px;border:1px solid;"
                       "border-image:url(ok.png) 1 4.5;border-image-repeat:repeat\"></div>",
                       "", &cfg);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_IMAGE) <= 8);
    EMB_CHECK_EQ_INT(band_tiles_h(&h.dl, 1.0f, 3001.0f, 1.0f), 3001);
    ph_free(&h);
}

EMB_TEST(bimage_gradient_width_auto_is_the_slice)
{
    embi_paint_cfg cfg = bi_cfg();
    ph h = ph_make_cfg("<div style=\"" BOX40 "border-image:linear-gradient(red,blue) 15;border-image-width:auto\"></div>", "",
                       &cfg);
    int tl = find_at(&h.dl, EMBI_DI_GRADIENT, 0.0f, 0.0f);
    EMB_REQUIRE(tl >= 0);
    EMB_CHECK(close_to(h.dl.items.data[tl].rect[2], 15.0f) && close_to(h.dl.items.data[tl].rect[3], 15.0f));
    ph_free(&h);
}

EMB_TEST(bimage_gradient_is_sized_to_the_outset_area)
{
    embi_paint_cfg cfg = bi_cfg();
    /* the border image area of a 40 px border box grown by 5 px is 50 px square at (45, 45): the gradient runs down from
     * its top (y 45) to its bottom (y 95). The top-left corner is 2 px of it, scaled 5 times. */
    ph h = ph_make_cfg("<div style=\"position:absolute;left:50px;top:50px;box-sizing:border-box;width:40px;height:40px;"
                       "border:10px solid;border-image:linear-gradient(red,blue) 2;border-image-outset:5px\"></div>",
                       "", &cfg);
    int tl = find_at(&h.dl, EMBI_DI_GRADIENT, 45.0f, 45.0f);
    EMB_REQUIRE(tl >= 0);
    const embi_ditem *it = &h.dl.items.data[tl];
    EMB_CHECK(close_to(it->rect[2], 10.0f) && close_to(it->rect[3], 10.0f));
    EMB_CHECK(close_to(it->f[1], 45.0f) && close_to(it->f[3], 295.0f));
    ph_free(&h);
}

EMB_TEST_MAIN()
