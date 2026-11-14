#include "emb_render_test.h"

#include "render_check.h"

/* One composite scene of every feature in R1a, checked two ways: a few interior pixels with
 * hand-computed colors (so the golden cannot hide a bug), and the whole image against
 * tests/gold/render_core_scene.bmp for everything else (edges, anti-aliasing).
 *
 * Layout on a 128 x 128 target, drawn in this order:
 *   blue background; white card at (8, 8, 56, 40) with radius 10; a 4 px bordered box at
 *   (72, 8, 48, 40) with radius 12 (red top, green right, blue bottom, yellow left); an
 *   orange 50% translucent square rotated 30 degrees about (32, 96); a green fill clipped to
 *   a circle at (96, 96) of radius 24; a purple fill clipped by a nested pair of clips in the
 *   bottom band. */

#define W 128
#define H 128

static void scene_core(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};

    /* Blue background. */
    const float bg[4] = {0, 0, 128, 128};
    embi_frame_fill(f, bg, zero, RGBA(40, 60, 200, 255), 0, 0);

    /* White rounded card. */
    const float card[4] = {8, 8, 56, 40};
    const float card_r[4] = {10, 10, 10, 10};
    embi_frame_fill(f, card, card_r, RGBA(255, 255, 255, 255), 0, 0);

    /* Bordered rounded box, one color per side. */
    const float box[4] = {72, 8, 48, 40};
    const float box_r[4] = {12, 12, 12, 12};
    const float widths[4] = {4, 4, 4, 4};
    const uint32_t colors[4] = {RGBA(255, 0, 0, 255), RGBA(0, 255, 0, 255),
                                RGBA(0, 0, 255, 255), RGBA(255, 255, 0, 255)};
    embi_frame_border(f, box, box_r, widths, colors, 0, 0);

    /* Translucent square, rotated 30 degrees about (32, 96). */
    const float c30 = 0.8660254f;
    const float s30 = 0.5f;
    const float rot[6] = {c30, s30, -s30, c30, 32, 96};
    const float sq[4] = {-12, -12, 24, 24};
    const uint32_t xr = embi_frame_xform(f, rot);
    embi_frame_fill(f, sq, zero, RGBA(255, 128, 0, 160), xr, 0);

    /* Green fill clipped to a circle. */
    const float disc[4] = {72, 72, 48, 48};
    const float disc_r[4] = {24, 24, 24, 24};
    const float green_box[4] = {64, 64, 64, 64};
    const uint32_t cdisc = embi_frame_clip(f, disc, disc_r, 0, 0);
    embi_frame_fill(f, green_box, zero, RGBA(0, 255, 0, 255), 0, cdisc);

    /* Purple fill in the bottom band, clipped by two nested rects. */
    const float band[4] = {0, 100, 128, 28};
    const float half[4] = {0, 100, 64, 28};
    const float full[4] = {0, 0, 128, 128};
    const uint32_t cb = embi_frame_clip(f, band, zero, 0, 0);
    const uint32_t ch = embi_frame_clip(f, half, zero, 0, cb);
    embi_frame_fill(f, full, zero, RGBA(128, 0, 128, 255), 0, ch);
}

EMB_TEST(render_core_scene_matches_golden)
{
    uint8_t *px = emb_test_render(W, H, scene_core, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Hand-computed interior samples. */
    /* Background, outside the card and the box. */
    EMB_CHECK_PIXEL(px, W, 5, 5, 40, 60, 200, 255);
    /* Card interior. */
    EMB_CHECK_PIXEL(px, W, 30, 20, 255, 255, 255, 255);
    /* Border bands: top red, right green, bottom blue, left yellow. */
    EMB_CHECK_PIXEL(px, W, 96, 9, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 117, 28, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 96, 45, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 73, 28, 255, 255, 0, 255);
    /* The hole of the box shows the background. */
    EMB_CHECK_PIXEL(px, W, 96, 28, 40, 60, 200, 255);
    /* Translucent orange over blue, premultiplied: 160/255 orange, the rest blue. Off by at
     * most two from the exact (175, 103, 75). */
    CHECK_PX_NEAR(px, W, 32, 96, 175, 103, 75, 255, 2);
    /* Clipped green disc: center in, corner of its box out (background shows there). */
    EMB_CHECK_PIXEL(px, W, 96, 96, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 74, 74, 40, 60, 200, 255);
    /* Purple band: inside both clips, and outside the narrower one. */
    EMB_CHECK_PIXEL(px, W, 10, 110, 128, 0, 128, 255);
    EMB_CHECK_PIXEL(px, W, 100, 126, 40, 60, 200, 255);

    /* Everything else, including every anti-aliased edge, against the golden. */
    EMB_CHECK(emb_gold_check("render_core_scene", px, W, H));
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
