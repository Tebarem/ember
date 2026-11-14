#include "emb_render_test.h"

#include "render_check.h"

/* Solid-style borders: uniform widths, per-side widths, rounded outer edges, and the zero-width
 * case. The box is 40 x 40 at the origin of a 64 x 64 target. Side colors: top red, right
 * green, bottom blue, left yellow. */

#define W 64
#define H 64

#define RED RGBA(255, 0, 0, 255)
#define GREEN RGBA(0, 255, 0, 255)
#define BLUE RGBA(0, 0, 255, 255)
#define YELLOW RGBA(255, 255, 0, 255)

static const uint32_t side_colors[4] = {RED, GREEN, BLUE, YELLOW};

static void scene_uniform(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    const float widths[4] = {4, 4, 4, 4};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_uniform_width_paints_each_side)
{
    uint8_t *px = emb_test_render(W, H, scene_uniform, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Band centers, one pixel in from each outer edge. */
    EMB_CHECK_PIXEL(px, W, 20, 1, 255, 0, 0, 255);   /* top: red */
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 255, 0, 255);  /* right: green */
    EMB_CHECK_PIXEL(px, W, 20, 38, 0, 0, 255, 255);  /* bottom: blue */
    EMB_CHECK_PIXEL(px, W, 1, 20, 255, 255, 0, 255); /* left: yellow */
    /* The hole, and the pixel just outside the box. */
    EMB_CHECK_PIXEL(px, W, 20, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 40, 20, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_per_side(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    /* Top 2, right 4, bottom 6, left 8. */
    const float widths[4] = {2, 4, 6, 8};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_per_side_widths_set_each_band)
{
    uint8_t *px = emb_test_render(W, H, scene_per_side, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Top band is y in [0, 2): pixel row 0 is red, row 2 is the hole. */
    EMB_CHECK_PIXEL(px, W, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 2, 0, 0, 0, 0);
    /* Right band is x in [36, 40). */
    EMB_CHECK_PIXEL(px, W, 37, 20, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 35, 20, 0, 0, 0, 0);
    /* Bottom band is y in [34, 40). */
    EMB_CHECK_PIXEL(px, W, 20, 35, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 20, 33, 0, 0, 0, 0);
    /* Left band is x in [0, 8). */
    EMB_CHECK_PIXEL(px, W, 2, 20, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 9, 20, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_rounded(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {12, 12, 12, 12};
    const float widths[4] = {4, 4, 4, 4};
    const uint32_t colors[4] = {RED, RED, RED, RED};
    embi_frame_border(f, rect, radii, widths, colors, 0, 0);
}

EMB_TEST(border_rounded_outer_edge_clips_the_corner)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* The outer arc is centered at (12, 12) with radius 12. (0, 0) is 16 px from that center. */
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    /* The straight top band is unaffected. */
    EMB_CHECK_PIXEL(px, W, 20, 1, 255, 0, 0, 255);
    /* The inner hole still exists. */
    EMB_CHECK_PIXEL(px, W, 20, 20, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_zero_width(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    const float widths[4] = {0, 0, 0, 0};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_zero_widths_draw_nothing)
{
    uint8_t *px = emb_test_render(W, H, scene_zero_width, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 20, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 20, 0, 0, 0, 0);
    SDL_free(px);
}

/* Corner joins. The border splits along the line from each outer corner to the matching inner
 * corner (CSS). Pixel centers are at (x + 0.5, y + 0.5), so each sample sits on one side of a
 * diagonal and far from it, and the expected color is hand-computed from which side of the line
 * the center is on. */

static void scene_even_corners(embi_frame *f, void *user)
{
    (void)user;
    /* Uniform 4 px width: the TL diagonal is y = x, the BR diagonal is y = x too (the box is
     * square), and the TR diagonal is y = 40 - x. */
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    const float widths[4] = {4, 4, 4, 4};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_uniform_corner_joins_split_diagonally)
{
    uint8_t *px = emb_test_render(W, H, scene_even_corners, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Top-left: above the diagonal is top (red), below is left (yellow). */
    EMB_CHECK_PIXEL(px, W, 2, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 1, 2, 255, 255, 0, 255);
    /* Top-right: above y = 40 - x is top, below is right (green). */
    EMB_CHECK_PIXEL(px, W, 37, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 38, 2, 0, 255, 0, 255);
    /* Bottom-right: above y = x is right, below is bottom (blue). */
    EMB_CHECK_PIXEL(px, W, 38, 37, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 37, 38, 0, 0, 255, 255);
    /* Bottom-left: the diagonal is x + y = 40. Centers with x + y below 40 are left (yellow),
     * above 40 are bottom (blue). (1, 37) has x + y = 39 and (2, 38) has 41. */
    EMB_CHECK_PIXEL(px, W, 1, 37, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 2, 38, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_per_side_corners(embi_frame *f, void *user)
{
    (void)user;
    /* Widths top 2, right 4, bottom 6, left 8. The four diagonals run from each outer corner to
     * the inner corner: TL (0,0)-(8,2), TR (40,0)-(36,2), BR (40,40)-(36,34), BL (0,40)-(8,34). */
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    const float widths[4] = {2, 4, 6, 8};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_per_side_corner_joins_split_diagonally)
{
    uint8_t *px = emb_test_render(W, H, scene_per_side_corners, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Corner joins are anti-aliased: the side color is blended with the neighbor across the
     * diagonal by the one px coverage of the perpendicular distance d from the pixel center to
     * the line, a = clamp(0.5 - d, 0, 1) for the far side. Pixels more than half a px from every
     * diagonal are pure; the values below were computed with the shader's formula.
     *
     * TL diagonal y = x / 4 (from (0, 0) to (8, 2)). At x = 6.5 it is at y = 1.625. Row 1
     * (center 1.5) is above it by 0.125 vertically, d = 0.125 * 4 / sqrt(17) = 0.121, so the
     * top keeps 0.621 and left gets 0.379: (255, 97, 0). Row 2 is 0.849 px below it: pure left. */
    CHECK_PX_NEAR(px, W, 6, 1, 255, 97, 0, 255, 2);
    EMB_CHECK_PIXEL(px, W, 6, 2, 255, 255, 0, 255);
    /* TL again, at x = 3.5 the line is at y = 0.875: row 0 is 0.364 px above it (top 0.864),
     * row 1 is 0.606 px below it (pure left). */
    CHECK_PX_NEAR(px, W, 3, 0, 255, 35, 0, 255, 2);
    EMB_CHECK_PIXEL(px, W, 3, 1, 255, 255, 0, 255);
    /* TR diagonal (from (40, 0) to (36, 2)), y = (40 - x) / 2. At x = 38.5 it is at y = 0.75:
     * row 0 is 0.25 px above it vertically (d = 0.224, top 0.724, right 0.276); row 1 is pure
     * right. */
    CHECK_PX_NEAR(px, W, 38, 0, 185, 70, 0, 255, 2);
    EMB_CHECK_PIXEL(px, W, 38, 1, 0, 255, 0, 255);
    /* BR diagonal (from (40, 40) to (36, 34)). At x = 38.5 it is at y = 37.75: row 37 keeps
     * right at 0.639, row 38 keeps bottom at 0.916. */
    CHECK_PX_NEAR(px, W, 38, 37, 0, 163, 92, 255, 2);
    CHECK_PX_NEAR(px, W, 38, 38, 0, 21, 234, 255, 2);
    /* BL diagonal (from (0, 40) to (8, 34)). At x = 4.5 it is at y = 36.625: row 36 keeps left
     * at 0.6 (blend with bottom), row 37 is pure bottom. */
    CHECK_PX_NEAR(px, W, 4, 36, 153, 153, 102, 255, 2);
    EMB_CHECK_PIXEL(px, W, 4, 37, 0, 0, 255, 255);
    /* Mid-side pixels are pure. */
    EMB_CHECK_PIXEL(px, W, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 20, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 39, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 0, 20, 255, 255, 0, 255);
    SDL_free(px);
}

EMB_TEST(border_corner_join_on_a_pixel_center_is_an_even_blend)
{
    uint8_t *px = emb_test_render(W, H, scene_even_corners, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* A pixel center exactly on a diagonal is a tie between two sides. GPUs break a hard
     * comparison differently (AMD vs WARP vs Metal disagreed), so the join must be an even blend
     * there: TL (2.5, 2.5) is top+left, TR (37.5, 2.5) top+right, BR (37.5, 37.5) right+bottom,
     * BL (2.5, 37.5) bottom+left. */
    CHECK_PX_NEAR(px, W, 2, 2, 255, 128, 0, 255, 2);
    CHECK_PX_NEAR(px, W, 37, 2, 128, 128, 0, 255, 2);
    CHECK_PX_NEAR(px, W, 37, 37, 0, 128, 128, 255, 2);
    CHECK_PX_NEAR(px, W, 2, 37, 128, 128, 128, 255, 2);
    /* One px off the diagonal (0.707 px away) is pure. */
    EMB_CHECK_PIXEL(px, W, 3, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 1, 3, 255, 255, 0, 255);
    SDL_free(px);
}

static void scene_rounded_hole(embi_frame *f, void *user)
{
    (void)user;
    /* Outer radius 12, width 4 on every side, so the inner radius is 12 - 4 = 8. The outer
     * top-left arc is centered at (12, 12), and so is the inner one. */
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {12, 12, 12, 12};
    const float widths[4] = {4, 4, 4, 4};
    const uint32_t red[4] = {RED, RED, RED, RED};
    embi_frame_border(f, rect, radii, widths, red, 0, 0);
}

EMB_TEST(border_inner_corner_is_rounded_by_radius_minus_width)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded_hole, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* (4, 4) is inside the inner corner square [4, 12)^2 but outside the inner arc: its center
     * is 10.6 px from (12, 12), and the arc radius is 8. It is border, so it is red. A sharp
     * inner corner (radius 0) would leave it as hole. */
    EMB_CHECK_PIXEL(px, W, 4, 4, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 5, 5, 255, 0, 0, 255);
    /* (7, 7) is 6.4 px from (12, 12), inside the inner arc: hole. */
    EMB_CHECK_PIXEL(px, W, 7, 7, 0, 0, 0, 0);
    /* On the inner arc at 45 degrees, distance 8 from (12, 12), the center (6.5, 6.5) is
     * 7.78 px away: 0.22 px inside the arc, so the border covers 0.278 of the pixel. */
    CHECK_PX_NEAR(px, W, 6, 6, 71, 0, 0, 71, 3);
    /* Straight band and hole away from the corner. */
    EMB_CHECK_PIXEL(px, W, 20, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 20, 0, 0, 0, 0);
    SDL_free(px);
}

/* Partial borders: some sides have zero width. A side with no width has no area, so the diagonal
 * split must never hand its pixels to it, and the other three sides stay empty. The widths are
 * passed through `user` as a float[4]. */

static void scene_widths(embi_frame *f, void *user)
{
    const float *widths = user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_bottom_only_paints_a_blue_band)
{
    float widths[4] = {0, 0, 6, 0};
    uint8_t *px = emb_test_render(W, H, scene_widths, widths);
    EMB_RENDER_OR_SKIP(px);
    /* Band is y in [34, 40), full width. Mid-band and next to each bottom corner. */
    EMB_CHECK_PIXEL(px, W, 20, 37, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 0, 39, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 39, 39, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 1, 35, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 38, 35, 0, 0, 255, 255);
    /* Rows above the band, the sides and the top are empty. */
    EMB_CHECK_PIXEL(px, W, 20, 33, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 33, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 33, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    /* Below the box. */
    EMB_CHECK_PIXEL(px, W, 20, 40, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(border_top_only_paints_a_red_band)
{
    float widths[4] = {4, 0, 0, 0};
    uint8_t *px = emb_test_render(W, H, scene_widths, widths);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 20, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 1, 3, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 38, 3, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 4, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 38, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 38, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 38, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(border_right_only_paints_a_green_band)
{
    float widths[4] = {0, 4, 0, 0};
    uint8_t *px = emb_test_render(W, H, scene_widths, widths);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 0, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 39, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 37, 1, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 37, 38, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 35, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 38, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 38, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(border_left_only_paints_a_yellow_band)
{
    float widths[4] = {0, 0, 0, 4};
    uint8_t *px = emb_test_render(W, H, scene_widths, widths);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 2, 20, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 0, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 39, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 3, 1, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 3, 38, 255, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 5, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 38, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 38, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(border_top_and_bottom_only_leave_the_sides_empty)
{
    float widths[4] = {4, 0, 4, 0};
    uint8_t *px = emb_test_render(W, H, scene_widths, widths);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 20, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 39, 3, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 38, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 0, 39, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 39, 36, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 1, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 4, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 35, 0, 0, 0, 0);
    SDL_free(px);
}

/* A rounded box (radius 12) at (10, 10) with only the bottom border (6 px). The pixels outside the
 * rounded outer corners and outside the box stay empty, including the fringe next to the zero
 * width sides. Outer corner arcs: bottom-left centered (22, 38), bottom-right (38, 38). */
static void scene_rounded_bottom_only(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {10, 10, 40, 40};
    const float radii[4] = {12, 12, 12, 12};
    const float widths[4] = {0, 0, 6, 0};
    embi_frame_border(f, rect, radii, widths, side_colors, 0, 0);
}

EMB_TEST(border_rounded_bottom_only_keeps_fringe_and_corners_empty)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded_bottom_only, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Band y in [44, 50), straight part x in [22, 38). */
    EMB_CHECK_PIXEL(px, W, 30, 47, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 24, 48, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 36, 48, 0, 0, 255, 255);
    /* Outside the rounded corners: (10.5, 49.5) is 16.3 px from (22, 38). */
    EMB_CHECK_PIXEL(px, W, 10, 49, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 49, 49, 0, 0, 0, 0);
    /* Outside the box on every side. */
    EMB_CHECK_PIXEL(px, W, 30, 50, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 51, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 9, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 50, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 9, 0, 0, 0, 0);
    /* The sides, the top and the hole stay empty. */
    EMB_CHECK_PIXEL(px, W, 11, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 48, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 11, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_rounded_unequal(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 40, 40};
    const float radii[4] = {10, 10, 10, 10};
    /* Top 2, right 8, bottom 2, left 8: each inner corner has width 8 on one axis and 2 on the
     * other. The shader approximates the inner radius as 10 - mean(8, 2) = 5, centered (13, 7)
     * for the top-left. The CSS inner corner is an ellipse (rx 2, ry 8); the two agree on the
     * pixels checked here. The elliptical form arrives with R3. */
    const float widths[4] = {2, 8, 2, 8};
    const uint32_t all_red[4] = {RED, RED, RED, RED};
    embi_frame_border(f, rect, radii, widths, all_red, 0, 0);
}

EMB_TEST(border_rounded_with_unequal_widths_places_the_inner_corner)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded_unequal, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Outer corner is cut. */
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    /* Bands: top y in [0, 2), left x in [0, 8), right x in [32, 40), bottom y in [38, 40). */
    EMB_CHECK_PIXEL(px, W, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 4, 20, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 33, 20, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 38, 255, 0, 0, 255);
    /* Just inside each band edge is the hole. */
    EMB_CHECK_PIXEL(px, W, 20, 2, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 8, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 31, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 37, 0, 0, 0, 0);
    /* The inner corner is rounded: the square corner pixel of the hole is painted, while a pixel
     * well inside the rounded corner is hole. */
    EMB_CHECK_PIXEL(px, W, 8, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 31, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 8, 37, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 31, 37, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 12, 6, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
