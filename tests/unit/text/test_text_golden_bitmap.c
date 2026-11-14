#include "text_fixture.h"

#include "emb_gold.h"
#include "emb_render_test.h"

#include "gpu/atlas.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>

/* Bitmap colour glyphs drawn through the text system's sink (W4i), on the GPU:
 *   bitmap_emoji_next_to_text_golden: "AxA B" at 40 px in the family list [Ember Bitmap, Noto Test]. Ember Bitmap has
 *     CBDT strikes (16, 32 and 64 ppem; the 64 ppem strike is a solid yellow 64x64 square) for 'A'. The two As are
 *     drawn as images, the 'x' comes from Noto Sans as an outline, 'B' is an outline of Ember Bitmap. Golden:
 *     tests/gold/text_bitmap_emoji.bmp.
 *   colr_wins_over_bitmaps: the same text in a face that has COLR and CBDT draws the COLR layers (opaque magenta) and
 *     never calls the sink.
 *   no_sink_draws_no_bitmap: without a sink a bitmap glyph draws nothing.
 * The sink here does what src/embed/gfx.c does: upload once per key, draw an image quad. */

#define GW 260
#define GH 72

typedef struct sink_cache {
    embi_atlas *atlas;
    SDL_GPUCommandBuffer *cmd;
    uint64_t keys[4];
    embi_atlas_region regs[4];
    uint32_t n;
    uint32_t calls;
    float last_rect[4];
    uint32_t last_w, last_h;
} sink_cache;

static bool test_sink(void *user, embi_frame *f, const embi_pixels *px, uint64_t key, const float rect[4], uint32_t xform,
                      uint32_t clip)
{
    sink_cache *c = user;
    c->calls++;
    SDL_memcpy(c->last_rect, rect, sizeof c->last_rect);
    c->last_w = px->w;
    c->last_h = px->h;
    uint32_t i = 0;
    while (i < c->n && c->keys[i] != key) {
        i++;
    }
    if (i == c->n) {
        if (c->n == 4 || !embi_atlas_alloc(c->atlas, px->w, px->h, &c->regs[c->n])) {
            return false;
        }
        if (!embi_atlas_upload(c->atlas, &c->regs[c->n], px->px, px->w * 4u, c->cmd)) {
            return false;
        }
        c->keys[c->n++] = key;
    }
    static const float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    embi_frame_image(f, rect, zero, c->regs[i], full, 0xFFFFFFFFu, xform, clip);
    return true;
}

typedef struct scene {
    embi_text *t;
    const embi_text_layout *lay;
    embi_text_vruns vr;
    sink_cache *sc;
} scene;

static void draw_scene(embi_frame *f, void *user)
{
    scene *s = user;
    embi_text_draw(s->t, f, s->lay, &s->vr, 10.0f, 50.0f, RGBA(20, 20, 20, 255), 0, 0);
}

/* A render like emb_test_render, with the command buffer known to the scene (the glyph uploads go into it). */
static uint8_t *render(scene *s, uint32_t w, uint32_t h)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return nullptr;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    embi_renderer *r = embi_renderer_create(g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    embi_atlas *atlas = embi_atlas_create(dev);
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(
        dev, &(SDL_GPUTextureCreateInfo){.type = SDL_GPU_TEXTURETYPE_2D,
                                         .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                         .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                         .width = w,
                                         .height = h,
                                         .layer_count_or_depth = 1,
                                         .num_levels = 1,
                                         .sample_count = SDL_GPU_SAMPLECOUNT_1});
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(
        dev, &(SDL_GPUTransferBufferCreateInfo){.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = w * h * 4u});
    uint8_t *out = nullptr;
    if (r != nullptr && atlas != nullptr && tex != nullptr && tb != nullptr) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        s->sc->atlas = atlas;
        s->sc->cmd = cmd;
        s->sc->n = 0;
        embi_frame *f = embi_frame_begin(r, w, h, 1.0f);
        draw_scene(f, s);
        embi_frame_end(f, cmd, tex, true, 0);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_DownloadFromGPUTexture(cp, &(SDL_GPUTextureRegion){.texture = tex, .w = w, .h = h, .d = 1},
                                   &(SDL_GPUTextureTransferInfo){.transfer_buffer = tb, .pixels_per_row = w,
                                                                 .rows_per_layer = h});
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence != nullptr) {
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            void *m = SDL_MapGPUTransferBuffer(dev, tb, false);
            if (m != nullptr) {
                out = SDL_malloc((size_t)w * h * 4u);
                if (out != nullptr) {
                    SDL_memcpy(out, m, (size_t)w * h * 4u);
                }
                SDL_UnmapGPUTransferBuffer(dev, tb);
            }
        }
    }
    SDL_WaitForGPUIdle(dev);
    if (tb != nullptr) {
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    if (tex != nullptr) {
        SDL_ReleaseGPUTexture(dev, tex);
    }
    embi_atlas_destroy(atlas);
    if (r != nullptr) {
        embi_renderer_destroy(r);
    }
    embi_gpu_destroy(g);
    return out;
}

static bool near_rgb(const uint8_t *px, int x, int y, int r, int g, int b)
{
    const uint8_t *p = px + ((size_t)y * GW + (size_t)x) * 4u;
    return SDL_abs(p[0] - r) <= 3 && SDL_abs(p[1] - g) <= 3 && SDL_abs(p[2] - b) <= 3 && p[3] >= 250;
}

static embi_text *make_text(const char *bitmap_font, bool with_noto)
{
    embi_text *t = embi_text_create();
    if (t == nullptr) {
        return nullptr;
    }
    embi_face_desc d = {0};
    d.family = tx_s("Ember Bitmap");
    char rel[128];
    SDL_snprintf(rel, sizeof rel, "tests/fonts/bitmap/%s", bitmap_font);
    if (tx_add_file(t, rel, &d) != 1) {
        embi_text_destroy(t);
        return nullptr;
    }
    if (with_noto) {
        embi_face_desc n = {0};
        n.family = tx_s("Noto Test");
        if (tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &n) != 1) {
            embi_text_destroy(t);
            return nullptr;
        }
    }
    return t;
}

EMB_TEST(bitmap_emoji_next_to_text_golden)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    size_t base = embi_mem_live();
    {
        embi_text *t = make_text("EmberBitmap-cbdt.ttf", true);
        EMB_REQUIRE(t != nullptr);
        sink_cache sc;
        SDL_memset(&sc, 0, sizeof sc);
        embi_text_set_bitmap_sink(t, test_sink, &sc);
        embi_str fams[2] = {tx_s("Ember Bitmap"), tx_s("Noto Test")};
        embi_text_style st;
        tx_style(&st, fams, 2, 40.0f);
        embi_text_layout lay;
        EMB_REQUIRE(tx_shape(t, "AxA B", &st, &lay));
        scene s;
        SDL_memset(&s, 0, sizeof s);
        s.t = t;
        s.lay = &lay;
        s.sc = &sc;
        EMB_REQUIRE(embi_text_line(&lay, 0, lay.text_len, &s.vr));
        uint8_t *px = render(&s, GW, GH);
        EMB_RENDER_OR_SKIP(px);
        /* Two As through the sink, both from the 64 ppem strike (40 px asks for the nearest strike that is at least
         * as large) scaled by 40/64: a 40 x 40 px image, left bearing 1 * 0.625, top 56 * 0.625 above the baseline. */
        EMB_CHECK_EQ_INT(sc.calls, 2);
        EMB_CHECK_EQ_INT(sc.last_w, 64);
        EMB_CHECK_EQ_INT(sc.last_h, 64);
        EMB_CHECK(SDL_fabsf(sc.last_rect[2] - 40.0f) < 1e-3f);
        EMB_CHECK(SDL_fabsf(sc.last_rect[3] - 40.0f) < 1e-3f);
        EMB_CHECK(SDL_fabsf(sc.last_rect[1] - 15.0f) < 1e-3f);
        /* The first A: yellow inside the image quad (x from 10.625 to 50.625, y from 15 to 55), dark text ink for 'x'. */
        EMB_CHECK(near_rgb(px, 30, 35, 255, 255, 0));
        EMB_CHECK(near_rgb(px, 14, 20, 255, 255, 0));
        EMB_CHECK(!near_rgb(px, 6, 35, 255, 255, 0)); /* left of the quad */
        uint32_t dark = 0;
        for (uint32_t i = 0; i < GW * GH; i++) {
            dark += (px[i * 4] < 60 && px[i * 4 + 1] < 60 && px[i * 4 + 3] > 200) ? 1u : 0u;
        }
        EMB_CHECK(dark > 100); /* the outline glyphs of 'x' and 'B' */
        EMB_CHECK(emb_gold_check("text_bitmap_emoji", px, GW, GH));
        SDL_free(px);
        embi_text_vruns_free(&s.vr);
        embi_text_layout_free(&lay);
        embi_text_destroy(t);
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(colr_wins_over_bitmaps)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = make_text("EmberBitmap-colr.ttf", false);
    EMB_REQUIRE(t != nullptr);
    sink_cache sc;
    SDL_memset(&sc, 0, sizeof sc);
    embi_text_set_bitmap_sink(t, test_sink, &sc);
    embi_str fams[1] = {tx_s("Ember Bitmap")};
    embi_text_style st;
    tx_style(&st, fams, 1, 40.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "A", &st, &lay));
    scene s;
        SDL_memset(&s, 0, sizeof s);
        s.t = t;
        s.lay = &lay;
        s.sc = &sc;
    EMB_REQUIRE(embi_text_line(&lay, 0, lay.text_len, &s.vr));
    uint8_t *px = render(&s, GW, GH);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_EQ_INT(sc.calls, 0); /* the COLR layers are drawn; the bitmap is not */
    /* COLR layer glyph 2 is the box (50, 0)-(170, 700) units of 1000: at 40 px x 12.0..16.8, y 22..50. Opaque
     * magenta (palette entry 0), not the yellow of the bitmap. */
    EMB_CHECK(near_rgb(px, 14, 40, 255, 0, 255));
    EMB_CHECK(!near_rgb(px, 30, 35, 255, 255, 0));
    SDL_free(px);
    embi_text_vruns_free(&s.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(no_sink_draws_no_bitmap)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = make_text("EmberBitmap-cbdt.ttf", false);
    EMB_REQUIRE(t != nullptr);
    sink_cache sc;
    SDL_memset(&sc, 0, sizeof sc);
    embi_str fams[1] = {tx_s("Ember Bitmap")};
    embi_text_style st;
    tx_style(&st, fams, 1, 40.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "A", &st, &lay));
    scene s;
        SDL_memset(&s, 0, sizeof s);
        s.t = t;
        s.lay = &lay;
        s.sc = &sc;
    EMB_REQUIRE(embi_text_line(&lay, 0, lay.text_len, &s.vr));
    uint8_t *px = render(&s, GW, GH);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_EQ_INT(sc.calls, 0);
    EMB_CHECK(!near_rgb(px, 30, 35, 255, 255, 0)); /* no yellow bitmap: the face's plain outline (a dark box) is drawn */
    SDL_free(px);
    embi_text_vruns_free(&s.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
