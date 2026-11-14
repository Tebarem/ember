#include "layer_rig.h"

#include <math.h>
#include <stdlib.h>

#include "base/base.h"
#include "gpu/fx.h"

/* The filter effect passes (gpu/fx.h): Gaussian blur against a CPU reference, the color matrix, the
 * drop-shadow composite, the downsampled blur of big sigmas, and that the work stays bounded. Layers are
 * composited into a target of exactly their size with the identity transform, which copies the texels, so a
 * downloaded target is the layer. */

#define ZERO_RADII ((float[4]){0.0f, 0.0f, 0.0f, 0.0f})

typedef struct fx_rig {
    layer_rig k;
    embi_fx *fx;
    SDL_GPUCommandBuffer *cmd;
} fx_rig;

static bool fx_rig_open(fx_rig *r)
{
    *r = (fx_rig){0};
    if (!layer_rig_open(&r->k)) {
        return false;
    }
    r->fx = embi_fx_create(r->k.g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, r->k.pool);
    r->cmd = SDL_AcquireGPUCommandBuffer(r->k.dev);
    if (r->fx == nullptr || r->cmd == nullptr) {
        embi_fx_destroy(r->fx);
        layer_rig_close(&r->k);
        return false;
    }
    return true;
}

static void fx_rig_close(fx_rig *r)
{
    embi_fx_destroy(r->fx);
    layer_rig_close(&r->k);
}

static void fill(embi_frame *f, float x, float y, float w, float h, uint32_t rgba)
{
    const float r[4] = {x, y, w, h};
    embi_frame_fill(f, r, ZERO_RADII, rgba, 0, 0);
}

/* A layer of w x h holding the test picture: an opaque red block and a half transparent blue block. */
static bool make_picture(fx_rig *r, uint32_t w, uint32_t h, embi_layer *L)
{
    if (!embi_layer_acquire(r->k.pool, w, h, L)) {
        return false;
    }
    embi_frame *f = embi_frame_begin(r->k.r, L->tex_w, L->tex_h, 1.0f);
    if (f == nullptr) {
        return false;
    }
    fill(f, (float)(w / 4), (float)(h / 4), (float)(w / 2), (float)(h / 2), RGBA(255, 0, 0, 255));
    fill(f, (float)(w / 2), (float)(h / 8), (float)(w / 4), (float)(h / 4), RGBA(0, 0, 255, 128));
    embi_frame_end(f, r->cmd, L->tex, true, 0);
    return true;
}

/* Composites L into a w x h target and records its download. */
static layer_dl grab(fx_rig *r, const embi_layer *L, SDL_GPUTexture **target_out)
{
    SDL_GPUTexture *target = layer_rig_target(&r->k, L->w, L->h);
    *target_out = target;
    embi_frame *f = embi_frame_begin(r->k.r, L->w, L->h, 1.0f);
    const float src[4] = {0, 0, (float)L->w, (float)L->h};
    embi_frame_layer(f, L, src, src, ZERO_RADII, 1.0f, 0, 0);
    embi_frame_end(f, r->cmd, target, true, 0);
    return layer_rig_download(&r->k, r->cmd, target, L->w, L->h);
}

/* CPU reference: separable Gaussian of the premultiplied bytes, transparent outside, the kernel normalized over all
 * its taps. */
static void ref_blur(const uint8_t *in, uint32_t w, uint32_t h, double sigma, int max_radius, double *out)
{
    int radius = (int)ceil(3.0 * sigma);
    if (radius < 1) {
        radius = 1;
    }
    if (max_radius > 0 && radius > max_radius) {
        radius = max_radius;
    }
    double *k = SDL_malloc((size_t)(2 * radius + 1) * sizeof *k);
    double ksum = 0.0;
    for (int i = -radius; i <= radius; i++) {
        k[i + radius] = exp(-0.5 * (double)(i * i) / (sigma * sigma));
        ksum += k[i + radius];
    }
    double *tmp = SDL_calloc((size_t)w * h * 4, sizeof *tmp);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                double s = 0.0;
                for (int i = -radius; i <= radius; i++) {
                    int xx = (int)x + i;
                    if (xx >= 0 && xx < (int)w) {
                        s += k[i + radius] * (double)in[((size_t)y * w + (uint32_t)xx) * 4 + (uint32_t)c];
                    }
                }
                tmp[((size_t)y * w + x) * 4 + (uint32_t)c] = s / ksum;
            }
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                double s = 0.0;
                for (int i = -radius; i <= radius; i++) {
                    int yy = (int)y + i;
                    if (yy >= 0 && yy < (int)h) {
                        s += k[i + radius] * tmp[((size_t)(uint32_t)yy * w + x) * 4 + (uint32_t)c];
                    }
                }
                out[((size_t)y * w + x) * 4 + (uint32_t)c] = s / ksum;
            }
        }
    }
    SDL_free(k);
    SDL_free(tmp);
}

static int max_diff(const uint8_t *px, const double *ref, size_t n)
{
    int worst = 0;
    for (size_t i = 0; i < n; i++) {
        int d = (int)lround(fabs((double)px[i] - ref[i]));
        if (d > worst) {
            worst = d;
        }
    }
    return worst;
}

static void blur_case(uint32_t w, uint32_t h, float sigma, int tol, int max_radius)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer src, dst;
    EMB_REQUIRE(make_picture(&r, w, h, &src));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &dst));
    SDL_GPUTexture *t0, *t1;
    layer_dl d0 = grab(&r, &src, &t0);
    EMB_REQUIRE(embi_fx_blur(r.fx, r.cmd, &src, &dst, sigma));
    layer_dl d1 = grab(&r, &dst, &t1);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *p0 = layer_rig_map(&r.k, d0);
    uint8_t *p1 = layer_rig_map(&r.k, d1);
    EMB_REQUIRE(p0 != nullptr && p1 != nullptr);
    double *ref = SDL_malloc((size_t)w * h * 4 * sizeof *ref);
    ref_blur(p0, w, h, (double)sigma, max_radius, ref);
    int worst = max_diff(p1, ref, (size_t)w * h * 4);
    if (worst > tol) {
        char msg[120];
        SDL_snprintf(msg, sizeof msg, "blur sigma %.1f on %ux%u: max difference %d > %d", (double)sigma, (unsigned)w,
                     (unsigned)h, worst, tol);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(ref);
    SDL_free(p0);
    SDL_free(p1);
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t0);
    SDL_ReleaseGPUTexture(r.k.dev, t1);
    fx_rig_close(&r);
}

EMB_TEST(blur_small_sigma_matches_the_reference)
{
    blur_case(64, 48, 1.0f, 2, 32);
}

EMB_TEST(blur_medium_sigma_matches_the_reference)
{
    blur_case(96, 72, 4.0f, 2, 32);
}

EMB_TEST(blur_direct_limit_sigma_matches_the_reference)
{
    blur_case(96, 72, 8.0f, 2, 32);
}

EMB_TEST(blur_big_sigma_downsampled_stays_close_to_the_reference)
{
    /* Sigma 12 runs at half size (sigma 6) and 24 at a quarter (sigma 6): box downsampling and bilinear upsampling
     * add a small error over the exact Gaussian. */
    blur_case(192, 160, 12.0f, 4, 0);
    blur_case(192, 160, 24.0f, 5, 0);
}

EMB_TEST(blur_odd_sizes_and_edges_fade_out)
{
    blur_case(37, 29, 3.0f, 2, 32);
    blur_case(130, 67, 16.0f, 4, 0);
}

EMB_TEST(blur_work_is_bounded_beyond_the_direct_sigma)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer src, dst;
    EMB_REQUIRE(make_picture(&r, 256, 256, &src));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 256, 256, &dst));
    EMB_REQUIRE(embi_fx_blur(r.fx, r.cmd, &src, &dst, 8.0f));
    uint32_t direct = embi_fx_last_passes(r.fx);
    EMB_REQUIRE(embi_fx_blur(r.fx, r.cmd, &src, &dst, 64.0f));
    uint32_t big = embi_fx_last_passes(r.fx);
    EMB_REQUIRE(embi_fx_blur(r.fx, r.cmd, &src, &dst, 512.0f));
    uint32_t huge = embi_fx_last_passes(r.fx);
    EMB_CHECK_EQ_INT(direct, 2);
    /* sigma 64 = 3 halvings, 512 = 6: passes grow with log2(sigma) only (down, 2 blur passes, up). */
    EMB_CHECK_EQ_INT(big, 3 + 2 + 1);
    EMB_CHECK_EQ_INT(huge, 6 + 2 + 1);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &dst);
    fx_rig_close(&r);
}

/* ------------------------------------------------------------------ color matrix */

static void matrix_case(const float m[20], void (*ref)(const uint8_t *in, uint8_t *out), int tol, const char *name)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const uint32_t w = 64, h = 48;
    embi_layer src, dst;
    EMB_REQUIRE(make_picture(&r, w, h, &src));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &dst));
    SDL_GPUTexture *t0, *t1;
    layer_dl d0 = grab(&r, &src, &t0);
    EMB_REQUIRE(embi_fx_matrix(r.fx, r.cmd, &src, &dst, m));
    layer_dl d1 = grab(&r, &dst, &t1);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *p0 = layer_rig_map(&r.k, d0);
    uint8_t *p1 = layer_rig_map(&r.k, d1);
    EMB_REQUIRE(p0 != nullptr && p1 != nullptr);
    int worst = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t want[4];
        ref(p0 + i * 4, want);
        for (int c = 0; c < 4; c++) {
            int d = abs((int)p1[i * 4 + (size_t)c] - (int)want[c]);
            worst = d > worst ? d : worst;
        }
    }
    if (worst > tol) {
        char msg[120];
        SDL_snprintf(msg, sizeof msg, "%s: max difference %d > %d", name, worst, tol);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(p0);
    SDL_free(p1);
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t0);
    SDL_ReleaseGPUTexture(r.k.dev, t1);
    fx_rig_close(&r);
}

static void ref_identity(const uint8_t *in, uint8_t *out)
{
    SDL_memcpy(out, in, 4);
}

/* invert(1) on unpremultiplied colors: 1 - c, alpha kept, premultiplied again. */
static void ref_invert(const uint8_t *in, uint8_t *out)
{
    double a = in[3] / 255.0;
    for (int c = 0; c < 3; c++) {
        double u = a > 0.0 ? (in[c] / 255.0) / a : 0.0;
        out[c] = (uint8_t)lround((1.0 - u) * a * 255.0);
    }
    out[3] = in[3];
}

/* opacity(0.5): alpha halves, the premultiplied colors with it. */
static void ref_half_opacity(const uint8_t *in, uint8_t *out)
{
    for (int c = 0; c < 4; c++) {
        out[c] = (uint8_t)lround(in[c] * 0.5);
    }
}

/* brightness(2) clamps: a channel above 1 saturates. */
static void ref_bright2(const uint8_t *in, uint8_t *out)
{
    double a = in[3] / 255.0;
    for (int c = 0; c < 3; c++) {
        double u = a > 0.0 ? (in[c] / 255.0) / a : 0.0;
        u = u * 2.0 > 1.0 ? 1.0 : u * 2.0;
        out[c] = (uint8_t)lround(u * a * 255.0);
    }
    out[3] = in[3];
}

EMB_TEST(matrix_identity_keeps_the_picture)
{
    const float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    matrix_case(m, ref_identity, 2, "identity");
}

EMB_TEST(matrix_invert_flips_the_unpremultiplied_colors)
{
    const float m[20] = {-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0};
    matrix_case(m, ref_invert, 2, "invert");
}

EMB_TEST(matrix_alpha_scale_is_opacity)
{
    const float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.5f, 0};
    matrix_case(m, ref_half_opacity, 2, "opacity");
}

EMB_TEST(matrix_results_are_clamped)
{
    const float m[20] = {2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0};
    matrix_case(m, ref_bright2, 2, "brightness 2");
}

/* ------------------------------------------------------------------ drop shadow */

EMB_TEST(drop_shadow_draws_the_tinted_blurred_alpha_under_the_source)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const uint32_t w = 64, h = 64;
    embi_layer src, blurred, dst;
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &src));
    embi_frame *f = embi_frame_begin(r.k.r, src.tex_w, src.tex_h, 1.0f);
    fill(f, 8, 8, 16, 16, RGBA(255, 0, 0, 255));
    embi_frame_end(f, r.cmd, src.tex, true, 0);
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &blurred));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &dst));
    EMB_REQUIRE(embi_fx_blur(r.fx, r.cmd, &src, &blurred, 2.0f));
    EMB_REQUIRE(embi_fx_drop_shadow(r.fx, r.cmd, &src, &blurred, &dst, 20.0f, 20.0f, RGBA(0, 0, 255, 204)));
    SDL_GPUTexture *t;
    layer_dl d = grab(&r, &dst, &t);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *px = layer_rig_map(&r.k, d);
    EMB_REQUIRE(px != nullptr);
    /* The source is untouched where it is opaque. */
    EMB_CHECK_PIXEL(px, w, 16, 16, 255, 0, 0, 255);
    /* Inside the shifted square (28..44) the shadow is the tint at 0.8 alpha, premultiplied: (0, 0, 204, 204). */
    CHECK_PX_NEAR(px, w, 36, 36, 0, 0, 204, 204, 3);
    /* Far from both it is transparent. */
    EMB_CHECK_PIXEL(px, w, 60, 4, 0, 0, 0, 0);
    /* The pixel center 0.5 px inside the shifted square's edge: the blurred alpha is Phi(0.25) = 0.599, times 0.8. */
    CHECK_PX_NEAR(px, w, 28, 36, 0, 0, 122, 122, 5);
    SDL_free(px);
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &blurred);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t);
    fx_rig_close(&r);
}

/* A layer filled with one opaque colour, blurred with edge mode duplicate (clamp) or transparent edges. */
static void edge_case(bool clamp, bool *border_kept)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    const uint32_t w = 48, h = 48;
    embi_layer src, dst;
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &src));
    embi_frame *f = embi_frame_begin(r.k.r, src.tex_w, src.tex_h, 1.0f);
    fill(f, 0, 0, (float)w, (float)h, RGBA(255, 0, 0, 255));
    embi_frame_end(f, r.cmd, src.tex, true, 0);
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, w, h, &dst));
    EMB_REQUIRE(embi_fx_blur_edge(r.fx, r.cmd, &src, &dst, 4.0f, clamp));
    SDL_GPUTexture *t;
    layer_dl d = grab(&r, &dst, &t);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    uint8_t *px = layer_rig_map(&r.k, d);
    EMB_REQUIRE(px != nullptr);
    if (clamp) {
        EMB_CHECK_PIXEL(px, w, 0, 0, 255, 0, 0, 255);
        EMB_CHECK_PIXEL(px, w, w - 1, 20, 255, 0, 0, 255);
        EMB_CHECK_PIXEL(px, w, 20, h - 1, 255, 0, 0, 255);
    } else {
        /* The corner sees a quarter of the kernel mass: well below opaque. */
        EMB_CHECK(px[3] < 160);
        EMB_CHECK(px[(((size_t)20 * w) + 0) * 4 + 3] < 220);
        EMB_CHECK_PIXEL(px, w, 24, 24, 255, 0, 0, 255);
    }
    *border_kept = px[3] == 255;
    SDL_free(px);
    embi_layer_release(r.k.pool, &src);
    embi_layer_release(r.k.pool, &dst);
    SDL_ReleaseGPUTexture(r.k.dev, t);
    fx_rig_close(&r);
}

EMB_TEST(blur_edge_duplicate_keeps_a_uniform_layer_opaque_to_the_border)
{
    size_t live = embi_mem_live();
    bool kept = false;
    edge_case(true, &kept);
    EMB_CHECK(kept);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(blur_without_edge_duplicate_fades_out_at_the_border)
{
    size_t live = embi_mem_live();
    bool kept = true;
    edge_case(false, &kept);
    EMB_CHECK(!kept);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(fx_rejects_bad_arguments)
{
    fx_rig r;
    if (!fx_rig_open(&r)) {
        EMB_SKIP("no GPU device");
        return;
    }
    embi_layer a, b;
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 32, 32, &a));
    EMB_REQUIRE(embi_layer_acquire(r.k.pool, 16, 16, &b));
    EMB_CHECK(!embi_fx_blur(r.fx, r.cmd, &a, &a, 2.0f));
    EMB_CHECK(!embi_fx_blur(r.fx, r.cmd, &a, &b, 2.0f));
    const float m[20] = {0};
    EMB_CHECK(!embi_fx_matrix(r.fx, r.cmd, &a, &b, m));
    EMB_CHECK(!embi_fx_matrix(r.fx, r.cmd, &a, &a, m));
    embi_layer_release(r.k.pool, &a);
    embi_layer_release(r.k.pool, &b);
    EMB_REQUIRE(layer_rig_submit(&r.k, r.cmd));
    fx_rig_close(&r);
}

EMB_RENDER_TEST_MAIN()
