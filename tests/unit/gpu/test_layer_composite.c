#include "layer_rig.h"

/* Layer composite (embi_frame_layer). Each test renders a 32 x 32 layer and composites it into a
 * 64 x 64 target, with the layer frame and the composite recorded into one command buffer, the
 * way a real frame sequence runs. The layer content is a red rect over texels x < 16 (opaque),
 * and a half-transparent blue 8 x 8 square at (24, 0), which is premultiplied to (0, 0, 128, 128).
 * Expected values are computed by hand from the premultiplied over operator. */

#define TW 64u
#define RED RGBA(255, 0, 0, 255)
#define ZERO_RADII ((float[4]){0.0f, 0.0f, 0.0f, 0.0f})

static void fill(embi_frame *f, float x, float y, float w, float h, uint32_t rgba)
{
    const float r[4] = {x, y, w, h};
    const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_fill(f, r, z, rgba, 0, 0);
}

/* Records the layer content into a fresh layer of the rig's pool. */
static bool make_layer(layer_rig *k, SDL_GPUCommandBuffer *cmd, embi_layer *L)
{
    if (!embi_layer_acquire(k->pool, 32, 32, L)) {
        return false;
    }
    embi_frame *lf = embi_frame_begin(k->r, L->tex_w, L->tex_h, 1.0f);
    if (lf == nullptr) {
        return false;
    }
    fill(lf, 0, 0, 16, 32, RED);
    fill(lf, 24, 0, 8, 8, RGBA(0, 0, 255, 128));
    embi_frame_end(lf, cmd, L->tex, true, 0);
    return true;
}

EMB_TEST(layer_identity_composite_copies_the_texels)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *target = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(target != nullptr && cmd != nullptr);
    embi_layer L = {0};
    EMB_REQUIRE(make_layer(&k, cmd, &L));

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float src[4] = {0, 0, 32, 32};
    const float dst[4] = {0, 0, 32, 32};
    embi_frame_layer(f, &L, src, dst, ZERO_RADII, 1.0f, 0, 0);
    embi_frame_end(f, cmd, target, true, 0);

    layer_dl d = layer_rig_download(&k, cmd, target, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, TW, 5, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, TW, 15, 31, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, TW, 20, 5, 0, 0, 0, 0);
    /* Premultiplied blue at alpha 128 comes through unchanged. */
    CHECK_PX_NEAR(px, TW, 28, 4, 0, 0, 128, 128, 1);
    EMB_CHECK_PIXEL(px, TW, 40, 40, 0, 0, 0, 0);
    SDL_free(px);

    embi_layer_release(k.pool, &L);
    SDL_ReleaseGPUTexture(k.dev, target);
    layer_rig_close(&k);
}

EMB_TEST(layer_opacity_blends_over_what_is_below)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *target = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(target != nullptr && cmd != nullptr);
    embi_layer L = {0};
    EMB_REQUIRE(make_layer(&k, cmd, &L));

    /* Blue under everything, then the layer at half opacity without clearing. */
    layer_rig_clear(&k, cmd, target, TW, TW, RGBA(0, 0, 255, 255));
    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float src[4] = {0, 0, 32, 32};
    const float dst[4] = {0, 0, 32, 32};
    embi_frame_layer(f, &L, src, dst, ZERO_RADII, 0.5f, 0, 0);
    embi_frame_end(f, cmd, target, false, 0);

    layer_dl d = layer_rig_download(&k, cmd, target, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    /* Red at 0.5 (128/255 after quantization): 0.50196 red over blue, so (128, 0, 127). */
    CHECK_PX_NEAR(px, TW, 5, 5, 128, 0, 127, 255, 2);
    /* Transparent texels leave the blue alone, inside the layer's rect and outside it. */
    EMB_CHECK_PIXEL(px, TW, 20, 5, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, TW, 50, 50, 0, 0, 255, 255);
    SDL_free(px);

    embi_layer_release(k.pool, &L);
    SDL_ReleaseGPUTexture(k.dev, target);
    layer_rig_close(&k);
}

EMB_TEST(layer_translated_composite_maps_texels_to_the_offset)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *target = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(target != nullptr && cmd != nullptr);
    embi_layer L = {0};
    EMB_REQUIRE(make_layer(&k, cmd, &L));

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float src[4] = {0, 0, 32, 32};
    const float dst[4] = {10, 5, 32, 32};
    embi_frame_layer(f, &L, src, dst, ZERO_RADII, 1.0f, 0, 0);
    embi_frame_end(f, cmd, target, true, 0);

    layer_dl d = layer_rig_download(&k, cmd, target, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    /* Texel (x - 10, y - 5). Texel 15 is red and texel 17 is transparent (the red ends at 16). */
    EMB_CHECK_PIXEL(px, TW, 25, 6, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, TW, 27, 6, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 9, 6, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 11, 4, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 11, 6, 255, 0, 0, 255);
    SDL_free(px);

    embi_layer_release(k.pool, &L);
    SDL_ReleaseGPUTexture(k.dev, target);
    layer_rig_close(&k);
}

EMB_TEST(layer_scaled_composite_samples_between_texels)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *target = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(target != nullptr && cmd != nullptr);
    embi_layer L = {0};
    EMB_REQUIRE(make_layer(&k, cmd, &L));

    /* The layer's top-left 32 x 16 texels, drawn at 2x: local (0, 0, 32, 16) covers device
     * (0, 0, 64, 32). Red is local x < 16, which is device x < 32. */
    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float m[6] = {2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    const uint32_t xf = embi_frame_xform(f, m);
    const float src[4] = {0, 0, 32, 16};
    const float dst[4] = {0, 0, 32, 16};
    embi_frame_layer(f, &L, src, dst, ZERO_RADII, 1.0f, xf, 0);
    embi_frame_end(f, cmd, target, true, 0);

    layer_dl d = layer_rig_download(&k, cmd, target, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, TW, 4, 4, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, TW, 20, 4, 255, 0, 0, 255);
    /* Device x 40 is texel 20: transparent. Device y 40 is below the 32 px tall destination. */
    EMB_CHECK_PIXEL(px, TW, 40, 4, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, TW, 4, 40, 0, 0, 0, 0);
    SDL_free(px);

    embi_layer_release(k.pool, &L);
    SDL_ReleaseGPUTexture(k.dev, target);
    layer_rig_close(&k);
}

EMB_TEST(layer_composite_is_clipped_by_a_rounded_clip)
{
    layer_rig k;
    if (!layer_rig_open(&k)) {
        EMB_SKIP("no GPU device");
        return;
    }
    SDL_GPUTexture *target = layer_rig_target(&k, TW, TW);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
    EMB_REQUIRE(target != nullptr && cmd != nullptr);
    embi_layer L = {0};
    EMB_REQUIRE(make_layer(&k, cmd, &L));

    embi_frame *f = embi_frame_begin(k.r, TW, TW, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float clip_rect[4] = {0, 0, 32, 32};
    const float clip_radii[4] = {12, 12, 12, 12};
    const uint32_t clip = embi_frame_clip(f, clip_rect, clip_radii, 0, 0);
    const float src[4] = {0, 0, 32, 32};
    const float dst[4] = {0, 0, 32, 32};
    embi_frame_layer(f, &L, src, dst, ZERO_RADII, 1.0f, 0, clip);
    embi_frame_end(f, cmd, target, true, 0);

    layer_dl d = layer_rig_download(&k, cmd, target, TW, TW);
    EMB_REQUIRE(layer_rig_submit(&k, cmd));
    uint8_t *px = layer_rig_map(&k, d);
    EMB_REQUIRE(px != nullptr);
    /* The corner arc is centered at (12, 12): (0, 0) is 16 px from it and outside the clip. */
    EMB_CHECK_PIXEL(px, TW, 0, 0, 0, 0, 0, 0);
    /* Straight edges are not clipped. */
    EMB_CHECK_PIXEL(px, TW, 2, 16, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, TW, 8, 16, 255, 0, 0, 255);
    SDL_free(px);

    embi_layer_release(k.pool, &L);
    SDL_ReleaseGPUTexture(k.dev, target);
    layer_rig_close(&k);
}

EMB_RENDER_TEST_MAIN()
