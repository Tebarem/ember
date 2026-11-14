#include "emb_render_test.h"

#include "render_check.h"

#include "base/base.h"
#include "gpu/atlas.h"

/* Atlas images through the renderer: uploads, nearest-texel and bilinear sampling, edges that
 * must not bleed, sub-rect uv, tint, premultiplied blending, solo pages, batch splits by page, and
 * a steady-state allocation check. Expected values are hand-computed: a pixel center is (x + 0.5,
 * y + 0.5), and an image texel i covers [i, i + 1) in image space.
 *
 * This file has its own rig, because an image needs its upload recorded in the same command
 * buffer as the frame, before embi_frame_end. */

typedef struct rig {
    embi_gpu *g;
    embi_renderer *r;
    embi_atlas *atlas;
    SDL_GPUDevice *dev;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *tb;
    uint32_t w;
    uint32_t h;
} rig;

static bool rig_open(rig *rg, uint32_t w, uint32_t h)
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

static void rig_close(rig *rg)
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

/* Uploads recorded into the frame's command buffer before the frame. */
#define RIG_MAX_UPLOADS 4
typedef struct upload_item {
    const embi_atlas_region *reg;
    const uint8_t *px;
    uint32_t stride;
} upload_item;
typedef struct uploads {
    embi_atlas *atlas;
    uint32_t n;
    upload_item item[RIG_MAX_UPLOADS];
} uploads;

static void run_uploads(SDL_GPUCommandBuffer *cmd, void *user)
{
    uploads *u = user;
    for (uint32_t i = 0; i < u->n; i++) {
        const bool ok = embi_atlas_upload(u->atlas, u->item[i].reg, u->item[i].px,
                                          u->item[i].stride, cmd);
        EMB_CHECK(ok);
    }
}

/* Runs one frame into the rig's target. Returns the draw calls the frame used. */
static uint32_t rig_draw(rig *rg, uploads *up, emb_test_scene_fn scene, void *user, bool clear,
                         uint32_t clear_rgba)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    if (up != nullptr) {
        run_uploads(cmd, up);
    }
    embi_frame *f = embi_frame_begin(rg->r, rg->w, rg->h, 1.0f);
    if (f != nullptr && scene != nullptr) {
        scene(f, user);
    }
    if (f != nullptr) {
        embi_frame_end(f, cmd, rg->tex, clear, clear_rgba);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
    return embi_renderer_last_draw_calls(rg->r);
}

/* Reads the target into a new RGBA8 buffer (SDL_free it). */
static uint8_t *rig_read(rig *rg)
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

    const size_t bytes = (size_t)rg->w * (size_t)rg->h * 4u;
    uint8_t *out = nullptr;
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(rg->dev, rg->tb, false);
    if (mapped != nullptr) {
        out = SDL_malloc(bytes);
        if (out != nullptr) {
            SDL_memcpy(out, mapped, bytes);
        }
        SDL_UnmapGPUTransferBuffer(rg->dev, rg->tb);
    }
    return out;
}

/* A 4 x 4 image with one color per quadrant: red top-left, green top-right, blue bottom-left,
 * yellow bottom-right. */
static void make_quadrants(uint8_t px[4 * 4 * 4])
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int top = y < 2;
            const int left = x < 2;
            uint8_t c[4];
            if (top && left) {
                c[0] = 255, c[1] = 0, c[2] = 0;
            } else if (top) {
                c[0] = 0, c[1] = 255, c[2] = 0;
            } else if (left) {
                c[0] = 0, c[1] = 0, c[2] = 255;
            } else {
                c[0] = 255, c[1] = 255, c[2] = 0;
            }
            c[3] = 255;
            SDL_memcpy(px + (y * 4 + x) * 4, c, 4);
        }
    }
}

/* ------------------------------------------------------------------ 1:1 and bilinear */

static uint8_t g_quad[4 * 4 * 4];
static embi_atlas_region g_quad_reg;

static void scene_quad_1to1(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {8, 8, 4, 4};
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    embi_frame_image(f, rect, radii, g_quad_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_1to1_copies_every_texel)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    make_quadrants(g_quad);
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_quad_reg));
    uploads up = {.atlas = rg.atlas,
                  .n = 1,
                  .item = {{&g_quad_reg, g_quad, 4 * 4}}};
    rig_draw(&rg, &up, scene_quad_1to1, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Each texel is one device px, centered on its texel, so the corners of the quadrants match
     * exactly. */
    CHECK_PX_NEAR(px, 32, 8, 8, 255, 0, 0, 255, 1);
    CHECK_PX_NEAR(px, 32, 11, 8, 0, 255, 0, 255, 1);
    CHECK_PX_NEAR(px, 32, 8, 11, 0, 0, 255, 255, 1);
    CHECK_PX_NEAR(px, 32, 11, 11, 255, 255, 0, 255, 1);
    EMB_CHECK_PIXEL(px, 32, 7, 8, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 32, 12, 8, 0, 0, 0, 0);
    SDL_free(px);
    rig_close(&rg);
}

static uint8_t g_ramp[2 * 2 * 4];
static embi_atlas_region g_ramp_reg;

static void scene_ramp(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 8, 8};
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    embi_frame_image(f, rect, radii, g_ramp_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_upscale_filters_linearly_and_clamps_at_the_edge)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* A 2 x 2 image: the left column is black, the right column white. */
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            const uint8_t v = (x == 0) ? 0 : 255;
            uint8_t *p = g_ramp + (y * 2 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 2, 2, &g_ramp_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_ramp_reg, g_ramp, 2 * 4}}};
    rig_draw(&rg, &up, scene_ramp, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Pixel 2: u = 2.5 / 8, so the texel coordinate is 0.625 and the white weight is 0.125.
     * Gray 32. */
    CHECK_PX_NEAR(px, 32, 2, 4, 32, 32, 32, 255, 3);
    /* Pixel 5: u = 5.5 / 8, texel coordinate 1.375, white weight 0.875. Gray 223. */
    CHECK_PX_NEAR(px, 32, 5, 4, 223, 223, 223, 255, 3);
    /* Pixel 0 sits left of the first texel center and is clamped to the edge: black. */
    EMB_CHECK_PIXEL(px, 32, 0, 4, 0, 0, 0, 255);
    /* Pixel 7 is past the last texel center: white. */
    EMB_CHECK_PIXEL(px, 32, 7, 4, 255, 255, 255, 255);
    SDL_free(px);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ edges and packing */

static uint8_t g_red[4 * 4 * 4];
static uint8_t g_blue[4 * 4 * 4];
static embi_atlas_region g_red_reg;
static embi_atlas_region g_blue_reg;

static void scene_two_images(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    /* Scaled 4x: each image fills 16 px. The red one sits on the left, the blue on the right. */
    const float left[4] = {0, 0, 16, 16};
    const float right[4] = {16, 0, 16, 16};
    embi_frame_image(f, left, radii, g_red_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, right, radii, g_blue_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_edges_do_not_bleed_into_neighbors)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    for (int i = 0; i < 16; i++) {
        g_red[i * 4 + 0] = 255, g_red[i * 4 + 3] = 255;
        g_blue[i * 4 + 2] = 255, g_blue[i * 4 + 3] = 255;
    }
    /* Both images go on one page, side by side, so each one's border touches the other's. */
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_red_reg));
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_blue_reg));
    EMB_REQUIRE(g_red_reg.tex == g_blue_reg.tex);
    uploads up = {.atlas = rg.atlas,
                  .n = 2,
                  .item = {{&g_red_reg, g_red, 16}, {&g_blue_reg, g_blue, 16}}};
    rig_draw(&rg, &up, scene_two_images, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Every pixel of the red image, including its edges, is pure red. Bilinear reads near the
     * edge must not pick up the blue neighbor. */
    EMB_CHECK_PIXEL(px, 32, 0, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 32, 15, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 32, 15, 15, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 32, 0, 15, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 32, 16, 0, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, 32, 31, 15, 0, 0, 255, 255);
    SDL_free(px);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ uv, tint, premultiply */

static uint8_t g_white[4 * 4 * 4];
static embi_atlas_region g_white_reg;

static void scene_tints(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float a[4] = {0, 0, 4, 4};
    const float b[4] = {8, 0, 4, 4};
    /* Tint red over white gives red. */
    embi_frame_image(f, a, radii, g_white_reg, uv, RGBA(255, 0, 0, 255), 0, 0);
    /* Half-alpha white tint gives half-alpha white, premultiplied. */
    embi_frame_image(f, b, radii, g_white_reg, uv, RGBA(255, 255, 255, 128), 0, 0);
}

EMB_TEST(image_tint_multiplies_color_and_alpha)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    for (int i = 0; i < 16; i++) {
        g_white[i * 4 + 0] = g_white[i * 4 + 1] = g_white[i * 4 + 2] = 255;
        g_white[i * 4 + 3] = 255;
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_white_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_white_reg, g_white, 16}}};
    rig_draw(&rg, &up, scene_tints, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 32, 1, 1, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, 32, 9, 1, 128, 128, 128, 128, 2);
    SDL_free(px);
    rig_close(&rg);
}

static uint8_t g_half[4];
static embi_atlas_region g_half_reg;

static void scene_premul_over(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float bg[4] = {0, 0, 4, 4};
    const float img[4] = {0, 0, 4, 4};
    /* An opaque green background, then a 1 x 1 blue image at half alpha, stretched over it. */
    embi_frame_fill(f, bg, zero, RGBA(0, 255, 0, 255), 0, 0);
    embi_frame_image(f, img, zero, g_half_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_stores_premultiplied_and_blends_over_the_destination)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* Non-premultiplied (0, 0, 255, 128). The atlas stores (0, 0, 128, 128). */
    g_half[0] = 0, g_half[1] = 0, g_half[2] = 255, g_half[3] = 128;
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 1, 1, &g_half_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_half_reg, g_half, 4}}};
    rig_draw(&rg, &up, scene_premul_over, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Source-over of premultiplied (0, 0, 128, 128) on opaque green: (0, 127.5, 128, 255). */
    CHECK_PX_NEAR(px, 32, 2, 2, 0, 128, 128, 255, 2);
    SDL_free(px);
    rig_close(&rg);
}

static void scene_subuv(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float a[4] = {0, 0, 2, 2};
    const float b[4] = {4, 0, 2, 2};
    /* Bottom-right quadrant (yellow) and top-left quadrant (red) of the quadrant image. */
    const float uv_br[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    const float uv_tl[4] = {0.0f, 0.0f, 0.5f, 0.5f};
    embi_frame_image(f, a, radii, g_quad_reg, uv_br, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, b, radii, g_quad_reg, uv_tl, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_uv_sub_rect_selects_part_of_the_image)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    make_quadrants(g_quad);
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_quad_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_quad_reg, g_quad, 16}}};
    rig_draw(&rg, &up, scene_subuv, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    CHECK_PX_NEAR(px, 32, 0, 0, 255, 255, 0, 255, 1);
    CHECK_PX_NEAR(px, 32, 1, 1, 255, 255, 0, 255, 1);
    CHECK_PX_NEAR(px, 32, 4, 0, 255, 0, 0, 255, 1);
    CHECK_PX_NEAR(px, 32, 5, 1, 255, 0, 0, 255, 1);
    SDL_free(px);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ transform and corners */

static void scene_xform_round(embi_frame *f, void *user)
{
    (void)user;
    const float m[6] = {2, 0, 0, 2, 0, 0};
    const float rect[4] = {0, 0, 4, 4};
    const float radii[4] = {2, 2, 2, 2};
    const float uv[4] = {0, 0, 1, 1};
    const uint32_t x = embi_frame_xform(f, m);
    embi_frame_image(f, rect, radii, g_white_reg, uv, RGBA(255, 255, 255, 255), x, 0);
}

EMB_TEST(image_follows_transform_and_rounded_corners)
{
    rig rg;
    if (!rig_open(&rg, 32, 32)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    for (int i = 0; i < 16; i++) {
        g_white[i * 4 + 0] = g_white[i * 4 + 1] = g_white[i * 4 + 2] = 255;
        g_white[i * 4 + 3] = 255;
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_white_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_white_reg, g_white, 16}}};
    rig_draw(&rg, &up, scene_xform_round, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* The 4 px box is scaled to 8 device px. Its corner, with a 2 local px radius (4 device px),
     * is outside the arc at (0, 0). Its middle is opaque. */
    EMB_CHECK_PIXEL(px, 32, 0, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 32, 4, 4, 255, 255, 255, 255);
    EMB_CHECK_PIXEL(px, 32, 9, 9, 0, 0, 0, 0);
    SDL_free(px);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ solo pages and batches */

static uint8_t *g_wide;
static embi_atlas_region g_wide_reg;
static uint8_t g_tiny[4 * 4 * 4];
static embi_atlas_region g_tiny_reg;

static void scene_solo_halves(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float left[4] = {0, 0, 64, 2};
    const float right[4] = {0, 8, 64, 2};
    const float uv_l[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    const float uv_r[4] = {0.5f, 0.0f, 1.0f, 1.0f};
    embi_frame_image(f, left, radii, g_wide_reg, uv_l, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, right, radii, g_wide_reg, uv_r, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(image_solo_page_for_a_wide_image_samples_its_halves)
{
    rig rg;
    if (!rig_open(&rg, 64, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* 1100 x 2 is over the solo limit, so it gets a page to itself. The left 550 texels are red,
     * the right ones blue. */
    const uint32_t w = 1100;
    const uint32_t h = 2;
    g_wide = SDL_malloc((size_t)w * h * 4u);
    EMB_REQUIRE(g_wide != nullptr);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = g_wide + (y * w + x) * 4u;
            p[0] = (x < 550) ? 255 : 0;
            p[1] = 0;
            p[2] = (x < 550) ? 0 : 255;
            p[3] = 255;
        }
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, w, h, &g_wide_reg));
    EMB_CHECK(g_wide_reg.page_w == w + 2u && g_wide_reg.page_h == h + 2u);
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_wide_reg, g_wide, w * 4u}}};
    rig_draw(&rg, &up, scene_solo_halves, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 64, 0, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 64, 63, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 64, 0, 8, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, 64, 63, 9, 0, 0, 255, 255);
    SDL_free(px);
    SDL_free(g_wide);
    g_wide = nullptr;
    rig_close(&rg);
}

static void scene_three_draws(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float a[4] = {0, 0, 4, 4};
    const float b[4] = {8, 0, 4, 4};
    const float c[4] = {16, 0, 4, 4};
    embi_frame_image(f, a, radii, g_tiny_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, b, radii, g_wide_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, c, radii, g_tiny_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(page_changes_split_batches)
{
    rig rg;
    if (!rig_open(&rg, 32, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* Shared page, then a solo page, then the shared page again: three batches. */
    const uint32_t w = 1100;
    g_wide = SDL_calloc((size_t)w * 2u, 4u);
    EMB_REQUIRE(g_wide != nullptr);
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_tiny_reg));
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, w, 2, &g_wide_reg));
    uploads up = {.atlas = rg.atlas,
                  .n = 2,
                  .item = {{&g_tiny_reg, g_tiny, 16}, {&g_wide_reg, g_wide, w * 4u}}};
    const uint32_t draws = rig_draw(&rg, &up, scene_three_draws, nullptr, true, 0);
    EMB_CHECK_EQ_INT(draws, 3);
    SDL_free(g_wide);
    g_wide = nullptr;
    rig_close(&rg);
}

static void scene_mixed_many(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float img[4] = {0, 0, 4, 4};
    /* 50k fills with one image in the middle. Fills take any page, so the frame stays one batch. */
    for (uint32_t i = 0; i < 50000u; i++) {
        if (i == 25000u) {
            embi_frame_image(f, img, zero, g_tiny_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
        }
        const float rect[4] = {(float)((i % 256u) * 2u), (float)((i / 256u) * 2u), 2, 2};
        embi_frame_fill(f, rect, zero, RGBA(i & 255u, 128, 0, 255), 0, 0);
    }
}

EMB_TEST(fills_around_one_image_take_at_most_four_draws)
{
    rig rg;
    if (!rig_open(&rg, 512, 512)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_tiny_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_tiny_reg, g_tiny, 16}}};
    const uint32_t draws = rig_draw(&rg, &up, scene_mixed_many, nullptr, true, 0);
    EMB_CHECK(draws <= 4);
    EMB_CHECK_EQ_INT(draws, 1);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ sub-rects and nearest sampling */

/* An 8 x 1 image: four black texels, then four white. */
static uint8_t g_band[8 * 4];
static embi_atlas_region g_band_reg;

/* Two sub-rects of the band, each drawn 10x: the left one holds only black texels, the right one only white. A
 * linear sample at the sub-rect's interior edge would blend in the texel next to it, so this checks that the
 * clamp keeps each slice to its own texels. */
static void scene_band_slices(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float left[4] = {0, 0, 40, 4};
    const float right[4] = {40, 0, 40, 4};
    const float uv_l[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    const float uv_r[4] = {0.5f, 0.0f, 1.0f, 1.0f};
    embi_frame_image(f, left, radii, g_band_reg, uv_l, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_image(f, right, radii, g_band_reg, uv_r, RGBA(255, 255, 255, 255), 0, 0);
}

EMB_TEST(sub_rect_linear_sample_takes_no_texel_outside_the_sub_rect)
{
    rig rg;
    if (!rig_open(&rg, 80, 4)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    for (int x = 0; x < 8; x++) {
        const uint8_t v = (x < 4) ? 0 : 255;
        SDL_memset(g_band + x * 4, v, 3);
        g_band[x * 4 + 3] = 255;
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 8, 1, &g_band_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_band_reg, g_band, 8 * 4}}};
    rig_draw(&rg, &up, scene_band_slices, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    for (int x = 0; x < 40; x++) {
        EMB_CHECK(px[(2 * 80 + x) * 4 + 0] <= 1);
        EMB_CHECK(px[(2 * 80 + x) * 4 + 3] == 255);
    }
    for (int x = 40; x < 80; x++) {
        EMB_CHECK(px[(2 * 80 + x) * 4 + 0] >= 254);
    }
    SDL_free(px);
    rig_close(&rg);
}

/* A 2 x 2 checkerboard: black and white on the top row, white and black below. */
static uint8_t g_board[2 * 2 * 4];
static embi_atlas_region g_board_reg;

/* The left copy is pixelated (nearest), the right copy is smooth. */
static void scene_board(embi_frame *f, void *user)
{
    (void)user;
    const float radii[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float left[4] = {0, 0, 20, 20};
    const float right[4] = {20, 0, 20, 20};
    embi_frame_image_ex(f, left, radii, g_board_reg, uv, RGBA(255, 255, 255, 255), 0, 0, true);
    embi_frame_image_ex(f, right, radii, g_board_reg, uv, RGBA(255, 255, 255, 255), 0, 0, false);
}

EMB_TEST(image_rendering_pixelated_gives_hard_texel_blocks)
{
    rig rg;
    if (!rig_open(&rg, 40, 20)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            const uint8_t v = ((x + y) & 1) ? 255 : 0;
            uint8_t *p = g_board + (y * 2 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 2, 2, &g_board_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_board_reg, g_board, 2 * 4}}};
    rig_draw(&rg, &up, scene_board, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Each texel is a 10 x 10 block: the block of texel (tx, ty) is black when tx + ty is even. */
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 20; x++) {
            const int v = ((x / 10 + y / 10) & 1) ? 255 : 0;
            const uint8_t *p = px + (y * 40 + x) * 4;
            EMB_CHECK(p[0] == v);
        }
    }
    /* The smooth copy has intermediate values at the block edge, the pixelated one has none. */
    const uint8_t *smooth = px + (5 * 40 + 30) * 4;
    EMB_CHECK(smooth[0] > 64 && smooth[0] < 192);
    SDL_free(px);
    rig_close(&rg);
}

/* ------------------------------------------------------------------ steady state */

static void scene_steady(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float uv[4] = {0, 0, 1, 1};
    const float img[4] = {0, 0, 4, 4};
    const float lin[4] = {8, 8, 40, 40};
    const embi_stop st[2] = {{0.0f, RGBA(255, 0, 0, 255)}, {1.0f, RGBA(0, 0, 255, 255)}};
    embi_frame_image(f, img, zero, g_tiny_reg, uv, RGBA(255, 255, 255, 255), 0, 0);
    embi_frame_linear(f, lin, zero, 8, 8, 48, 8, st, 2, false, 0, 0);
}

EMB_TEST(image_and_gradient_frames_reach_a_steady_state)
{
    rig rg;
    if (!rig_open(&rg, 64, 64)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    EMB_REQUIRE(embi_atlas_alloc(rg.atlas, 4, 4, &g_tiny_reg));
    uploads up = {.atlas = rg.atlas, .n = 1, .item = {{&g_tiny_reg, g_tiny, 16}}};
    rig_draw(&rg, &up, scene_steady, nullptr, true, 0);
    rig_draw(&rg, nullptr, scene_steady, nullptr, true, 0);
    rig_draw(&rg, nullptr, scene_steady, nullptr, true, 0);
    const size_t before = embi_mem_live();
    for (int i = 0; i < 100; i++) {
        rig_draw(&rg, nullptr, scene_steady, nullptr, true, 0);
    }
    EMB_CHECK(embi_mem_live() == before);
    rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
