#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <SDL3/SDL_stdinc.h>

#include "gpu/path.h"

/* Slug coverage on paths, checked against hand-computed values. Pixel centers are at (x + 0.5,
 * y + 0.5). Expected colors are premultiplied RGBA8 of a solid red or blue fill. */

#define W 64
#define H 64

/* Builds a closed rectangle path. The caller destroys it. */
static embi_path *rect_path(float x0, float y0, float x1, float y1)
{
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, x0, y0);
    embi_path_line_to(p, x1, y0);
    embi_path_line_to(p, x1, y1);
    embi_path_line_to(p, x0, y1);
    embi_path_close(p);
    return p;
}

static void fill_path_solid(embi_frame *f, embi_path *p, embi_fill_rule rule, uint32_t rgba,
                            uint32_t xform, uint32_t clip)
{
    const embi_paint paint = embi_paint_solid(rgba);
    embi_frame_fill_path(f, p, rule, &paint, xform, clip);
}

/* ------------------------------------------------------------------ edges */

static void scene_square(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = rect_path(8.0f, 8.0f, 24.0f, 24.0f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_square_is_exact_on_pixel_edges)
{
    uint8_t *px = emb_test_render(W, H, scene_square, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 8, 8, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 12, 12, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 23, 23, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 7, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 24, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 12, 7, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 12, 24, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 0, 0, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_vertical_half(embi_frame *f, void *user)
{
    (void)user;
    /* Left and right edges at x = 10.5 and 20.5: pixel centers exactly on both. */
    embi_path *p = rect_path(10.5f, 8.0f, 20.5f, 24.0f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_pixel_centered_vertical_edge_has_half_coverage)
{
    uint8_t *px = emb_test_render(W, H, scene_vertical_half, nullptr);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, W, 10, 12, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 9, 12, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 11, 12, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, W, 20, 12, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 21, 12, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_horizontal_half(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = rect_path(8.0f, 10.5f, 24.0f, 20.5f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_pixel_centered_horizontal_edge_has_half_coverage)
{
    uint8_t *px = emb_test_render(W, H, scene_horizontal_half, nullptr);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, W, 12, 10, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 12, 9, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 12, 11, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, W, 12, 20, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 12, 21, 0, 0, 0, 0);
    SDL_free(px);
}

static void scene_diagonal(embi_frame *f, void *user)
{
    (void)user;
    /* The triangle below the diagonal y = x: its edge passes through the centers of the pixels on
     * the diagonal, which therefore have half coverage by symmetry. */
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, 0.0f, 0.0f);
    embi_path_line_to(p, 64.0f, 0.0f);
    embi_path_line_to(p, 64.0f, 64.0f);
    embi_path_close(p);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(0, 0, 255, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_diagonal_edge_has_half_coverage_on_the_diagonal)
{
    uint8_t *px = emb_test_render(W, H, scene_diagonal, nullptr);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, W, 10, 10, 0, 0, 128, 128, 2);
    CHECK_PX_NEAR(px, W, 30, 30, 0, 0, 128, 128, 2);
    EMB_CHECK_PIXEL(px, W, 5, 20, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 40, 10, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 63, 0, 0, 0, 255, 255);
    SDL_free(px);
}

/* ------------------------------------------------------------------ curves */

static void scene_circle(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, 12.0f, 32.0f);
    embi_path_arc_to(p, 20.0f, 20.0f, 0.0f, false, true, 52.0f, 32.0f);
    embi_path_arc_to(p, 20.0f, 20.0f, 0.0f, false, true, 12.0f, 32.0f);
    embi_path_close(p);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_circle_area_is_within_one_percent)
{
    uint8_t *px = emb_test_render(W, H, scene_circle, nullptr);
    EMB_RENDER_OR_SKIP(px);
    const double want = 3.14159265358979 * 20.0 * 20.0;
    const double got = slug_alpha_sum(px, W, H);
    EMB_CHECK(SDL_fabs(got - want) <= want * 0.01);
    /* Center in, corners out. */
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 2, 2, 0, 0, 0, 0);
    SDL_free(px);
}

/* A pentagram: the inner pentagon has winding 2, the tips winding 1. */
static void star_path(embi_path *p)
{
    static const int order[5] = {0, 2, 4, 1, 3};
    for (int i = 0; i < 5; i++) {
        const float a = -1.5707963f + 1.2566371f * (float)order[i];
        const float x = 32.0f + 28.0f * SDL_cosf(a);
        const float y = 32.0f + 28.0f * SDL_sinf(a);
        if (i == 0) {
            embi_path_move_to(p, x, y);
        } else {
            embi_path_line_to(p, x, y);
        }
    }
    embi_path_close(p);
}

static void scene_star(embi_frame *f, void *user)
{
    const embi_fill_rule rule = *(const embi_fill_rule *)user;
    embi_path *p = embi_path_create(0.0f);
    star_path(p);
    fill_path_solid(f, p, rule, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_evenodd_and_nonzero_differ_on_a_star)
{
    embi_fill_rule nz = EMBI_FILL_NONZERO;
    embi_fill_rule eo = EMBI_FILL_EVENODD;
    uint8_t *a = emb_test_render(W, H, scene_star, &nz);
    EMB_RENDER_OR_SKIP(a);
    uint8_t *b = emb_test_render(W, H, scene_star, &eo);
    EMB_REQUIRE(b != nullptr);
    /* The center is inside the inner pentagon (winding 2): filled under nonzero only. */
    EMB_CHECK_PIXEL(a, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(b, W, 32, 32, 0, 0, 0, 0);
    /* The top tip, winding 1: filled under both rules. */
    EMB_CHECK_PIXEL(a, W, 32, 8, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(b, W, 32, 8, 255, 0, 0, 255);
    SDL_free(a);
    SDL_free(b);
}

static void scene_o(embi_frame *f, void *user)
{
    const embi_fill_rule rule = *(const embi_fill_rule *)user;
    embi_path *p = embi_path_create(0.0f);
    /* Outer loop: right, bottom, left, top (quadratics, circumscribed corners). */
    embi_path_move_to(p, 56.0f, 32.0f);
    embi_path_quad_to(p, 56.0f, 56.0f, 32.0f, 56.0f);
    embi_path_quad_to(p, 8.0f, 56.0f, 8.0f, 32.0f);
    embi_path_quad_to(p, 8.0f, 8.0f, 32.0f, 8.0f);
    embi_path_quad_to(p, 56.0f, 8.0f, 56.0f, 32.0f);
    embi_path_close(p);
    /* Inner loop, the opposite orientation: right, top, left, bottom. */
    embi_path_move_to(p, 42.0f, 32.0f);
    embi_path_quad_to(p, 42.0f, 22.0f, 32.0f, 22.0f);
    embi_path_quad_to(p, 22.0f, 22.0f, 22.0f, 32.0f);
    embi_path_quad_to(p, 22.0f, 42.0f, 32.0f, 42.0f);
    embi_path_quad_to(p, 42.0f, 42.0f, 42.0f, 32.0f);
    embi_path_close(p);
    fill_path_solid(f, p, rule, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_quadratic_o_has_a_hole_under_both_rules)
{
    embi_fill_rule nz = EMBI_FILL_NONZERO;
    embi_fill_rule eo = EMBI_FILL_EVENODD;
    uint8_t *a = emb_test_render(W, H, scene_o, &nz);
    EMB_RENDER_OR_SKIP(a);
    uint8_t *b = emb_test_render(W, H, scene_o, &eo);
    EMB_REQUIRE(b != nullptr);
    EMB_CHECK_PIXEL(a, W, 32, 32, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(b, W, 32, 32, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(a, W, 32, 15, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(b, W, 32, 15, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(a, W, 32, 1, 0, 0, 0, 0);
    SDL_free(a);
    SDL_free(b);
}

/* ------------------------------------------------------------------ transforms */

static void scene_rotated(embi_frame *f, void *user)
{
    (void)user;
    /* A 20 px square turned 17 degrees about (32, 32). */
    const float c = SDL_cosf(0.2967060f);
    const float s = SDL_sinf(0.2967060f);
    const float m[6] = {c, s, -s, c, 32.0f, 32.0f};
    const uint32_t x = embi_frame_xform(f, m);
    embi_path *p = rect_path(-10.0f, -10.0f, 10.0f, 10.0f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), x, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_rotated_17_degrees_keeps_area_and_center)
{
    uint8_t *px = emb_test_render(W, H, scene_rotated, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 1, 1, 0, 0, 0, 0);
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, W, H) - 400.0) <= 400.0 * 0.015);
    SDL_free(px);
}

/* ------------------------------------------------------------------ small shapes */

static void scene_text_like(embi_frame *f, void *user)
{
    (void)user;
    /* A 2.5 x 4 px stem and a 40 x 4 px bar, both off the pixel grid. */
    embi_path *stem = rect_path(10.3f, 20.2f, 12.8f, 24.2f);
    embi_path *bar = rect_path(20.4f, 30.3f, 60.4f, 34.3f);
    fill_path_solid(f, stem, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    fill_path_solid(f, bar, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(stem);
    embi_path_destroy(bar);
}

EMB_TEST(slug_four_px_strokes_keep_their_area)
{
    uint8_t *px = emb_test_render(W, H, scene_text_like, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* 2.5 * 4 + 40 * 4 = 170 px of coverage. */
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, W, H) - 170.0) <= 170.0 * 0.01);
    EMB_CHECK_PIXEL(px, W, 40, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 11, 22, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 13, 22, 0, 0, 0, 0);
    SDL_free(px);
}

/* ------------------------------------------------------------------ paint and clip */

static void scene_gradient(embi_frame *f, void *user)
{
    (void)user;
    const embi_stop stops[2] = {
        {.offset = 0.0f, .color = RGBA(255, 0, 0, 255)},
        {.offset = 1.0f, .color = RGBA(0, 0, 255, 255)},
    };
    embi_paint paint = {0};
    paint.kind = EMBI_PRIM_LINEAR;
    paint.g[0] = 12.0f;
    paint.g[1] = 32.0f;
    paint.g[2] = 52.0f;
    paint.g[3] = 32.0f;
    paint.stops = stops;
    paint.n_stops = 2;
    embi_path *p = rect_path(12.0f, 12.0f, 52.0f, 52.0f);
    embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_linear_gradient_paints_by_position)
{
    uint8_t *px = emb_test_render(W, H, scene_gradient, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* t = (x - 12) / 40 at the pixel center: 0.1125 and 0.9125. */
    CHECK_PX_NEAR(px, W, 16, 32, 226, 0, 29, 255, 3);
    CHECK_PX_NEAR(px, W, 48, 32, 22, 0, 233, 255, 3);
    SDL_free(px);
}

static void scene_clipped(embi_frame *f, void *user)
{
    (void)user;
    const float clip_rect[4] = {8.0f, 8.0f, 48.0f, 48.0f};
    const float clip_radii[4] = {20.0f, 20.0f, 20.0f, 20.0f};
    const uint32_t clip = embi_frame_clip(f, clip_rect, clip_radii, 0, 0);
    embi_path *p = rect_path(0.0f, 0.0f, 64.0f, 64.0f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, clip);
    embi_path_destroy(p);
}

EMB_TEST(slug_fill_respects_a_rounded_clip)
{
    uint8_t *px = emb_test_render(W, H, scene_clipped, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 9, 9, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 32, 32, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 8, 32, 255, 0, 0, 255);
    SDL_free(px);
}

/* ------------------------------------------------------------------ mirrored and HiDPI */

static void scene_mirrored(embi_frame *f, void *user)
{
    (void)user;
    /* x' = 64 - x maps the square [8.5, 24.5] to [39.5, 55.5]: half-pixel edges on both sides. */
    const float m[6] = {-1.0f, 0.0f, 0.0f, 1.0f, 64.0f, 0.0f};
    const uint32_t x = embi_frame_xform(f, m);
    embi_path *p = rect_path(8.5f, 8.5f, 24.5f, 24.5f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), x, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_mirrored_transform_keeps_half_pixel_edges)
{
    uint8_t *px = emb_test_render(W, H, scene_mirrored, nullptr);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, W, 39, 16, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 40, 16, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 54, 16, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, W, 55, 16, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, W, 56, 16, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, W, 38, 16, 0, 0, 0, 0);
    /* 16 x 16 px of coverage. */
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, W, H) - 256.0) <= 256.0 * 0.01);
    SDL_free(px);
}

static void scene_hidpi(embi_frame *f, void *user)
{
    (void)user;
    /* At device scale 2 the CSS square [4.25, 12.25] is the device square [8.5, 24.5]. */
    embi_path *p = rect_path(4.25f, 4.25f, 12.25f, 12.25f);
    fill_path_solid(f, p, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_at_device_scale_two_keeps_half_pixel_edges)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, 2 * W, 2 * H)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    slug_rig_frame(&rg, 2.0f, scene_hidpi, nullptr, true, RGBA(0, 0, 0, 0));
    uint8_t *px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    CHECK_PX_NEAR(px, 2 * W, 8, 10, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, 2 * W, 9, 10, 255, 0, 0, 255);
    CHECK_PX_NEAR(px, 2 * W, 24, 10, 128, 0, 0, 128, 2);
    EMB_CHECK_PIXEL(px, 2 * W, 25, 10, 0, 0, 0, 0);
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, 2 * W, 2 * H) - 256.0) <= 256.0 * 0.01);
    SDL_free(px);
    slug_rig_close(&rg);
}

/* ------------------------------------------------------------------ radial and conic paint */

static embi_paint radial_red_blue(void)
{
    static const embi_stop stops[2] = {
        {.offset = 0.0f, .color = RGBA(255, 0, 0, 255)},
        {.offset = 1.0f, .color = RGBA(0, 0, 255, 255)},
    };
    embi_paint paint = {0};
    paint.kind = EMBI_PRIM_RADIAL;
    paint.g[0] = 32.0f; /* center */
    paint.g[1] = 32.0f;
    paint.g[2] = 20.0f; /* radii */
    paint.g[3] = 20.0f;
    paint.stops = stops;
    paint.n_stops = 2;
    return paint;
}

static void scene_radial(embi_frame *f, void *user)
{
    (void)user;
    const embi_paint paint = radial_red_blue();
    embi_path *p = rect_path(12.0f, 12.0f, 52.0f, 52.0f);
    embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_radial_gradient_paints_by_distance)
{
    uint8_t *px = emb_test_render(W, H, scene_radial, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* (32.5, 32.5): distance 0.7071 over r = 20, t = 0.03536: red 246, blue 9. */
    CHECK_PX_NEAR(px, W, 32, 32, 246, 0, 9, 255, 3);
    /* (50.5, 32.5): distance 18.507, t = 0.92535: red 19, blue 236. */
    CHECK_PX_NEAR(px, W, 50, 32, 19, 0, 236, 255, 3);
    SDL_free(px);
}

static void scene_conic(embi_frame *f, void *user)
{
    (void)user;
    static const embi_stop stops[2] = {
        {.offset = 0.0f, .color = RGBA(255, 0, 0, 255)},
        {.offset = 1.0f, .color = RGBA(0, 0, 255, 255)},
    };
    embi_paint paint = {0};
    paint.kind = EMBI_PRIM_CONIC;
    paint.g[0] = 32.0f; /* center */
    paint.g[1] = 32.0f;
    paint.g[2] = 0.0f; /* from_radians */
    paint.stops = stops;
    paint.n_stops = 2;
    embi_path *p = rect_path(12.0f, 12.0f, 52.0f, 52.0f);
    embi_frame_fill_path(f, p, EMBI_FILL_NONZERO, &paint, 0, 0);
    embi_path_destroy(p);
}

EMB_TEST(slug_conic_gradient_sweeps_clockwise_from_the_top)
{
    uint8_t *px = emb_test_render(W, H, scene_conic, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* (50.5, 31.5): clockwise angle from straight up atan(37) = 1.5438 rad, t = 0.24571:
     * red 192, blue 63. */
    CHECK_PX_NEAR(px, W, 50, 31, 192, 0, 63, 255, 3);
    SDL_free(px);
}

/* ------------------------------------------------------------------ order */

static void scene_layers(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float full[4] = {0, 0, 64, 64};
    const float green_rect[4] = {12, 12, 8, 8};
    /* Box blue, slug red, box green on top, slug white on top of that. */
    embi_frame_fill(f, full, zero, RGBA(0, 0, 255, 255), 0, 0);
    embi_path *red = rect_path(8.0f, 8.0f, 24.0f, 24.0f);
    fill_path_solid(f, red, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(red);
    embi_frame_fill(f, green_rect, zero, RGBA(0, 255, 0, 255), 0, 0);
    embi_path *white = rect_path(14.0f, 14.0f, 18.0f, 18.0f);
    fill_path_solid(f, white, EMBI_FILL_NONZERO, RGBA(255, 255, 255, 255), 0, 0);
    embi_path_destroy(white);
}

EMB_TEST(slug_and_box_fills_draw_in_paint_order)
{
    uint8_t *px = emb_test_render(W, H, scene_layers, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, W, 2, 2, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, W, 9, 9, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, W, 13, 13, 0, 255, 0, 255);
    EMB_CHECK_PIXEL(px, W, 15, 15, 255, 255, 255, 255);
    SDL_free(px);
}

/* The whole composite scene against a golden. Every analytic case above passes first. */
static void scene_core(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    const float bg[4] = {0, 0, 64, 64};
    embi_frame_fill(f, bg, zero, RGBA(20, 20, 40, 255), 0, 0);

    embi_path *sq = rect_path(4.0f, 4.0f, 20.0f, 20.0f);
    fill_path_solid(f, sq, EMBI_FILL_NONZERO, RGBA(255, 0, 0, 255), 0, 0);
    embi_path_destroy(sq);

    embi_fill_rule eo = EMBI_FILL_EVENODD;
    embi_path *star = embi_path_create(0.0f);
    star_path(star);
    fill_path_solid(f, star, eo, RGBA(255, 200, 0, 255), 0, 0);
    embi_path_destroy(star);

    embi_path *stem = rect_path(44.3f, 4.2f, 46.8f, 30.2f);
    fill_path_solid(f, stem, EMBI_FILL_NONZERO, RGBA(120, 255, 120, 255), 0, 0);
    embi_path_destroy(stem);

    embi_path *tri = embi_path_create(0.0f);
    embi_path_move_to(tri, 4.0f, 60.0f);
    embi_path_line_to(tri, 30.0f, 36.0f);
    embi_path_line_to(tri, 30.0f, 60.0f);
    embi_path_close(tri);
    fill_path_solid(f, tri, EMBI_FILL_NONZERO, RGBA(80, 160, 255, 255), 0, 0);
    embi_path_destroy(tri);
}

EMB_TEST(slug_core_scene_matches_golden)
{
    uint8_t *px = emb_test_render(W, H, scene_core, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK(emb_gold_check("slug_core_scene", px, W, H));
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
