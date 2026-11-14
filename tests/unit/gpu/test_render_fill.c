#include "emb_render_test.h"

#include "render_check.h"

/* Solid fills: exact edges, rounded corners, premultiplied blending, anti-aliased edges, and
 * the inputs that must draw nothing. Every expected value below is computed by hand: pixel
 * centers are at (x + 0.5, y + 0.5), and coverage follows from the signed distance. */

#define W 64
#define H 64

static void scene_rect_edges(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {8, 8, 16, 16};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_axis_aligned_edges_are_exact)
{
    uint8_t *px = emb_test_render(W, H, scene_rect_edges, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Inside [8, 24) x [8, 24): both corners and the middle are opaque red. */
    EMB_CHECK_PIXEL(px, W, 8, 8, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 12, 12, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 23, 23, 255, 0, 0, 255);
    /* One pixel outside each edge: transparent black. */
    EMB_CHECK_PIXEL(px, W, 7, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 24, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 12, 7, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 12, 24, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 63, 63, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_rounded(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 32, 32};
    const float radii[4] = {10, 10, 10, 10};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_rounded_corners_clip_the_corner_only)
{
    uint8_t *px = emb_test_render(W, H, scene_rounded, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Corner arc centered at (10, 10), radius 10. The pixel at (0, 0) is 3.4 px outside. */
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    /* (2, 2): center (2.5, 2.5) is 0.6 px outside the arc. */
    EMB_CHECK_PIXEL(px, W, 2, 2, 0, 0, 0, 0);
    /* Inside the arc, and in the straight-edge zones. */
    EMB_CHECK_PIXEL(px, W, 5, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 16, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 16, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    /* Bottom-right corner, arc centered at (22, 22). */
    EMB_CHECK_PIXEL(px, W, 31, 31, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 21, 21, 255, 0, 0, 255);
    SDL_free(px);
}

static void scene_blend(embi_frame *f, void *user)
{
    (void)user;
    const float full[4] = {0, 0, 64, 64};
    const float part[4] = {16, 16, 32, 32};
    const float zero[4] = {0, 0, 0, 0};
    embi_frame_fill(f, full, zero, RGBA(0, 0, 255, 255), 0, 0);
    /* Half-transparent red over opaque blue, premultiplied source-over. */
    embi_frame_fill(f, part, zero, RGBA(255, 0, 0, 128), 0, 0);
}

EMB_TEST(fill_alpha_blends_premultiplied_over_destination)
{
    uint8_t *px = emb_test_render(W, H, scene_blend, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* src = (128/255, 0, 0, 128/255), dst = (0, 0, 1, 1):
     * out.r = 128/255 * 255 = 128, out.b = 1 - 128/255 = 127/255 * 255 = 127, out.a = 255. */
    EMB_CHECK_PIXEL(px, W, 30, 30, 128, 0, 127, 255);
    /* Outside the half-transparent rect, the blue fill is untouched. */
    EMB_CHECK_PIXEL(px, W, 5, 5, 0, 0, 255, 255);
    SDL_free(px);
}

static void scene_half_edge(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {10.5f, 10, 10, 10};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_fractional_edge_has_half_coverage)
{
    uint8_t *px = emb_test_render(W, H, scene_half_edge, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Pixel 10 has its center at x = 10.5, exactly on the left edge: half coverage. */
    CHECK_PX_NEAR(px, W, 10, 12, 128, 0, 0, 128, 2);
    /* Pixel 9 is fully outside, pixel 11 fully inside. */
    EMB_CHECK_PIXEL(px, W, 9, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 11, 12, 255, 0, 0, 255);
    /* Right edge at x = 20.5: pixel 20 is half covered, pixel 21 is outside. */
    CHECK_PX_NEAR(px, W, 20, 12, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 21, 12, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_nothing(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 64, 64};
    const float zero[4] = {0, 0, 0, 0};
    const float nan_rect[4] = {__builtin_nanf(""), 0, 10, 10};
    const float empty[4] = {4, 4, 0, 10};
    /* Zero alpha, empty size, and a non-finite rect all draw nothing. */
    embi_frame_fill(f, rect, zero, RGBA(255, 0, 0, 0), 0, 0);
    embi_frame_fill(f, empty, zero, RGBA(255, 0, 0, 255), 0, 0);
    embi_frame_fill(f, nan_rect, zero, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_degenerate_inputs_draw_nothing)
{
    uint8_t *px = emb_test_render(W, H, scene_nothing, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 5, 5, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_radius_overflow(embi_frame *f, void *user)
{
    (void)user;
    /* 30 + 30 > 20 on the top side, so CSS scales every radius by 20/60 to a radius of 10. */
    const float rect[4] = {0, 0, 20, 20};
    const float radii[4] = {30, 30, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(0, 255, 0, 255), 0, 0);
}

EMB_TEST(fill_radii_overflow_scales_all_corners)
{
    uint8_t *px = emb_test_render(W, H, scene_radius_overflow, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* The top side needs 30 + 30 = 60 px but has 20, so all radii scale by 1/3 to 10 px. The
     * top-left arc is centered at (10, 10): the corner pixel is outside it, and (10, 10) is
     * inside the box's straight part. */
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 255, 0, 255);
    SDL_free(px);
}

/* CSS corner overlap only limits adjacent radii to sum to at most the side, so one corner may be
 * larger than half the shorter side. 60 x 30 with {20, 0, 0, 0}: the top-left arc is centered
 * (20, 20) with radius 20, and the other three corners are square. */
static void scene_one_big_radius(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 60, 30};
    const float radii[4] = {20, 0, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_single_radius_over_half_the_short_side_is_honored)
{
    uint8_t *px = emb_test_render(W, H, scene_one_big_radius, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* (1, 1) and (4, 4) are outside the arc. (2, 8) is 0.94 px outside it (a radius clamped to 15
     * would draw it). (6, 6) and (1, 20) are inside. */
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 4, 4, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 2, 8, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 6, 6, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 1, 20, 255, 0, 0, 255);
    /* Square corners at the other three. */
    EMB_CHECK_PIXEL(px, W, 0, 29, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 59, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 59, 29, 255, 0, 0, 255);
    /* Outside the box. */
    EMB_CHECK_PIXEL(px, W, 0, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 60, 10, 0, 0, 0, 0);
    SDL_free(px);
}

/* `border-radius: 50px 10px 0 0` on a 64 px box: a 50 px top-left arc centered (50, 50), a 10 px
 * top-right arc centered (54, 10), and square bottom corners. */
static void scene_mixed_radii(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 64, 64};
    const float radii[4] = {50, 10, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(fill_mixed_radii_follow_css)
{
    uint8_t *px = emb_test_render(W, H, scene_mixed_radii, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    /* (5, 25) is 0.8 px outside the 50 px arc. A clamped 32 px radius would draw it. */
    EMB_CHECK_PIXEL(px, W, 5, 25, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 8, 25, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 20, 20, 255, 0, 0, 255);
    /* Top-right: the corner pixel is 3.4 px outside the small arc. */
    EMB_CHECK_PIXEL(px, W, 63, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 54, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 0, 63, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 63, 63, 255, 0, 0, 255);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
