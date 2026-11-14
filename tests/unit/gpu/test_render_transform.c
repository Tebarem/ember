#include "emb_render_test.h"

#include "render_check.h"

/* Transforms: translation, a 30 degree rotation (with a rotated edge sampled for anti-aliasing),
 * non-uniform scale (edges must stay one device px wide along each axis), and degenerate
 * scale. Matrices use CSS order (a, b, c, d, e, f): x' = a*x + c*y + e, y' = b*x + d*y + f.
 * Index dedup is tested in test_render_frame.c, because it needs a frame but no scene. */

#define W 64
#define H 64

#define COS30 0.8660254f
#define SIN30 0.5f

static void scene_translate(embi_frame *f, void *user)
{
    (void)user;
    const float m[6] = {1, 0, 0, 1, 20, 0};
    const float rect[4] = {0, 0, 10, 10};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), x, 0);
}

EMB_TEST(transform_translation_moves_the_fill)
{
    uint8_t *px = emb_test_render(W, H, scene_translate, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* The fill covers device x in [20, 30), y in [0, 10). */
    EMB_CHECK_PIXEL(px, W, 25, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 5, 5, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 35, 5, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_rotate(embi_frame *f, void *user)
{
    (void)user;
    /* Rotate 30 degrees clockwise on screen (y down), then move the origin to (32, 32). The
     * fill is a 20 x 20 square centered on the origin in local space. */
    const float m[6] = {COS30, SIN30, -SIN30, COS30, 32, 32};
    const float rect[4] = {-10, -10, 20, 20};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), x, 0);
}

EMB_TEST(transform_rotate_30_degrees)
{
    uint8_t *px = emb_test_render(W, H, scene_rotate, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* The center maps to device (32, 32). */
    EMB_CHECK_PIXEL(px, W, 31, 31, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    /* Local (9, 0) is inside the square, 1 px from its edge. Device pixel (39, 36) holds the
     * point (39.5, 36.5); its local coordinates are about (8.75, 0.15). */
    EMB_CHECK_PIXEL(px, W, 39, 36, 255, 0, 0, 255);
    /* Device pixel (42, 38) holds (42.5, 38.5), local about (12.3, 0.4): outside. */
    EMB_CHECK_PIXEL(px, W, 42, 38, 0, 0, 0, 0);
    /* (42, 42) is inside the axis-aligned bounding box of the rotated square, but outside it. */
    EMB_CHECK_PIXEL(px, W, 42, 42, 0, 0, 0, 0);
    /* Device pixel (38, 42) holds (38.5, 42.5), local about (10.9, 5.8): outside. */
    EMB_CHECK_PIXEL(px, W, 38, 42, 0, 0, 0, 0);
    /* Rotated edge. The right edge (local x = 10) is 1 px wide in device space, and pixel
     * (40, 36) has its center (40.5, 36.5) 0.389 px inside it along the device normal (0.866,
     * 0.5). Coverage is 0.5 + 0.389 = 0.889, so the premultiplied red is 227 (of 255). */
    CHECK_PX_NEAR(px, W, 40, 36, 227, 0, 0, 227, 3);
    SDL_free(px);
}

static void scene_scale_y(embi_frame *f, void *user)
{
    (void)user;
    /* scale(1, 0.1) with the origin moved to (10, 20). The box is 100 local px wide and tall, so
     * it is 100 device px wide and 10 device px tall. The left edge sits at local x = 0.5, so
     * device x = 10.5 (a half-covered pixel column). The top edge is at device y = 20. */
    const float m[6] = {1, 0, 0, 0.1f, 10, 20};
    const float rect[4] = {0.5f, 0, 100, 100};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), x, 0);
}

/* A vertical edge under a 0.1 vertical scale must still be one device px wide. The old
 * sigma_min form scaled the AA width by 0.1 along x and smeared it over about 10 px. The
 * horizontal edges are the ones that smear over one device px (in local units, 10 px). */
EMB_TEST(transform_non_uniform_scale_keeps_vertical_edges_crisp)
{
    uint8_t *px = emb_test_render(128, 64, scene_scale_y, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Left edge at device x = 10.5: pixel 10 is half covered, 9 is empty, 11 is full. */
    CHECK_PX_NEAR(px, 128, 10, 25, 128, 0, 0, 128, 3);
    EMB_CHECK_PIXEL(px, 128, 9, 25, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 128, 11, 25, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 128, 12, 25, 255, 0, 0, 255);
    /* Right edge at device x = 110.5: pixel 110 is half covered, 111 is empty, 109 is full. */
    CHECK_PX_NEAR(px, 128, 110, 25, 128, 0, 0, 128, 3);
    EMB_CHECK_PIXEL(px, 128, 111, 25, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 128, 109, 25, 255, 0, 0, 255);
    /* Top edge at device y = 20, bottom at y = 30: whole pixel rows, no smear. */
    EMB_CHECK_PIXEL(px, 128, 60, 19, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 128, 60, 20, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 128, 60, 29, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 128, 60, 30, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_degenerate(embi_frame *f, void *user)
{
    (void)user;
    const float m[6] = {0, 0, 0, 0, 0, 0};
    const float rect[4] = {0, 0, 64, 64};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    /* A zero scale collapses the box to nothing: the fill is skipped. */
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), x, 0);
}

EMB_TEST(transform_zero_scale_hides_content)
{
    uint8_t *px = emb_test_render(W, H, scene_degenerate, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 0, 0);
    SDL_free(px);
}

/* A near-singular skew: the columns (1, 1) and (1, 1 + 1e-5) are almost parallel, so the ideal pad
 * is about 1.4e5 local px. The pad is capped, so the quad stays near the viewport and the draw
 * stays correct elsewhere: a pixel far from the squashed line stays empty. */
static void scene_near_singular(embi_frame *f, void *user)
{
    (void)user;
    const float m[6] = {1, 1, 1, 1.00001f, 0, 0};
    const float rect[4] = {10, 10, 20, 20};
    const float radii[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), x, 0);
    const float plain[4] = {50, 50, 8, 8};
    embi_frame_fill(f, plain, radii, RGBA(0, 0, 255, 255), 0, 0);
}

EMB_TEST(transform_near_singular_skew_stays_bounded)
{
    uint8_t *px = emb_test_render(W, H, scene_near_singular, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Nothing near the top-right or bottom-left corners; the later plain fill is intact. */
    EMB_CHECK_PIXEL(px, W, 60, 2, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 2, 60, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 54, 54, 0, 0, 255, 255);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
