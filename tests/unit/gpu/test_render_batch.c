#include "emb_render_test.h"

#include "render_check.h"

#include "base/base.h"
#include "gpu/atlas.h"
#include "gpu/path.h"

/* Batching (shaders/box.slang, gpu/render.c): a frame of box, outline and image prims in paint order
 * is one draw, images on several atlas pages included, and the atlas array keeps the pixels of its
 * earlier pages when it grows. Every expected pixel is hand-computed: a pixel center is (x + 0.5,
 * y + 0.5). Needs a GPU device; without one the tests are skipped.
 *
 * This file has its own rig, because each image's upload must be recorded in the frame's command
 * buffer, before embi_frame_end. */

typedef struct brig {
    embi_gpu *g;
    embi_renderer *r;
    embi_atlas *atlas;
    SDL_GPUDevice *dev;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *tb;
    uint32_t w;
    uint32_t h;
} brig;

static bool brig_open(brig *rg, uint32_t w, uint32_t h)
{
    SDL_memset(rg, 0, sizeof *rg);
    rg->w = w;
    rg->h = h;
    rg->g = embi_gpu_create(nullptr, false);
    if (rg->g == nullptr) {
        return false;
    }
    rg->dev = embi_gpu_device(rg->g);
    rg->r = embi_renderer_create(rg->g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    rg->atlas = embi_atlas_create(rg->dev);
    rg->tex = SDL_CreateGPUTexture(
        rg->dev, &(SDL_GPUTextureCreateInfo){
                     .type = SDL_GPU_TEXTURETYPE_2D,
                     .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                     .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                     .width = w,
                     .height = h,
                     .layer_count_or_depth = 1,
                     .num_levels = 1,
                     .sample_count = SDL_GPU_SAMPLECOUNT_1,
                 });
    rg->tb = SDL_CreateGPUTransferBuffer(
        rg->dev, &(SDL_GPUTransferBufferCreateInfo){
                     .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                     .size = w * h * 4u,
                 });
    return rg->r != nullptr && rg->atlas != nullptr && rg->tex != nullptr && rg->tb != nullptr;
}

static void brig_close(brig *rg)
{
    if (rg->dev != nullptr) {
        SDL_WaitForGPUIdle(rg->dev);
        if (rg->tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(rg->dev, rg->tb);
        }
        if (rg->tex != nullptr) {
            SDL_ReleaseGPUTexture(rg->dev, rg->tex);
        }
    }
    embi_atlas_destroy(rg->atlas);
    if (rg->r != nullptr) {
        embi_renderer_destroy(rg->r);
    }
    if (rg->g != nullptr) {
        embi_gpu_destroy(rg->g);
    }
    SDL_memset(rg, 0, sizeof *rg);
}

/* Records one frame into the rig's target, after the uploads already recorded in cmd. Returns the
 * draw calls of the frame. */
static uint32_t brig_frame(brig *rg, SDL_GPUCommandBuffer *cmd, emb_test_scene_fn scene,
                           void *user)
{
    embi_frame *f = embi_frame_begin(rg->r, rg->w, rg->h, 1.0f);
    if (f != nullptr && scene != nullptr) {
        scene(f, user);
    }
    if (f != nullptr) {
        embi_frame_end(f, cmd, rg->tex, true, 0);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
    return embi_renderer_last_draw_calls(rg->r);
}

/* Reads the target into a new RGBA8 buffer (SDL_free it). */
static uint8_t *brig_read(brig *rg)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(
        cp,
        &(SDL_GPUTextureRegion){.texture = rg->tex, .w = rg->w, .h = rg->h, .d = 1},
        &(SDL_GPUTextureTransferInfo){
            .transfer_buffer = rg->tb,
            .pixels_per_row = rg->w,
            .rows_per_layer = rg->h,
        });
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
    const size_t n = (size_t)rg->w * rg->h * 4u;
    uint8_t *out = SDL_malloc(n);
    if (out == nullptr) {
        return nullptr;
    }
    uint8_t *mapped = SDL_MapGPUTransferBuffer(rg->dev, rg->tb, false);
    if (mapped != nullptr) {
        SDL_memcpy(out, mapped, n);
        SDL_UnmapGPUTransferBuffer(rg->dev, rg->tb);
    }
    return out;
}

/* Fills an RGBA8 buffer of w x h with one color (non-premultiplied, r in the low byte). */
static void fill_solid(uint8_t *px, uint32_t w, uint32_t h, uint32_t rgba)
{
    for (uint32_t i = 0; i < w * h; i++) {
        SDL_memcpy(px + (size_t)i * 4u, &rgba, 4);
    }
}

/* The closed square outline of [x0, x1] x [y0, y1] as four quadratic segments with straight
 * controls, in local px. */
static void square_curves(embi_qcurve c[4], float x0, float y0, float x1, float y1)
{
    const float xs[5] = {x0, x1, x1, x0, x0};
    const float ys[5] = {y0, y0, y1, y1, y0};
    for (int i = 0; i < 4; i++) {
        c[i].x0 = xs[i];
        c[i].y0 = ys[i];
        c[i].cx = 0.5f * (xs[i] + xs[i + 1]);
        c[i].cy = 0.5f * (ys[i] + ys[i + 1]);
        c[i].x1 = xs[i + 1];
        c[i].y1 = ys[i + 1];
    }
}

/* ------------------------------------------------------------------ interleaved frame */

static void scene_interleaved(embi_frame *f, void *user)
{
    const embi_atlas_region *img = user; /* two regions: img[0] magenta, img[1] cyan */
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};

    /* 1: a red fill over the top-left corner. */
    const float red_rect[4] = {0, 0, 32, 32};
    embi_frame_fill(f, red_rect, zero, RGBA(255, 0, 0, 255), 0, 0);

    /* 2: a green square outline, a glyph in the batch sense (an outline prim). */
    embi_qcurve sq[4];
    square_curves(sq, 8.0f, 8.0f, 20.0f, 20.0f);
    const float ident[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_paint green = embi_paint_solid(RGBA(0, 255, 0, 255));
    embi_frame_fill_quads(f, 0x1001u, sq, 4, ident, &green, EMBI_FILL_NONZERO, 0, 0);

    /* 3: a magenta image on the atlas. */
    const float img_a[4] = {32, 0, 8, 8};
    embi_frame_image(f, img_a, zero, img[0], uv, RGBA(255, 255, 255, 255), 0, 0);

    /* 4: a 2 px blue border. */
    const float bo_rect[4] = {40, 40, 16, 16};
    const float bo_w[4] = {2, 2, 2, 2};
    const uint32_t bo_c[4] = {RGBA(0, 0, 255, 255), RGBA(0, 0, 255, 255), RGBA(0, 0, 255, 255),
                              RGBA(0, 0, 255, 255)};
    embi_frame_border(f, bo_rect, zero, bo_w, bo_c, 0, 0);

    /* 5: a linear gradient, drawn over an empty area. */
    const embi_stop stops[2] = {{0.0f, RGBA(255, 255, 255, 255)}, {1.0f, RGBA(0, 0, 0, 255)}};
    const float gr_rect[4] = {0, 40, 32, 16};
    embi_frame_linear(f, gr_rect, zero, 0, 40, 32, 40, stops, 2, false, 0, 0);

    /* 6: a yellow outline over the red fill. Paint order makes it win. */
    embi_qcurve sq2[4];
    square_curves(sq2, 24.0f, 24.0f, 30.0f, 30.0f);
    embi_paint yellow = embi_paint_solid(RGBA(255, 255, 0, 255));
    embi_frame_fill_quads(f, 0x1002u, sq2, 4, ident, &yellow, EMBI_FILL_NONZERO, 0, 0);

    /* 7: a cyan image in the same atlas page. */
    const float img_b[4] = {32, 32, 8, 8};
    embi_frame_image(f, img_b, zero, img[1], uv, RGBA(255, 255, 255, 255), 0, 0);

    /* 8: a white fill in the corner. */
    const float wh_rect[4] = {56, 56, 4, 4};
    embi_frame_fill(f, wh_rect, zero, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(batch_interleaved_box_outline_image_is_one_draw)
{
    brig rg;
    if (!brig_open(&rg, 64, 64)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* Two 16 x 16 solid images on the shared atlas, uploaded in the frame's command buffer. */
    embi_atlas_region reg[2];
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 16, 16, &reg[0]));
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 16, 16, &reg[1]));
    uint8_t *px = SDL_malloc(16 * 16 * 4);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    fill_solid(px, 16, 16, RGBA(255, 0, 255, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[0], px, 16 * 4, cmd));
    fill_solid(px, 16, 16, RGBA(0, 255, 255, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[1], px, 16 * 4, cmd));
    SDL_free(px);

    const uint32_t draws = brig_frame(&rg, cmd, scene_interleaved, reg);
    /* Box, outline and image prims, all on one texture, in one pipeline: one draw. */
    EMB_CHECK_EQ_INT(draws, 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    EMB_CHECK_PIXEL(out, 64, 4, 4, 255, 0, 0, 255);      /* red fill, outside the outline */
    EMB_CHECK_PIXEL(out, 64, 10, 10, 0, 255, 0, 255);    /* green outline interior */
    EMB_CHECK_PIXEL(out, 64, 36, 4, 255, 0, 255, 255);   /* magenta image */
    EMB_CHECK_PIXEL(out, 64, 41, 45, 0, 0, 255, 255);    /* border side */
    EMB_CHECK_PIXEL(out, 64, 27, 27, 255, 255, 0, 255);  /* later outline over the fill */
    EMB_CHECK_PIXEL(out, 64, 36, 36, 0, 255, 255, 255);  /* cyan image, second layer use */
    EMB_CHECK_PIXEL(out, 64, 57, 57, 255, 255, 255, 255); /* white fill */
    EMB_CHECK_PIXEL(out, 64, 60, 2, 0, 0, 0, 0);         /* nothing drawn there */
    SDL_free(out);
    brig_close(&rg);
}

/* ------------------------------------------------------------------ images on three pages */

#define BIG_SIDE 1000u /* a shared image (under the solo side): four to a page */
#define BIG_N 9u

typedef struct pages_scene {
    const embi_atlas_region *reg;
    uint32_t n;
} pages_scene;

static void scene_three_pages(embi_frame *f, void *user)
{
    const pages_scene *s = user;
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    for (uint32_t i = 0; i < s->n; i++) {
        const float r[4] = {(float)(i * 6u), 0.0f, 4.0f, 4.0f};
        embi_frame_image(f, r, zero, s->reg[i], uv, RGBA(255, 255, 255, 255), 0, 0);
        /* A black fill between the images: the frame stays one batch. */
        const float gap[4] = {(float)(i * 6u + 4u), 0.0f, 2.0f, 4.0f};
        embi_frame_fill(f, gap, zero, RGBA(0, 0, 0, 255), 0, 0);
    }
}

EMB_TEST(batch_images_on_three_pages_is_one_draw)
{
    brig rg;
    if (!brig_open(&rg, 64, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg[BIG_N];
    uint8_t *px = SDL_malloc((size_t)BIG_SIDE * BIG_SIDE * 4u);
    EMB_REQUIRE(px != nullptr);
    /* Each image is allocated and uploaded in its own command buffer, submitted before the next,
     * so a page that grows the array copies what was uploaded before it. */
    for (uint32_t i = 0; i < BIG_N; i++) {
        EMB_REQUIRE(embi_atlas_alloc(rg.atlas, BIG_SIDE, BIG_SIDE, &reg[i]));
        fill_solid(px, BIG_SIDE, BIG_SIDE, RGBA(i * 28u, 255u - i * 28u, 128u, 255u));
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
        EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[i], px, BIG_SIDE * 4u, cmd));
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    SDL_free(px);
    /* Four shared images to a page: the nine images fill pages 0, 1 and 2. */
    EMB_CHECK_EQ_INT(reg[0].layer, 0);
    EMB_CHECK_EQ_INT(reg[4].layer, 1);
    EMB_CHECK_EQ_INT(reg[8].layer, 2);

    pages_scene s = {.reg = reg, .n = BIG_N};
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    const uint32_t draws = brig_frame(&rg, cmd, scene_three_pages, &s);
    EMB_CHECK_EQ_INT(draws, 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    for (uint32_t i = 0; i < BIG_N; i++) {
        const uint32_t want = RGBA(i * 28u, 255u - i * 28u, 128u, 255u);
        const uint8_t *p = out + ((size_t)1 * 64u + i * 6u + 1u) * 4u;
        EMB_CHECK(SDL_memcmp(p, &want, 4) == 0);
    }
    SDL_free(out);
    for (uint32_t i = 0; i < BIG_N; i++) {
        embi_atlas_free(rg.atlas, &reg[i]);
    }
    brig_close(&rg);
}

/* ------------------------------------------------------------------ growth keeps pages */

#define GROW_N 17u /* five pages: the array grows from 0 to 4 layers, then to 8 */

EMB_TEST(batch_atlas_growth_keeps_earlier_pages)
{
    brig rg;
    if (!brig_open(&rg, 128, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg[GROW_N];
    uint8_t *px = SDL_malloc((size_t)BIG_SIDE * BIG_SIDE * 4u);
    EMB_REQUIRE(px != nullptr);
    /* Each image goes up before the next is allocated. The fifth page grows the array (a copy of
     * the four pages already there), so the early pages must survive it. */
    for (uint32_t i = 0; i < GROW_N; i++) {
        EMB_REQUIRE(embi_atlas_alloc(rg.atlas, BIG_SIDE, BIG_SIDE, &reg[i]));
        fill_solid(px, BIG_SIDE, BIG_SIDE, RGBA(i * 14u, 200u - i * 10u, 60u + i * 9u, 255u));
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
        EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[i], px, BIG_SIDE * 4u, cmd));
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    SDL_free(px);
    EMB_CHECK_EQ_INT(reg[16].layer, 4);

    /* The frame draws the first image of each page and the last one, from the grown array. */
    embi_atlas_region draw[GROW_N];
    for (uint32_t i = 0; i < GROW_N; i++) {
        draw[i] = reg[i];
    }
    pages_scene s = {.reg = draw, .n = GROW_N};
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    const uint32_t draws = brig_frame(&rg, cmd, scene_three_pages, &s);
    EMB_CHECK_EQ_INT(draws, 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    for (uint32_t i = 0; i < GROW_N; i++) {
        const uint32_t want = RGBA(i * 14u, 200u - i * 10u, 60u + i * 9u, 255u);
        const uint8_t *p = out + ((size_t)1 * 128u + i * 6u + 1u) * 4u;
        EMB_CHECK(SDL_memcmp(p, &want, 4) == 0);
    }
    SDL_free(out);
    for (uint32_t i = 0; i < GROW_N; i++) {
        embi_atlas_free(rg.atlas, &reg[i]);
    }
    brig_close(&rg);
}

/* ------------------------------------------------------------------ growth in one command buffer */

/* A 1024 x 1024 image takes a shared page of its own (its slot is 1026 square, so two do not fit),
 * so each one is one layer. The five layers take the array from 1 to 2, 3, 4 and then 6 layers. */
#define LAYER_SIDE 1024u
#define LAYER_N 5u

static uint32_t layer_color(uint32_t i)
{
    return RGBA(i * 40u + 10u, 200u - i * 30u, 90u + i * 20u, 255u);
}

/* Every upload goes into one command buffer, which is submitted once, after the last layer is
 * allocated. Each growth happens between two uploads of that buffer, so the copies must run in
 * the buffer, after the earlier uploads and before the later ones. Every layer's pixels survive. */
EMB_TEST(batch_growth_in_one_cmd_keeps_earlier_layers)
{
    const size_t mem_before = embi_mem_live();
    brig rg;
    if (!brig_open(&rg, 64, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg[LAYER_N];
    uint8_t *px = SDL_malloc((size_t)LAYER_SIDE * LAYER_SIDE * 4u);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    for (uint32_t i = 0; i < LAYER_N; i++) {
        EMB_REQUIRE(embi_atlas_alloc(rg.atlas, LAYER_SIDE, LAYER_SIDE, &reg[i]));
        fill_solid(px, LAYER_SIDE, LAYER_SIDE, layer_color(i));
        EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[i], px, LAYER_SIDE * 4u, cmd));
    }
    SDL_free(px);
    EMB_CHECK_EQ_INT(reg[4].layer, 4);

    pages_scene s = {.reg = reg, .n = LAYER_N};
    const uint32_t draws = brig_frame(&rg, cmd, scene_three_pages, &s);
    EMB_CHECK_EQ_INT(draws, 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    for (uint32_t i = 0; i < LAYER_N; i++) {
        const uint32_t want = layer_color(i);
        const uint8_t *p = out + ((size_t)1 * 64u + i * 6u + 1u) * 4u;
        EMB_CHECK(SDL_memcmp(p, &want, 4) == 0);
    }
    SDL_free(out);
    for (uint32_t i = 0; i < LAYER_N; i++) {
        embi_atlas_free(rg.atlas, &reg[i]);
    }
    brig_close(&rg);
    EMB_CHECK(embi_mem_live() == mem_before);
}

/* A growth with no upload after it: the frame's end records the pending copies before it draws
 * layers 0 to 3 from the grown array. */
EMB_TEST(batch_growth_pending_at_frame_end_is_flushed)
{
    brig rg;
    if (!brig_open(&rg, 64, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg[LAYER_N];
    uint8_t *px = SDL_malloc((size_t)LAYER_SIDE * LAYER_SIDE * 4u);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    for (uint32_t i = 0; i < LAYER_N - 1u; i++) {
        EMB_REQUIRE(embi_atlas_alloc(rg.atlas, LAYER_SIDE, LAYER_SIDE, &reg[i]));
        fill_solid(px, LAYER_SIDE, LAYER_SIDE, layer_color(i));
        EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[i], px, LAYER_SIDE * 4u, cmd));
    }
    /* The fifth page grows the array from four layers. Nothing is uploaded to it. */
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, LAYER_SIDE, LAYER_SIDE, &reg[LAYER_N - 1u]));
    SDL_free(px);

    pages_scene s = {.reg = reg, .n = LAYER_N - 1u};
    const uint32_t draws = brig_frame(&rg, cmd, scene_three_pages, &s);
    EMB_CHECK_EQ_INT(draws, 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    for (uint32_t i = 0; i < LAYER_N - 1u; i++) {
        const uint32_t want = layer_color(i);
        const uint8_t *p = out + ((size_t)1 * 64u + i * 6u + 1u) * 4u;
        EMB_CHECK(SDL_memcmp(p, &want, 4) == 0);
    }
    SDL_free(out);
    for (uint32_t i = 0; i < LAYER_N; i++) {
        embi_atlas_free(rg.atlas, &reg[i]);
    }
    brig_close(&rg);
}

/* The paint-hook pattern: inside one open frame, each image is allocated, uploaded into the frame's
 * command buffer and drawn. The array grows in the middle of the frame, and each image drawn
 * before or after the growth resolves to the array as it is at the frame's end. */
EMB_TEST(batch_image_drawn_after_growth_in_same_frame)
{
    brig rg;
    if (!brig_open(&rg, 64, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    uint8_t *px = SDL_malloc((size_t)LAYER_SIDE * LAYER_SIDE * 4u);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    embi_frame *f = embi_frame_begin(rg.r, rg.w, rg.h, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    for (uint32_t i = 0; i < LAYER_N; i++) {
        embi_atlas_region r;
        EMB_REQUIRE(embi_atlas_alloc(rg.atlas, LAYER_SIDE, LAYER_SIDE, &r));
        fill_solid(px, LAYER_SIDE, LAYER_SIDE, layer_color(i));
        EMB_REQUIRE(embi_atlas_upload(rg.atlas, &r, px, LAYER_SIDE * 4u, cmd));
        const float rect[4] = {(float)(i * 6u), 0.0f, 4.0f, 4.0f};
        embi_frame_image(f, rect, zero, r, uv, RGBA(255, 255, 255, 255), 0, 0);
    }
    SDL_free(px);
    embi_frame_end(f, cmd, rg.tex, true, 0);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg.dev);
    EMB_CHECK_EQ_INT(embi_renderer_last_draw_calls(rg.r), 1);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    for (uint32_t i = 0; i < LAYER_N; i++) {
        const uint32_t want = layer_color(i);
        const uint8_t *p = out + ((size_t)1 * 64u + i * 6u + 1u) * 4u;
        EMB_CHECK(SDL_memcmp(p, &want, 4) == 0);
    }
    SDL_free(out);
    brig_close(&rg);
}

/* ------------------------------------------------------------------ batch breaks */

#define SOLO_W 1100u /* a side above the solo limit: a texture of its own */

/* An atlas image, a solo image, then another atlas image of the same page: the solo image breaks
 * the batch before it, and the second atlas image breaks it again. Three draws. */
static void scene_atlas_solo_atlas(embi_frame *f, void *user)
{
    const embi_atlas_region *reg = user; /* reg[0] and reg[2]: atlas, reg[1]: solo */
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float ra[4] = {0, 0, 8, 8};
    const float rs[4] = {8, 0, 16, 8};
    const float rb[4] = {24, 0, 8, 8};
    embi_frame_image(f, ra, zero, reg[0], uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, rs, zero, reg[1], uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, rb, zero, reg[2], uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(batch_atlas_solo_atlas_is_three_draws)
{
    brig rg;
    if (!brig_open(&rg, 32, 8)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg[3];
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 16, 8, &reg[0]));
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, SOLO_W, 8, &reg[1]));
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 16, 8, &reg[2]));
    EMB_CHECK(reg[1].atlas == nullptr); /* the solo page */
    EMB_CHECK(reg[0].atlas == rg.atlas && reg[2].atlas == rg.atlas);

    uint8_t *px = SDL_malloc((size_t)SOLO_W * 8u * 4u);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    fill_solid(px, 16, 8, RGBA(255, 0, 0, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[0], px, 16 * 4u, cmd));
    fill_solid(px, SOLO_W, 8, RGBA(0, 255, 0, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[1], px, SOLO_W * 4u, cmd));
    fill_solid(px, 16, 8, RGBA(0, 0, 255, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg[2], px, 16 * 4u, cmd));
    SDL_free(px);

    const uint32_t draws = brig_frame(&rg, cmd, scene_atlas_solo_atlas, reg);
    EMB_CHECK_EQ_INT(draws, 3);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    const uint32_t red = RGBA(255, 0, 0, 255);
    const uint32_t green = RGBA(0, 255, 0, 255);
    const uint32_t blue = RGBA(0, 0, 255, 255);
    EMB_CHECK(SDL_memcmp(out + (4u * 32u + 4u) * 4u, &red, 4) == 0);
    EMB_CHECK(SDL_memcmp(out + (4u * 32u + 16u) * 4u, &green, 4) == 0);
    EMB_CHECK(SDL_memcmp(out + (4u * 32u + 28u) * 4u, &blue, 4) == 0);
    SDL_free(out);
    for (uint32_t i = 0; i < 3; i++) {
        embi_atlas_free(rg.atlas, &reg[i]);
    }
    brig_close(&rg);
}

/* A region frame: the region clear is its own draw, and the atlas image is the second. */
EMB_TEST(batch_region_frame_is_two_draws)
{
    brig rg;
    if (!brig_open(&rg, 64, 16)) {
        brig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_atlas_region reg;
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 16, 16, &reg));
    uint8_t *px = SDL_malloc(16 * 16 * 4);
    EMB_REQUIRE(px != nullptr);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    fill_solid(px, 16, 16, RGBA(255, 0, 255, 255));
    EMB_REQUIRE(embi_atlas_upload(rg.atlas, &reg, px, 16 * 4u, cmd));
    SDL_free(px);

    /* A full frame of red first, so the region frame has something outside its damage to keep. */
    const float red_rect[4] = {0, 0, 64, 16};
    const float zero[4] = {0, 0, 0, 0};
    embi_frame *f0 = embi_frame_begin(rg.r, 64, 16, 1.0f);
    EMB_REQUIRE(f0 != nullptr);
    embi_frame_fill(f0, red_rect, zero, RGBA(255, 0, 0, 255), 0, 0);
    embi_frame_end(f0, cmd, rg.tex, true, 0);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg.dev);

    /* The region frame: damage x 0..31 is cleared to blue, then the image is drawn in it. */
    cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    EMB_REQUIRE(cmd != nullptr);
    const embi_dev_rect dmg = {0, 0, 32, 16};
    embi_frame *f = embi_frame_begin_region(rg.r, 64, 16, 1.0f, dmg);
    EMB_REQUIRE(f != nullptr);
    const float uv[4] = {0, 0, 1, 1};
    const float ri[4] = {0, 0, 16, 16};
    embi_frame_image(f, ri, zero, reg, uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_end(f, cmd, rg.tex, true, RGBA(0, 0, 255, 255));
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg.dev);
    EMB_CHECK_EQ_INT(embi_renderer_last_draw_calls(rg.r), 2);

    uint8_t *out = brig_read(&rg);
    EMB_REQUIRE(out != nullptr);
    const uint32_t magenta = RGBA(255, 0, 255, 255);
    const uint32_t blue = RGBA(0, 0, 255, 255);
    const uint32_t red2 = RGBA(255, 0, 0, 255);
    EMB_CHECK(SDL_memcmp(out + (4u * 64u + 4u) * 4u, &magenta, 4) == 0); /* image, in damage */
    EMB_CHECK(SDL_memcmp(out + (4u * 64u + 20u) * 4u, &blue, 4) == 0);   /* cleared, in damage */
    EMB_CHECK(SDL_memcmp(out + (4u * 64u + 50u) * 4u, &red2, 4) == 0);   /* kept outside it */
    SDL_free(out);
    embi_atlas_free(rg.atlas, &reg);
    brig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
