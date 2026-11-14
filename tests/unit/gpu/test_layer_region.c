#include "layer_rig.h"

/* Partial redraw (embi_frame_begin_region): a redraw of a damage rect must produce the same pixels
 * as a full frame in that rect, keep everything else, cull prims that cannot reach the damage,
 * and clear the damage (with a replacing clear, so alpha 0 really clears). */

#define TW 64u
#define RED RGBA(255, 0, 0, 255)
#define GREEN RGBA(0, 255, 0, 255)
#define WHITE RGBA(255, 255, 255, 255)
#define BLUE RGBA(0, 0, 255, 255)

static void fill(embi_frame *f, float x, float y, float w, float h, uint32_t rgba)
{
    const float r[4] = {x, y, w, h};
    const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_fill(f, r, z, rgba, 0, 0);
}

/* Scene S0: a red square. S1 adds a green square inside the damage (25, 25, 15, 15). Both are
 * given in local px; the scale maps them to device px. */
static void scene_s0(embi_frame *f)
{
    fill(f, 0, 0, 40, 40, RED);
}

static void scene_s1(embi_frame *f)
{
    fill(f, 0, 0, 40, 40, RED);
    fill(f, 25, 25, 15, 15, GREEN);
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    return SDL_memcmp(a, b, n) == 0;
}

/* The equivalence check at one device scale. The damage is the device rect of the green square.
 * T gets S0 as a full frame, then the redraw of S1 in the damage with clear; R gets S1 as a full
 * frame. The two must match at every pixel. */
static void check_region_equals_full(layer_rig *k, float scale, embi_dev_rect damage)
{
    SDL_GPUTexture *t = layer_rig_target(k, TW, TW);
    SDL_GPUTexture *r = layer_rig_target(k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k->dev);
    EMB_REQUIRE(t != nullptr && r != nullptr && cmd != nullptr);

    embi_frame *f = embi_frame_begin(k->r, TW, TW, scale);
    EMB_REQUIRE(f != nullptr);
    scene_s0(f);
    embi_frame_end(f, cmd, t, true, WHITE);

    f = embi_frame_begin_region(k->r, TW, TW, scale, damage);
    EMB_REQUIRE(f != nullptr);
    scene_s1(f);
    embi_frame_end(f, cmd, t, true, WHITE);

    f = embi_frame_begin(k->r, TW, TW, scale);
    EMB_REQUIRE(f != nullptr);
    scene_s1(f);
    embi_frame_end(f, cmd, r, true, WHITE);

    layer_dl dt = layer_rig_download(k, cmd, t, TW, TW);
    layer_dl dr = layer_rig_download(k, cmd, r, TW, TW);
    EMB_REQUIRE(layer_rig_submit(k, cmd));
    uint8_t *pt = layer_rig_map(k, dt);
    uint8_t *pr = layer_rig_map(k, dr);
    EMB_REQUIRE(pt != nullptr && pr != nullptr);
    EMB_CHECK(bytes_equal(pt, pr, TW * TW * 4u));
    if (scale == 1.0f) {
        /* The green square is in the damage, and the red one outside it stays red. */
        EMB_CHECK_PIXEL(pt, TW, 30, 30, 0, 255, 0, 255);
        EMB_CHECK_PIXEL(pt, TW, 10, 10, 255, 0, 0, 255);
        EMB_CHECK_PIXEL(pt, TW, 50, 50, 255, 255, 255, 255);
    }
    SDL_free(pt);
    SDL_free(pr);
    SDL_ReleaseGPUTexture(k->dev, t);
    SDL_ReleaseGPUTexture(k->dev, r);
}

EMB_TEST(layer_region_redraw_equals_the_full_frame)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const embi_dev_rect damage = {25, 25, 15, 15};
    check_region_equals_full(&k, 1.0f, damage);
    layer_rig_close(&k);
}

EMB_TEST(layer_region_redraw_equals_the_full_frame_at_scale_two)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    /* At scale 2 the green square is device (50, 50, 30, 30). The damage is a little larger than
     * it, so that the margin around the prim is exercised too. */
    const embi_dev_rect damage = {46, 46, 38, 38};
    check_region_equals_full(&k, 2.0f, damage);
    layer_rig_close(&k);
}

EMB_TEST(layer_region_without_clear_keeps_what_is_outside_and_paints_over_the_rest)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *t = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(t != nullptr && cmd != nullptr);

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    scene_s0(f);
    embi_frame_end(f, cmd, t, true, WHITE);

    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, (embi_dev_rect){25, 25, 15, 15});
    EMB_REQUIRE(f != nullptr);
    fill(f, 25, 25, 15, 15, GREEN);
    embi_frame_end(f, cmd, t, false, 0);

    layer_dl d = layer_rig_download(&k, cmd, t, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, TW, 30, 30, 0, 255, 0, 255);  /* painted over the red */
    EMB_CHECK_PIXEL(px, TW, 20, 20, 255, 0, 0, 255);  /* above the damage: red kept */
    EMB_CHECK_PIXEL(px, TW, 30, 22, 255, 0, 0, 255);  /* above the damage, inside the square */
    EMB_CHECK_PIXEL(px, TW, 50, 30, 255, 255, 255, 255); /* outside the damage: white kept */
    SDL_free(px);

    SDL_ReleaseGPUTexture(k.dev, t);
    layer_rig_close(&k);
}

EMB_TEST(layer_region_culls_prims_that_cannot_reach_the_damage)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *t = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(t != nullptr && cmd != nullptr);
    const embi_dev_rect damage = {50, 50, 10, 10};

    /* A square far from the damage: culled, so nothing is drawn. */
    embi_frame *f = embi_frame_begin_region(k.r, TW, TW, 1.0f, damage);
    EMB_REQUIRE(f != nullptr);
    fill(f, 0, 0, 20, 20, RED);
    embi_frame_end(f, cmd, t, false, 0);
    EMB_CHECK_EQ_INT((int)embi_renderer_last_draw_calls(k.r), 0);

    /* A square that reaches into the damage: one draw. */
    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, damage);
    EMB_REQUIRE(f != nullptr);
    fill(f, 45, 45, 20, 20, RED);
    embi_frame_end(f, cmd, t, false, 0);
    EMB_CHECK_EQ_INT((int)embi_renderer_last_draw_calls(k.r), 1);

    /* With a clear, the clear itself is the one draw. */
    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, damage);
    EMB_REQUIRE(f != nullptr);
    fill(f, 0, 0, 20, 20, RED);
    embi_frame_end(f, cmd, t, true, WHITE);
    EMB_CHECK_EQ_INT((int)embi_renderer_last_draw_calls(k.r), 1);

    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    SDL_ReleaseGPUTexture(k.dev, t);
    layer_rig_close(&k);
}

EMB_TEST(layer_region_clear_replaces_the_damage_including_alpha)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *t = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(t != nullptr && cmd != nullptr);

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    scene_s0(f);
    embi_frame_end(f, cmd, t, true, WHITE);

    /* A transparent clear over the damage replaces the opaque pixels, which blending could not. */
    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, (embi_dev_rect){10, 10, 10, 10});
    EMB_REQUIRE(f != nullptr);
    embi_frame_end(f, cmd, t, true, 0);

    layer_dl d = layer_rig_download(&k, cmd, t, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, TW, 12, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 19, 19, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 9, 9, 255, 0, 0, 255);   /* just outside: red kept */
    EMB_CHECK_PIXEL(px, TW, 20, 20, 255, 0, 0, 255);     /* inside the red, past the damage */
    EMB_CHECK_PIXEL(px, TW, 45, 45, 255, 255, 255, 255); /* outside the red: the white clear */
    SDL_free(px);

    SDL_ReleaseGPUTexture(k.dev, t);
    layer_rig_close(&k);
}

EMB_TEST(layer_region_empty_or_clipped_damage)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *t = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(t != nullptr && cmd != nullptr);

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    embi_frame_end(f, cmd, t, true, WHITE);

    /* Empty damage: no draws, and the target stays white. */
    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, (embi_dev_rect){10, 10, 0, 0});
    EMB_REQUIRE(f != nullptr);
    embi_frame_end(f, cmd, t, true, BLUE);
    EMB_CHECK_EQ_INT((int)embi_renderer_last_draw_calls(k.r), 0);

    /* Damage that runs past the target's corner is clipped to it. */
    f = embi_frame_begin_region(k.r, TW, TW, 1.0f, (embi_dev_rect){60, 60, 20, 20});
    EMB_REQUIRE(f != nullptr);
    embi_frame_end(f, cmd, t, true, BLUE);

    layer_dl d = layer_rig_download(&k, cmd, t, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, TW, 12, 12, 255, 255, 255, 255);
    EMB_CHECK_PIXEL(px, TW, 63, 63, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, TW, 59, 59, 255, 255, 255, 255);
    SDL_free(px);

    SDL_ReleaseGPUTexture(k.dev, t);
    layer_rig_close(&k);
}

EMB_RENDER_TEST_MAIN()
