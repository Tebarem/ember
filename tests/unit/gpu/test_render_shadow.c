#include "emb_render_test.h"

#include <math.h>

#include "render_check.h"

/* Analytic box shadows (embi_frame_shadow): a Gaussian-blurred rounded rect in closed form, against a CPU reference
 * (the exact erf product for a rect, a supersampled direct Gaussian for rounded corners), the cut-out of the border
 * box (outer), the inset polarity, and a plain AA rect when sigma is 0. Black shadows on a transparent target, so the
 * alpha channel is the coverage. */

#define W 128
#define H 112

#define NO_RADII ((float[4]){0.0f, 0.0f, 0.0f, 0.0f})

typedef struct shadow_scene {
    float quad[4];
    float shape[4];
    float shape_radii[4];
    float clip[4];
    float clip_radii[4];
    float sigma;
    bool inset;
    uint32_t color;
} shadow_scene;

static void scene_shadow(embi_frame *f, void *user)
{
    const shadow_scene *s = user;
    embi_frame_shadow(f, s->quad, s->shape, s->shape_radii, s->clip, s->clip_radii, s->sigma, s->inset, s->color, 0,
                      0);
}

static shadow_scene outer_scene(const float shape[4], const float radii[4], float sigma)
{
    shadow_scene s = {0};
    float reach = 3.5f * sigma + 2.0f;
    s.quad[0] = shape[0] - reach;
    s.quad[1] = shape[1] - reach;
    s.quad[2] = shape[2] + 2 * reach;
    s.quad[3] = shape[3] + 2 * reach;
    for (int i = 0; i < 4; i++) {
        s.shape[i] = shape[i];
        s.shape_radii[i] = radii[i];
    }
    s.sigma = sigma;
    s.color = RGBA(0, 0, 0, 255);
    return s;
}

static double gauss_mass(double a, double b, double p, double sigma)
{
    double inv = 1.0 / (sigma * 1.4142135623730951);
    return 0.5 * (erf((b - p) * inv) - erf((a - p) * inv));
}

/* The exact blurred coverage of a plain rect at pixel (x, y) (center x + 0.5). */
static double rect_ref(const float r[4], double sigma, int x, int y)
{
    return gauss_mass(r[0], r[0] + r[2], x + 0.5, sigma) * gauss_mass(r[1], r[1] + r[3], y + 0.5, sigma);
}

/* Whether the point is inside the rounded rect (radii tl, tr, br, bl). */
static bool inside_rrect(double x, double y, const float r[4], const float rad[4])
{
    if (x < r[0] || x > r[0] + r[2] || y < r[1] || y > r[1] + r[3]) {
        return false;
    }
    const double cx[4] = {r[0] + rad[0], r[0] + r[2] - rad[1], r[0] + r[2] - rad[2], r[0] + rad[3]};
    const double cy[4] = {r[1] + rad[0], r[1] + rad[1], r[1] + r[3] - rad[2], r[1] + r[3] - rad[3]};
    for (int i = 0; i < 4; i++) {
        bool in_x = (i == 0 || i == 3) ? x < cx[i] : x > cx[i];
        bool in_y = (i < 2) ? y < cy[i] : y > cy[i];
        if (in_x && in_y && hypot(x - cx[i], y - cy[i]) > (double)rad[i]) {
            return false;
        }
    }
    return true;
}

/* The blurred coverage of a rounded rect by direct summation over a 4x4 supersampled window. */
static double rrect_ref(const float r[4], const float rad[4], double sigma, int x, int y)
{
    const int ss = 4;
    int reach = (int)ceil(4.0 * sigma);
    double sum = 0.0, wsum = 0.0;
    for (int j = -reach * ss; j < reach * ss; j++) {
        for (int i = -reach * ss; i < reach * ss; i++) {
            double dx = (i + 0.5) / ss, dy = (j + 0.5) / ss; /* midpoint samples */
            double w = exp(-0.5 * (dx * dx + dy * dy) / (sigma * sigma));
            wsum += w;
            if (inside_rrect(x + 0.5 + dx, y + 0.5 + dy, r, rad)) {
                sum += w;
            }
        }
    }
    return sum / wsum;
}

static int alpha_worst(const uint8_t *px, double (*ref)(const void *, int, int), const void *ctx)
{
    int worst = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double want = ref(ctx, x, y) * 255.0;
            int d = (int)lround(fabs((double)px[((size_t)y * W + (size_t)x) * 4 + 3] - want));
            worst = d > worst ? d : worst;
        }
    }
    return worst;
}

typedef struct ref_ctx {
    float shape[4];
    float radii[4];
    float sigma;
} ref_ctx;

static double ref_rect_fn(const void *c, int x, int y)
{
    const ref_ctx *r = c;
    return rect_ref(r->shape, r->sigma, x, y);
}

static double ref_rrect_fn(const void *c, int x, int y)
{
    const ref_ctx *r = c;
    return rrect_ref(r->shape, r->radii, r->sigma, x, y);
}

EMB_TEST(shadow_of_a_rect_is_the_erf_product)
{
    const float shape[4] = {32, 28, 56, 40};
    shadow_scene s = outer_scene(shape, NO_RADII, 5.0f);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    ref_ctx c = {{32, 28, 56, 40}, {0, 0, 0, 0}, 5.0f};
    int worst = alpha_worst(px, ref_rect_fn, &c);
    EMB_CHECK(worst <= 2);
    if (worst > 2) {
        char msg[80];
        SDL_snprintf(msg, sizeof msg, "rect shadow differs from erf by %d", worst);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    /* The color is the shadow color, premultiplied by the coverage: black stays black. */
    EMB_CHECK(px[((size_t)48 * W + 60) * 4 + 0] == 0);
    SDL_free(px);
}

EMB_TEST(shadow_of_a_rounded_rect_matches_direct_summation)
{
    const float shape[4] = {30, 24, 64, 56};
    const float radii[4] = {20, 8, 16, 0};
    shadow_scene s = outer_scene(shape, radii, 4.0f);
    for (int i = 0; i < 4; i++) {
        s.shape_radii[i] = radii[i];
    }
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    ref_ctx c = {{30, 24, 64, 56}, {20, 8, 16, 0}, 4.0f};
    int worst = alpha_worst(px, ref_rrect_fn, &c);
    if (worst > 4) {
        char msg[80];
        SDL_snprintf(msg, sizeof msg, "rounded shadow differs from the reference by %d", worst);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(px);
}

EMB_TEST(shadow_with_a_pill_shape_and_big_sigma)
{
    const float shape[4] = {24, 30, 80, 40};
    const float radii[4] = {20, 20, 20, 20};
    shadow_scene s = outer_scene(shape, radii, 10.0f);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    ref_ctx c = {{24, 30, 80, 40}, {20, 20, 20, 20}, 10.0f};
    int worst = alpha_worst(px, ref_rrect_fn, &c);
    if (worst > 5) {
        char msg[80];
        SDL_snprintf(msg, sizeof msg, "pill shadow differs from the reference by %d", worst);
        emb_test_report_fail(__FILE__, __LINE__, msg);
    }
    SDL_free(px);
}

EMB_TEST(outer_shadow_is_cut_out_under_the_box)
{
    /* The box (40, 30, 48, 40) casts a shadow moved by (6, 6) with blur: nothing is drawn inside the box. */
    const float box[4] = {40, 30, 48, 40};
    const float moved[4] = {46, 36, 48, 40};
    shadow_scene s = outer_scene(moved, NO_RADII, 3.0f);
    SDL_memcpy(s.clip, box, sizeof box);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 60, 50, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 41, 31, 0, 0, 0, 0);
    /* Right of the box (it ends at x = 88) but inside the moved shadow rect (to x = 94) the shadow is there. */
    EMB_CHECK(px[((size_t)50 * W + 90) * 4 + 3] > 200);
    /* Far outside it is clear. */
    EMB_CHECK_PIXEL(px, W, 5, 5, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(inset_shadow_darkens_the_edges_of_the_padding_box)
{
    /* Padding box (20, 16, 88, 80); the hole is the padding box shifted by (8, 8): the shadow hugs the top and left. */
    shadow_scene s = {0};
    const float pb[4] = {20, 16, 88, 80};
    const float hole[4] = {28, 24, 88, 80};
    SDL_memcpy(s.quad, pb, sizeof pb);
    SDL_memcpy(s.clip, pb, sizeof pb);
    SDL_memcpy(s.shape, hole, sizeof hole);
    s.sigma = 3.0f;
    s.inset = true;
    s.color = RGBA(0, 0, 0, 255);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    /* Outside the padding box nothing. */
    EMB_CHECK_PIXEL(px, W, 10, 40, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 60, 100, 0, 0, 0, 0);
    /* Deep in the hole nothing. */
    CHECK_PX_NEAR(px, W, 70, 60, 0, 0, 0, 0, 2);
    /* In the strip between the padding edge and the hole it is nearly full: x = 22 is 6 px left of the hole's edge. */
    EMB_CHECK(px[((size_t)60 * W + 22) * 4 + 3] > 200);
    /* At the hole's edge x = 28 the shadow is the complement of the blurred hole: the pixel centers 0.5 px either
     * side of the edge are at Phi(+-0.5 / 3) = 0.566 and 0.434 of the hole, so alpha 111 and 144. */
    CHECK_PX_NEAR(px, W, 28, 60, 0, 0, 0, 111, 4);
    CHECK_PX_NEAR(px, W, 27, 60, 0, 0, 0, 144, 4);
    SDL_free(px);
}

EMB_TEST(inset_shadow_without_a_hole_fills_the_padding_box)
{
    shadow_scene s = {0};
    const float pb[4] = {20, 16, 60, 50};
    SDL_memcpy(s.quad, pb, sizeof pb);
    SDL_memcpy(s.clip, pb, sizeof pb);
    s.sigma = 2.0f;
    s.inset = true;
    s.color = RGBA(255, 0, 0, 255);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 50, 40, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 10, 40, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(shadow_without_blur_is_a_plain_rect)
{
    const float shape[4] = {32, 24, 48, 40};
    shadow_scene s = outer_scene(shape, NO_RADII, 0.0f);
    s.color = RGBA(0, 128, 0, 255);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 24, 0, 128, 0, 255);
    EMB_CHECK_PIXEL(px, W, 79, 63, 0, 128, 0, 255);
    EMB_CHECK_PIXEL(px, W, 31, 24, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 80, 40, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 50, 64, 0, 0, 0, 0);
    SDL_free(px);
}

EMB_TEST(shadow_color_alpha_scales_the_coverage)
{
    const float shape[4] = {32, 24, 48, 40};
    shadow_scene s = outer_scene(shape, NO_RADII, 0.0f);
    s.color = RGBA(255, 0, 0, 128);
    uint8_t *px = emb_test_render(W, H, scene_shadow, &s);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, W, 50, 40, 128, 0, 0, 128, 1);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
