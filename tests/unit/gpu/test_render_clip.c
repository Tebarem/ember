#include "emb_render_test.h"

#include "render_check.h"

/* Rounded-rect clips: nested intersection, rounded shape, transformed clip, empty clip, and deep
 * chains that the renderer folds (see render.c). The fill that is clipped always covers the
 * whole 64 x 64 target. */

#define W 64
#define H 64

static void scene_nested(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float parent_rect[4] = {0, 0, 32, 64};
    const float child_rect[4] = {0, 0, 64, 32};
    const float full[4] = {0, 0, 64, 64};
    const uint32_t p = embi_frame_clip(f, parent_rect, zero, 0, 0);
    const uint32_t c = embi_frame_clip(f, child_rect, zero, 0, p);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_nested_rects_intersect)
{
    uint8_t *px = emb_test_render(W, H, scene_nested, nullptr);
    EMB_RENDER_OR_SKIP(px);

    /* Inside both clips: x < 32 and y < 32. */
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    /* Inside the child (y < 32) only: x >= 32. */
    EMB_CHECK_PIXEL(px, W, 40, 10, 0, 0, 0, 0);
    /* Inside the parent (x < 32) only: y >= 32. */
    EMB_CHECK_PIXEL(px, W, 10, 40, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 40, 40, 0, 0, 0, 0);
    /* The shared edge at x = 32 is exact: pixel 31 is in, pixel 32 is out. */
    EMB_CHECK_PIXEL(px, W, 31, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 32, 10, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_round_clip(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float disc[4] = {0, 0, 64, 64};
    const float radii[4] = {32, 32, 32, 32};
    const float full[4] = {0, 0, 64, 64};
    const uint32_t c = embi_frame_clip(f, disc, radii, 0, 0);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_rounded_rect_is_circular)
{
    uint8_t *px = emb_test_render(W, H, scene_round_clip, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Circle of radius 32 centered at (32, 32). */
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 32, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 2, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 2, 2, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 62, 62, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_xform_clip(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float m[6] = {1, 0, 0, 1, 20, 0};
    const float rect[4] = {0, 0, 20, 64};
    const float full[4] = {0, 0, 64, 64};
    const uint32_t x = embi_frame_xform(f, m);
    /* The clip lives in its own space, translated 20 px right: device x in [20, 40). */
    const uint32_t c = embi_frame_clip(f, rect, zero, x, 0);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_uses_its_own_transform)
{
    uint8_t *px = emb_test_render(W, H, scene_xform_clip, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 25, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 45, 10, 0, 0, 0, 0);
    SDL_free(px);
}

/* A 24 x 24 clip centered at the origin of its own space, rotated 30 degrees about the device
 * point (32, 32). Pixel centers are mapped back to clip space by hand: local = R^-1 (p - c). */
static void scene_rotated_clip(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float m[6] = {0.8660254f, 0.5f, -0.5f, 0.8660254f, 32, 32};
    const float rect[4] = {-12, -12, 24, 24};
    const float full[4] = {0, 0, 64, 64};
    const uint32_t x = embi_frame_xform(f, m);
    const uint32_t c = embi_frame_clip(f, rect, zero, x, 0);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_rotated_30_degrees_follows_the_rotated_square)
{
    uint8_t *px = emb_test_render(W, H, scene_rotated_clip, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* Inside: center; (40, 36) has local (9.6, -0.35); (23, 27) has local (-9.6, 0.35). */
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 40, 36, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 23, 27, 255, 0, 0, 255);
    /* Outside: (45, 40) has local x 15.9. (43, 43) and (20, 20) are inside the 24 px
     * axis-aligned square around the center but outside the rotated one (local x = +-15.7). */
    EMB_CHECK_PIXEL(px, W, 45, 40, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 43, 43, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 20, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 2, 2, 0, 0, 0, 0);
    SDL_free(px);
}

/* A rounded clip under a 2x scale: rect (4, 4, 10, 10) with radius 4 becomes device (8, 8, 20,
 * 20) with radius 8, so the corner arc is centered (16, 16). */
static void scene_scaled_clip(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {4, 4, 4, 4};
    const float m[6] = {2, 0, 0, 2, 0, 0};
    const float rect[4] = {4, 4, 10, 10};
    const float full[4] = {0, 0, 64, 64};
    const float zero[4] = {0, 0, 0, 0};
    const uint32_t x = embi_frame_xform(f, m);
    const uint32_t c = embi_frame_clip(f, rect, radii, x, 0);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_scaled_2x_scales_rect_and_radius)
{
    uint8_t *px = emb_test_render(W, H, scene_scaled_clip, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 18, 18, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 27, 18, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 8, 18, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 7, 18, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 28, 18, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 18, 28, 0, 0, 0, 0);
    /* The corner pixel is 2.6 px outside the radius 8 arc; (11, 11) is 1.6 px inside it. */
    EMB_CHECK_PIXEL(px, W, 8, 8, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 11, 11, 255, 0, 0, 255);
    SDL_free(px);
}

/* A 9-deep chain (one over the shader limit) whose outermost level is a rounded clip under a 2x
 * scale covering the whole target. The fold may drop it as redundant only when the next level
 * clears its scaled radius. Either way the visible shape is the 40 x 40 square of the inner
 * levels. */
static void scene_scaled_outer_chain(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float rr[4] = {4, 4, 4, 4};
    const float m[6] = {2, 0, 0, 2, 0, 0};
    const float outer[4] = {0, 0, 32, 32};
    const float inner[4] = {10, 10, 40, 40};
    const float big[4] = {0, 0, 64, 64};
    const uint32_t x = embi_frame_xform(f, m);
    uint32_t parent = embi_frame_clip(f, outer, rr, x, 0);
    parent = embi_frame_clip(f, inner, zero, 0, parent);
    for (int i = 2; i < 9; i++) {
        parent = embi_frame_clip(f, big, zero, 0, parent);
    }
    embi_frame_fill(f, big, zero, RGBA(255, 0, 0, 255), 0, parent);
}

EMB_TEST(clip_deep_chain_with_scaled_rounded_outer_level)
{
    uint8_t *px = emb_test_render(W, H, scene_scaled_outer_chain, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 12, 12, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 48, 48, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 9, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 50, 30, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 9, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 50, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_empty_clip(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float empty[4] = {4, 4, 0, 10};
    const float full[4] = {0, 0, 64, 64};
    const uint32_t c = embi_frame_clip(f, empty, zero, 0, 0);
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, c);
}

EMB_TEST(clip_empty_rect_hides_everything)
{
    uint8_t *px = emb_test_render(W, H, scene_empty_clip, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 5, 5, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 32, 32, 0, 0, 0, 0);
    SDL_free(px);
}

/* A chain of `depth` plain rect clips, all under the identity transform. Every level is 64 x 64
 * except the level at index `small`, which is 20 x 20. The fill is clipped by the innermost. */
typedef struct chain_probe {
    uint32_t depth;
    uint32_t small; /* index of the 20 x 20 level, 0 = outermost */
} chain_probe;

static void scene_chain(embi_frame *f, void *user)
{
    const chain_probe *p = user;
    const float zero[4] = {0, 0, 0, 0};
    const float big[4] = {0, 0, 64, 64};
    const float small[4] = {0, 0, 20, 20};
    const float full[4] = {0, 0, 64, 64};
    uint32_t parent = 0;
    for (uint32_t i = 0; i < p->depth; i++) {
        parent = embi_frame_clip(f, (i == p->small) ? small : big, zero, 0, parent);
    }
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, parent);
}

EMB_TEST(clip_chain_of_eight_applies_every_level)
{
    chain_probe p = {.depth = 8, .small = 7};
    uint8_t *px = emb_test_render(W, H, scene_chain, &p);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 0, 0);
    SDL_free(px);
}

/* Deeper than the shader walks. The outermost level is the small one, so the fold has to keep
 * it exactly: nothing may show outside 20 x 20. */
EMB_TEST(clip_chain_deeper_than_limit_keeps_small_outer_clip)
{
    chain_probe p = {.depth = 9, .small = 0};
    uint8_t *px = emb_test_render(W, H, scene_chain, &p);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 19, 19, 255, 0, 0, 255);
    /* Inside the big inner levels but outside the small outer one: must not leak. */
    EMB_CHECK_PIXEL(px, W, 25, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 25, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(clip_chain_twelve_deep_keeps_small_outer_clip)
{
    chain_probe p = {.depth = 12, .small = 0};
    uint8_t *px = emb_test_render(W, H, scene_chain, &p);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 25, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 25, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(clip_chain_deeper_than_limit_keeps_small_inner_clip)
{
    chain_probe p = {.depth = 9, .small = 8};
    uint8_t *px = emb_test_render(W, H, scene_chain, &p);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 10, 10, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 30, 30, 0, 0, 0, 0);
    SDL_free(px);
}

/* Nine circular clips, each 64 x 64 with radius 32. No two adjacent levels merge exactly, so the
 * fold uses its conservative path for the outer pair. The deeper copied levels keep their
 * circles, so the corner stays cut. The center stays drawn. */
static void scene_round_chain(embi_frame *f, void *user)
{
    const chain_probe *p = user;
    const float zero[4] = {0, 0, 0, 0};
    const float disc[4] = {0, 0, 64, 64};
    const float radii[4] = {32, 32, 32, 32};
    const float full[4] = {0, 0, 64, 64};
    uint32_t parent = 0;
    for (uint32_t i = 0; i < p->depth; i++) {
        parent = embi_frame_clip(f, disc, radii, 0, parent);
    }
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, parent);
}

EMB_TEST(clip_deep_rounded_chain_keeps_its_round_corners)
{
    chain_probe p = {.depth = 9, .small = 0};
    uint8_t *px = emb_test_render(W, H, scene_round_chain, &p);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 62, 62, 0, 0, 0, 0);
    SDL_free(px);
}

/* Nine levels: a big rounded outer clip, then a small circle, then seven plain full-size rects. The
 * outer level is redundant under the circle (the circle lies far inside its rounded corners), so
 * the fold drops it and keeps the circle round. Merging the outer pair into a bounding box instead
 * would turn the circle into a square. */
static void scene_nested_container(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float outer[4] = {0, 0, 64, 64};
    const float outer_r[4] = {16, 16, 16, 16};
    const float disc[4] = {24, 24, 16, 16};
    const float disc_r[4] = {8, 8, 8, 8};
    const float full[4] = {0, 0, 64, 64};
    uint32_t parent = embi_frame_clip(f, outer, outer_r, 0, 0);
    parent = embi_frame_clip(f, disc, disc_r, 0, parent);
    for (int i = 0; i < 7; i++) {
        parent = embi_frame_clip(f, full, zero, 0, parent);
    }
    embi_frame_fill(f, full, zero, RGBA(255, 0, 0, 255), 0, parent);
}

EMB_TEST(clip_fold_drops_a_redundant_rounded_outer_level_exactly)
{
    uint8_t *px = emb_test_render(W, H, scene_nested_container, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    /* Inside the disc's bounding square, outside the circle: (25.5, 25.5) is 9.2 px from the
     * center and the radius is 8. */
    EMB_CHECK_PIXEL(px, W, 25, 25, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 38, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 10, 10, 0, 0, 0, 0);
    SDL_free(px);
}

/* Many siblings under one depth-8 parent. Each must clip to its own rect, whether or not the
 * fold result is reused. */
static void scene_siblings(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float big[4] = {0, 0, 64, 64};
    uint32_t parent = 0;
    for (int i = 0; i < 8; i++) {
        parent = embi_frame_clip(f, big, zero, 0, parent);
    }
    for (int i = 0; i < 16; i++) {
        /* Sibling i covers the 4 x 4 cell at column i (16 columns of 4 px) in the top row. */
        const float cell[4] = {(float)(i * 4), 0, 4, 4};
        const uint32_t c = embi_frame_clip(f, cell, zero, 0, parent);
        const float row[4] = {(float)(i * 4), 0, 64, 64};
        embi_frame_fill(f, row, zero, (i & 1) ? RGBA(0, 0, 255, 255) : RGBA(255, 0, 0, 255), 0, c);
    }
}

EMB_TEST(clip_siblings_under_a_deep_parent_each_clip_correctly)
{
    uint8_t *px = emb_test_render(W, H, scene_siblings, nullptr);
    EMB_RENDER_OR_SKIP(px);
    for (int i = 0; i < 16; i++) {
        if (i & 1) {
            EMB_CHECK_PIXEL(px, W, i * 4 + 1, 1, 0, 0, 255, 255);
        } else {
            EMB_CHECK_PIXEL(px, W, i * 4 + 1, 1, 255, 0, 0, 255);
        }
        EMB_CHECK_PIXEL(px, W, i * 4 + 1, 6, 0, 0, 0, 0);
    }
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
