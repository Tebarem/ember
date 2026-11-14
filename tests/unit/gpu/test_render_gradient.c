#include "emb_render_test.h"

#include "render_check.h"

/* Gradients: linear, radial and conic, repeating and not, premultiplied stop interpolation,
 * stop cleanup, degenerate geometry, transforms, and clips. Expected values are worked out from
 * the gradient formulas by hand: a pixel center is (x + 0.5, y + 0.5), and t is the gradient
 * parameter at that center. The comment before each check gives t and the mix. */

#define W 64
#define H 64

static const embi_stop RED_TO_BLUE[2] = {{0.0f, RGBA(255, 0, 0, 255)},
                                         {1.0f, RGBA(0, 0, 255, 255)}};

/* ------------------------------------------------------------------ linear */

static void scene_linear_two(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, RED_TO_BLUE, 2, false, 0, 0);
}

EMB_TEST(linear_two_stops_interpolates_along_the_line)
{
    uint8_t *px = emb_test_render(W, H, scene_linear_two, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* x = 0: t = 0.5/64, so red 253, blue 2. */
    CHECK_PX_NEAR(px, W, 0, 32, 253, 0, 2, 255, 2);
    /* x = 31: t = 31.5/64 = 0.4921875, so red 129, blue 125. */
    CHECK_PX_NEAR(px, W, 31, 32, 129, 0, 125, 255, 2);
    /* x = 63: t = 63.5/64, so red 2, blue 253. */
    CHECK_PX_NEAR(px, W, 63, 32, 2, 0, 253, 255, 2);
    SDL_free(px);
}

static void scene_linear_clamp(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.25f, RGBA(255, 0, 0, 255)}, {0.75f, RGBA(0, 0, 255, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 2, false, 0, 0);
}

EMB_TEST(linear_outside_the_stops_keeps_the_nearest_color)
{
    uint8_t *px = emb_test_render(W, H, scene_linear_clamp, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* t = 0.0390625 < 0.25 is red, and t = 0.9765625 > 0.75 is blue. */
    EMB_CHECK_PIXEL(px, W, 2, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 62, 32, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_linear_repeat(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.0f, RGBA(255, 0, 0, 255)}, {0.25f, RGBA(0, 0, 255, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 2, true, 0, 0);
}

EMB_TEST(linear_repeat_wraps_over_the_stop_span)
{
    uint8_t *px = emb_test_render(W, H, scene_linear_repeat, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* x = 2: t = 0.0390625, below the period, so f = t / 0.25 = 0.15625: red 215, blue 40. */
    CHECK_PX_NEAR(px, W, 2, 32, 215, 0, 40, 255, 2);
    /* x = 40: t = 0.6328125 wraps to 0.1328125 (two periods gone), so f = 0.53125: red 120,
     * blue 135. Without the wrap this pixel would be solid blue. */
    CHECK_PX_NEAR(px, W, 40, 32, 120, 0, 135, 255, 2);
    SDL_free(px);
}

static void scene_linear_hard(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.5f, RGBA(255, 0, 0, 255)}, {0.5f, RGBA(0, 0, 255, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 2, false, 0, 0);
}

EMB_TEST(linear_equal_offsets_make_a_hard_edge)
{
    uint8_t *px = emb_test_render(W, H, scene_linear_hard, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* t = 0.3203 is before the edge, t = 0.6328 after it. Nothing is blended. */
    EMB_CHECK_PIXEL(px, W, 20, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 40, 32, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_premul(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.0f, RGBA(255, 0, 0, 0)}, {1.0f, RGBA(0, 0, 255, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 2, false, 0, 0);
}

EMB_TEST(linear_stops_interpolate_premultiplied)
{
    uint8_t *px = emb_test_render(W, H, scene_premul, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* A transparent red stop must not tint the middle. At t = 0.4921875 the premultiplied mix
     * is (0, 0, 125.5, 125.5). Interpolating unpremultiplied colors would leave red near 63. */
    CHECK_PX_NEAR(px, W, 31, 32, 0, 0, 125, 125, 2);
    SDL_free(px);
}

static void scene_monotonic(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    /* The second offset (0.2) is below the first (0.6), so it is raised to 0.6. */
    const embi_stop st[3] = {{0.6f, RGBA(255, 0, 0, 255)},
                             {0.2f, RGBA(0, 0, 255, 255)},
                             {1.0f, RGBA(0, 255, 0, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 3, false, 0, 0);
}

EMB_TEST(linear_decreasing_offsets_are_raised_to_the_previous)
{
    uint8_t *px = emb_test_render(W, H, scene_monotonic, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* x = 19: t = 0.3046875 < 0.6, so red. x = 51: t = 0.8046875 between the blue stop at 0.6
     * and the green stop at 1.0, f = 0.51171875: green 130.5, blue 124.5. */
    EMB_CHECK_PIXEL(px, W, 19, 32, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, W, 51, 32, 0, 130, 125, 255, 3);
    SDL_free(px);
}

/* ------------------------------------------------------------------ radial */

static void scene_radial_circle(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.0f, RGBA(255, 255, 255, 255)}, {1.0f, RGBA(0, 0, 0, 255)}};
    embi_frame_radial(f, rect, radii, 32, 32, 32, 32, st, 2, false, 0, 0);
}

EMB_TEST(radial_circle_grows_from_the_center)
{
    uint8_t *px = emb_test_render(W, H, scene_radial_circle, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Pixel (48, 32): center offset (16.5, 0.5), distance 16.508 / 32 = 0.51587, gray 123.5. */
    CHECK_PX_NEAR(px, W, 48, 32, 124, 124, 124, 255, 3);
    /* Corner: distance 1.39, past the last stop, so black. */
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 255);
    SDL_free(px);
}

static void scene_radial_ellipse(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.0f, RGBA(255, 255, 255, 255)}, {1.0f, RGBA(0, 0, 0, 255)}};
    /* Radii 16 across and 32 down. */
    embi_frame_radial(f, rect, radii, 32, 32, 16, 32, st, 2, false, 0, 0);
}

EMB_TEST(radial_ellipse_uses_separate_radii)
{
    uint8_t *px = emb_test_render(W, H, scene_radial_ellipse, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Pixel (40, 32): offset (8.5, 0.5) divided by (16, 32) is (0.53125, 0.015625), distance
     * 0.53196, gray 119.3. */
    CHECK_PX_NEAR(px, W, 40, 32, 119, 119, 119, 255, 3);
    /* Pixel (48, 32): distance 1.03, past the edge, so black. */
    EMB_CHECK_PIXEL(px, W, 48, 32, 0, 0, 0, 255);
    SDL_free(px);
}

static void scene_radial_repeat(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[2] = {{0.0f, RGBA(255, 0, 0, 255)}, {0.5f, RGBA(0, 0, 255, 255)}};
    /* Period 0.5 of a 16 px radius, so 8 px. */
    embi_frame_radial(f, rect, radii, 32, 32, 16, 16, st, 2, true, 0, 0);
}

EMB_TEST(radial_repeat_rings)
{
    uint8_t *px = emb_test_render(W, H, scene_radial_repeat, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Pixel (40, 32): distance 8.5147 / 16 = 0.53217, wrapped to 0.03217, so f = 0.06434: red
     * 239, blue 16. */
    CHECK_PX_NEAR(px, W, 40, 32, 239, 0, 16, 255, 3);
    SDL_free(px);
}

/* ------------------------------------------------------------------ conic */

static void scene_conic_red_blue_red(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[3] = {{0.0f, RGBA(255, 0, 0, 255)},
                             {0.5f, RGBA(0, 0, 255, 255)},
                             {1.0f, RGBA(255, 0, 0, 255)}};
    embi_frame_conic(f, rect, radii, 32, 32, 0.0f, st, 3, false, 0, 0);
}

EMB_TEST(conic_sweeps_clockwise_from_the_top)
{
    uint8_t *px = emb_test_render(W, H, scene_conic_red_blue_red, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Right side, angle 90.99 degrees: t = 0.25280, f = 0.50559 from red to blue: red 126,
     * blue 129. */
    CHECK_PX_NEAR(px, W, 60, 32, 126, 0, 129, 255, 3);
    /* Bottom, t = 0.49721: almost pure blue. */
    CHECK_PX_NEAR(px, W, 32, 60, 1, 0, 254, 255, 3);
    /* Left side, t = 0.74709: the same mix as the right side, reversed. */
    CHECK_PX_NEAR(px, W, 4, 32, 126, 0, 129, 255, 3);
    SDL_free(px);
}

static void scene_conic_from(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    const embi_stop st[3] = {{0.0f, RGBA(255, 0, 0, 255)},
                             {0.5f, RGBA(0, 0, 255, 255)},
                             {1.0f, RGBA(255, 0, 0, 255)}};
    /* Start at 90 degrees, so the top is a quarter turn into the gradient. */
    embi_frame_conic(f, rect, radii, 32, 32, 1.5707963267948966f, st, 3, false, 0, 0);
}

EMB_TEST(conic_start_angle_rotates_the_sweep)
{
    uint8_t *px = emb_test_render(W, H, scene_conic_from, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Top: the angle is 1.04 degrees, minus 90 gives t = 0.75290 in the blue-to-red half, so
     * f = 0.50580: red 129, blue 126. */
    CHECK_PX_NEAR(px, W, 32, 4, 129, 0, 126, 255, 3);
    SDL_free(px);
}

/* ------------------------------------------------------------------ degenerate and empty */

static void scene_degenerate_linear(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    /* The line has zero length, and repeat is set. The last stop's color is painted. */
    embi_frame_linear(f, rect, radii, 10, 10, 10, 10, RED_TO_BLUE, 2, true, 0, 0);
}

EMB_TEST(zero_length_linear_paints_the_last_stop)
{
    uint8_t *px = emb_test_render(W, H, scene_degenerate_linear, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 3, 3, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 60, 50, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_degenerate_radial(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_radial(f, rect, radii, 32, 32, 0, 16, RED_TO_BLUE, 2, false, 0, 0);
}

EMB_TEST(zero_radius_radial_paints_the_last_stop)
{
    uint8_t *px = emb_test_render(W, H, scene_degenerate_radial, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_no_stops(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, RED_TO_BLUE, 0, false, 0, 0);
    embi_frame_conic(f, rect, radii, 32, 32, 0, RED_TO_BLUE, 0, false, 0, 0);
}

EMB_TEST(gradient_with_no_stops_draws_nothing)
{
    uint8_t *px = emb_test_render(W, H, scene_no_stops, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 0, 0);
    SDL_free(px);
}

/* ------------------------------------------------------------------ transforms and clips */

static void scene_xform(embi_frame *f, void *user)
{
    (void)user;
    /* The gradient runs over local x 0..16 and the box is scaled by 2 to device 0..32. */
    const float m[6] = {2, 0, 0, 2, 0, 0};
    const float rect[4] = {0, 0, 16, 16};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_linear(f, rect, radii, 0, 0, 16, 0, RED_TO_BLUE, 2, false, x, 0);
}

EMB_TEST(gradient_follows_the_transform)
{
    uint8_t *px = emb_test_render(W, H, scene_xform, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Device pixel (4, 8): local x = 4.5 / 2 = 2.25, so t = 2.25 / 16 = 0.140625: red 219,
     * blue 36. */
    CHECK_PX_NEAR(px, W, 4, 8, 219, 0, 36, 255, 2);
    /* Outside the scaled box. */
    EMB_CHECK_PIXEL(px, W, 40, 8, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_rounded_clipped(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {8, 8, 48, 48};
    const float radii[4] = {12, 12, 12, 12};
    const float clip_rect[4] = {0, 0, 32, 64};
    const float zero[4] = {0, 0, 0, 0};
    const uint32_t c = embi_frame_clip(f, clip_rect, zero, 0, 0);
    embi_frame_linear(f, rect, radii, 8, 0, 56, 0, RED_TO_BLUE, 2, false, 0, c);
}

EMB_TEST(gradient_respects_rounded_corners_and_clips)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded_clipped, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* The corner outside the 12 px arc is empty. */
    EMB_CHECK_PIXEL(px, W, 8, 8, 0, 0, 0, 0);
    /* Inside the arc and the clip: opaque. */
    EMB_CHECK(px[(32 * W + 20) * 4 + 3] == 255);
    /* Past the clip at x = 32: empty even though the gradient covers it. */
    EMB_CHECK_PIXEL(px, W, 40, 32, 0, 0, 0, 0);
    SDL_free(px);
}

/* ------------------------------------------------------------------ multi-stop and repeat */

static void scene_linear_three(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    /* Two segments: red to green over [0, 0.5], green to blue over [0.5, 1]. */
    const embi_stop st[3] = {{0.0f, RGBA(255, 0, 0, 255)},
                             {0.5f, RGBA(0, 255, 0, 255)},
                             {1.0f, RGBA(0, 0, 255, 255)}};
    embi_frame_linear(f, rect, radii, 0, 0, W, 0, st, 3, false, 0, 0);
}

EMB_TEST(linear_three_stops_second_segment)
{
    uint8_t *px = emb_test_render(W, H, scene_linear_three, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* x = 16: t = 16.5/64 = 0.2578125, first segment f = 0.515625: red 123.5, green 131.5. */
    CHECK_PX_NEAR(px, W, 16, 32, 124, 131, 0, 255, 3);
    /* x = 32: t = 32.5/64 = 0.5078125, second segment f = 0.015625: green 251, blue 4. */
    CHECK_PX_NEAR(px, W, 32, 32, 0, 251, 4, 255, 3);
    /* x = 48: t = 48.5/64 = 0.7578125, second segment f = 0.515625: green 123.5, blue 131.5.
     * This is the pixel in the second segment that a bad stop search would get wrong. */
    CHECK_PX_NEAR(px, W, 48, 32, 0, 124, 131, 255, 3);
    /* x = 63: t = 63.5/64 = 0.9921875, second segment f = 0.984375: green 4, blue 251. */
    CHECK_PX_NEAR(px, W, 63, 32, 0, 4, 251, 255, 3);
    SDL_free(px);
}

static void scene_conic_repeat_from(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, W, H};
    const float radii[4] = {0, 0, 0, 0};
    /* Stops span a quarter turn, so repeating wraps every 0.25 turns. Starts at 90 degrees. */
    const embi_stop st[2] = {{0.0f, RGBA(255, 0, 0, 255)}, {0.25f, RGBA(0, 0, 255, 255)}};
    embi_frame_conic(f, rect, radii, 32, 32, 1.5707963267948966f, st, 2, true, 0, 0);
}

EMB_TEST(conic_repeat_with_start_angle_wraps_each_quarter_turn)
{
    uint8_t *px = emb_test_render(W, H, scene_conic_repeat_from, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Pixel (23, 58), center offset (-8.5, 26.5) from (32, 32). CSS angle from 3 o'clock,
     * clockwise: atan2(26.5, -8.5) = 1.881185 rad, so t = 0.299393 turns. Repeating over 0.25
     * gives 0.049393, f = 0.197572: red 204.6, blue 50.4. Without the repeat, t is past the blue
     * stop and the pixel would be solid blue. */
    CHECK_PX_NEAR(px, W, 23, 58, 205, 0, 50, 255, 4);
    SDL_free(px);
}

/* ------------------------------------------------------------------ golden scene */

static void scene_gradient_golden(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};

    /* Full-canvas dark-to-light background, diagonal. */
    const float bg[4] = {0, 0, 128, 128};
    const embi_stop bg_st[2] = {{0.0f, RGBA(20, 30, 80, 255)}, {1.0f, RGBA(200, 220, 255, 255)}};
    embi_frame_linear(f, bg, zero, 0, 0, 128, 128, bg_st, 2, false, 0, 0);

    /* Rounded card with a radial highlight. */
    const float card[4] = {8, 8, 56, 40};
    const float card_r[4] = {10, 10, 10, 10};
    const embi_stop card_st[2] = {{0.0f, RGBA(255, 255, 255, 255)},
                                  {1.0f, RGBA(255, 128, 0, 255)}};
    embi_frame_radial(f, card, card_r, 36, 28, 40, 30, card_st, 2, false, 0, 0);

    /* Disc with a conic sweep. */
    const float disc[4] = {72, 72, 48, 48};
    const float disc_r[4] = {24, 24, 24, 24};
    const embi_stop disc_st[5] = {{0.0f, RGBA(255, 0, 0, 255)},
                                  {0.25f, RGBA(0, 255, 0, 255)},
                                  {0.5f, RGBA(0, 0, 255, 255)},
                                  {0.75f, RGBA(255, 255, 0, 255)},
                                  {1.0f, RGBA(255, 0, 0, 255)}};
    embi_frame_conic(f, disc, disc_r, 96, 96, 0.0f, disc_st, 5, false, 0, 0);

    /* Repeating stripes along the bottom band. */
    const float band[4] = {0, 100, 128, 28};
    const embi_stop band_st[2] = {{0.0f, RGBA(255, 255, 255, 255)},
                                  {0.5f, RGBA(0, 0, 0, 255)}};
    embi_frame_linear(f, band, zero, 0, 0, 16, 0, band_st, 2, true, 0, 0);
}

EMB_TEST(gradient_scene_matches_golden)
{
    uint8_t *px = emb_test_render(128, 128, scene_gradient_golden, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Background corner: t = 0.0039, so about (21, 31, 81). */
    CHECK_PX_NEAR(px, 128, 0, 0, 21, 31, 81, 255, 3);
    /* Conic disc, pixel (96, 80): t = 0.00514, f = 0.02056: red 250, green 5. */
    CHECK_PX_NEAR(px, 128, 96, 80, 250, 5, 0, 255, 3);
    /* Repeating stripe, pixel (4, 110): t = 0.28125, f = 0.5625 of the first half: gray 112. */
    CHECK_PX_NEAR(px, 128, 4, 110, 112, 112, 112, 255, 3);
    EMB_CHECK(emb_gold_check("render_gradient_scene", px, 128, 128));
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
