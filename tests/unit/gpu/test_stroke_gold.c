#include "emb_render_test.h"

#include "render_check.h"

#include "gpu/stroke.h"

/* Strokes through the renderer: a few pixel checks of the filled stroke, and a golden image of
 * caps, joins, curves, dashes and hairlines. The geometry itself is checked on the CPU in
 * test_stroke.c. */

#define GOLD_W 320
#define GOLD_H 200

static void stroke(embi_frame *f, embi_path *p, const embi_stroke_style *st, uint32_t rgba)
{
    const embi_paint paint = embi_paint_solid(rgba);
    embi_frame_stroke_path(f, p, st, 0.0f, &paint, 0, 0);
}

static embi_path *polyline(const float *pt, int n, bool closed)
{
    embi_path *p = embi_path_create(0.0f);
    embi_path_move_to(p, pt[0], pt[1]);
    for (int i = 1; i < n; i++) {
        embi_path_line_to(p, pt[2 * i], pt[2 * i + 1]);
    }
    if (closed) {
        embi_path_close(p);
    }
    return p;
}

static void scene_basic(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = polyline((const float[]){8, 16, 56, 16}, 2, false);
    embi_stroke_style st = {.width = 8, .cap = EMBI_CAP_BUTT};
    stroke(f, p, &st, RGBA(255, 0, 0, 255));
    embi_path_destroy(p);
}

EMB_TEST(stroke_fills_pixels)
{
    uint8_t *px = emb_test_render(64, 32, scene_basic, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 64, 30, 16, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 64, 9, 13, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 64, 30, 10, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 64, 30, 22, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 64, 5, 16, 0, 0, 0, 0);  /* butt: nothing before the start */
    EMB_CHECK_PIXEL(px, 64, 59, 16, 0, 0, 0, 0); /* nor past the end */
    SDL_free(px);
}

static void scene_hairline(embi_frame *f, void *user)
{
    (void)user;
    embi_path *p = polyline((const float[]){4, 8.5f, 60, 8.5f}, 2, false);
    embi_stroke_style st = {.width = 0.25f, .scale = 1.0f, .min_device_width = 1.0f};
    stroke(f, p, &st, RGBA(0, 0, 255, 255));
    embi_path_destroy(p);
}

EMB_TEST(stroke_hairline_is_one_pixel_at_reduced_alpha)
{
    uint8_t *px = emb_test_render(64, 16, scene_hairline, nullptr);
    EMB_RENDER_OR_SKIP(px);
    /* A 1 px wide line centered on row 8 covers it fully at alpha 0.25 (64 of 255). */
    const uint8_t *p = px + (8u * 64u + 30u) * 4u;
    EMB_CHECK(p[3] >= 60 && p[3] <= 68);
    EMB_CHECK(p[2] > 0);
    const uint8_t *q = px + (6u * 64u + 30u) * 4u;
    EMB_CHECK(q[3] == 0);
    SDL_free(px);
}

static void scene_gold(embi_frame *f, void *user)
{
    (void)user;
    const uint32_t ink = RGBA(30, 60, 160, 255);
    const uint32_t red = RGBA(200, 50, 40, 255);

    /* Row 1: caps on a line, then joins on a zigzag. */
    for (int c = 0; c < 3; c++) {
        embi_path *p = polyline((const float[]){14, 20.0f + 22.0f * (float)c, 62, 20.0f + 22.0f * (float)c},
                                2, false);
        embi_stroke_style st = {.width = 12, .cap = (embi_stroke_cap)c};
        stroke(f, p, &st, ink);
        embi_stroke_style thin = {.width = 1};
        stroke(f, p, &thin, RGBA(255, 255, 255, 255));
        embi_path_destroy(p);
    }
    for (int j = 0; j < 3; j++) {
        const float x0 = 90.0f + 76.0f * (float)j;
        embi_path *p = polyline((const float[]){x0, 70, x0 + 18, 12, x0 + 36, 70, x0 + 54, 12}, 4, false);
        embi_stroke_style st = {.width = 10, .cap = EMBI_CAP_BUTT, .join = (embi_stroke_join)j,
                                .miter_limit = 4};
        stroke(f, p, &st, ink);
        embi_path_destroy(p);
    }

    /* Row 2: a circle, an S curve with round caps, a tight arc with a wide stroke, a cusp. */
    {
        embi_path *p = embi_path_create(0.0f);
        embi_path_move_to(p, 14, 120);
        embi_path_arc_to(p, 26, 26, 0, false, true, 66, 120);
        embi_path_arc_to(p, 26, 26, 0, false, true, 14, 120);
        embi_path_close(p);
        embi_stroke_style st = {.width = 8};
        stroke(f, p, &st, red);
        embi_path_destroy(p);
    }
    {
        embi_path *p = embi_path_create(0.0f);
        embi_path_move_to(p, 84, 100);
        embi_path_cubic_to(p, 150, 84, 90, 150, 150, 140);
        embi_stroke_style st = {.width = 7, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
        stroke(f, p, &st, ink);
        embi_path_destroy(p);
    }
    {
        embi_path *p = embi_path_create(0.0f);
        embi_path_move_to(p, 176, 140);
        embi_path_quad_to(p, 190, 90, 204, 140);
        embi_stroke_style st = {.width = 30, .cap = EMBI_CAP_BUTT};
        stroke(f, p, &st, RGBA(60, 160, 80, 255));
        embi_path_destroy(p);
    }
    {
        embi_path *p = embi_path_create(0.0f);
        embi_path_move_to(p, 232, 140);
        embi_path_quad_to(p, 290, 140, 262, 140);
        embi_path_move_to(p, 232, 112);
        embi_path_cubic_to(p, 300, 90, 230, 90, 296, 112);
        embi_stroke_style st = {.width = 8, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND};
        stroke(f, p, &st, red);
        embi_path_destroy(p);
    }

    /* Row 3: dashes, dots, a dashed circle, a hairline. */
    {
        embi_path *p = polyline((const float[]){10, 170, 150, 170}, 2, false);
        const float dash[2] = {14, 8};
        embi_stroke_style st = {.width = 8, .cap = EMBI_CAP_BUTT, .dash = dash, .dash_count = 2,
                                .dash_offset = 5};
        stroke(f, p, &st, ink);
        embi_path_destroy(p);
        p = polyline((const float[]){10, 188, 150, 188}, 2, false);
        const float dots[2] = {0, 12};
        st = (embi_stroke_style){.width = 7, .cap = EMBI_CAP_ROUND, .dash = dots, .dash_count = 2};
        stroke(f, p, &st, red);
        embi_path_destroy(p);
    }
    {
        embi_path *p = embi_path_create(0.0f);
        embi_path_move_to(p, 190, 172);
        embi_path_arc_to(p, 18, 18, 0, false, true, 226, 172);
        embi_path_arc_to(p, 18, 18, 0, false, true, 190, 172);
        embi_path_close(p);
        const float dash[2] = {9, 6};
        embi_stroke_style st = {.width = 5, .cap = EMBI_CAP_ROUND, .join = EMBI_JOIN_ROUND,
                                .dash = dash, .dash_count = 2};
        stroke(f, p, &st, ink);
        embi_path_destroy(p);
    }
    {
        embi_path *p = polyline((const float[]){250, 160, 310, 160, 280, 196}, 3, true);
        embi_stroke_style st = {.width = 3, .join = EMBI_JOIN_MITER, .miter_limit = 10};
        stroke(f, p, &st, ink);
        embi_stroke_style hair = {.width = 0.2f, .scale = 1.0f, .min_device_width = 1.0f};
        embi_path_destroy(p);
        p = polyline((const float[]){252, 190, 308, 166}, 2, false);
        stroke(f, p, &hair, RGBA(0, 0, 0, 255));
        embi_path_destroy(p);
    }
}

EMB_TEST(stroke_golden_sheet)
{
    uint8_t *px = emb_test_render(GOLD_W, GOLD_H, scene_gold, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK(emb_gold_check("stroke_sheet", px, GOLD_W, GOLD_H));
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
