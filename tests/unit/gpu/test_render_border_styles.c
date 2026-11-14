#include "emb_render_test.h"

#include "render_check.h"

/* CSS border styles, checked pixel by pixel against the analytic layout. Pixel centers are at
 * (x + 0.5, y + 0.5), so every sample is at least half a px from each edge of the pattern, and
 * the expected value is exact (no partial coverage).
 *
 * Dashes are 3 w long with gaps fitted so a whole number of dashes spans the side. Dots are
 * round, with diameter w, centered on the centerline of the side. Double is two lines of w / 3
 * with a gap of w / 3 (solid below 3 px). Groove, ridge, inset and outset use the dark shade of
 * the color, which for red is (171, 0, 0) (see embi_frame_border_styled). */

#define RED RGBA(255, 0, 0, 255)
#define BLUE RGBA(0, 0, 255, 255)

typedef struct bscene {
    float rect[4];
    float radii[4];
    float widths[4];
    uint32_t colors[4];
    uint32_t styles[4];
} bscene;

static void scene_border(embi_frame *f, void *user)
{
    const bscene *s = user;
    embi_frame_border_styled(f, s->rect, s->radii, s->widths, s->colors, s->styles, 0, 0);
}

static void all4(uint32_t out[4], uint32_t v)
{
    out[0] = out[1] = out[2] = out[3] = v;
}

/* Dashed top side, 4 px wide, on a 100 px side. d = 12 and n = round((100 + 12) / 24) = 5, so
 * the period is (100 - 12) / 4 = 22 and the dashes start at 0, 22, 44, 66 and 88. The last one
 * runs to the far corner. */
EMB_TEST(border_dashed_top_fits_five_dashes_to_the_side)
{
    bscene s = {{0, 0, 100, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DASHED);
    uint8_t *px = emb_test_render(100, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 100, 0, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 5, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 11, 1, 255, 0, 0, 255);  /* last px of the first dash */
    EMB_CHECK_PIXEL(px, 100, 12, 1, 0, 0, 0, 0);      /* first gap px */
    EMB_CHECK_PIXEL(px, 100, 21, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 100, 22, 1, 255, 0, 0, 255);  /* second dash starts */
    EMB_CHECK_PIXEL(px, 100, 33, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 34, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 100, 55, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 56, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 100, 70, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 80, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 100, 99, 1, 255, 0, 0, 255);  /* the last dash reaches the corner */
    /* Below the band. */
    EMB_CHECK_PIXEL(px, 100, 5, 4, 0, 0, 0, 0);
    SDL_free(px);
}

/* Dashed left side: its along coordinate runs up from the bottom corner. With H = 100 the same
 * five dashes lie at 0..12, 22..34, 44..56, 66..78 and 88..100 measured from the bottom. */
EMB_TEST(border_dashed_left_runs_up_from_the_bottom_corner)
{
    bscene s = {{0, 0, 40, 100}, {0, 0, 0, 0}, {0, 0, 0, 4}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DASHED);
    uint8_t *px = emb_test_render(40, 100, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 1, 95, 255, 0, 0, 255);  /* 4.5 from the bottom: first dash */
    EMB_CHECK_PIXEL(px, 40, 1, 85, 0, 0, 0, 0);      /* 14.5: gap */
    EMB_CHECK_PIXEL(px, 40, 1, 77, 255, 0, 0, 255);  /* 22.5: second dash */
    EMB_CHECK_PIXEL(px, 40, 1, 65, 0, 0, 0, 0);      /* 34.5: gap */
    EMB_CHECK_PIXEL(px, 40, 1, 40, 0, 0, 0, 0);      /* 59.5: gap 56..66 */
    EMB_CHECK_PIXEL(px, 40, 1, 50, 255, 0, 0, 255);  /* 49.5: dash 44..56 */
    EMB_CHECK_PIXEL(px, 40, 1, 1, 255, 0, 0, 255);   /* 98.5: last dash */
    SDL_free(px);
}

/* Dashed right side: its along coordinate runs down from the top corner. */
EMB_TEST(border_dashed_right_runs_down_from_the_top_corner)
{
    bscene s = {{0, 0, 40, 100}, {0, 0, 0, 0}, {0, 4, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DASHED);
    uint8_t *px = emb_test_render(40, 100, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 38, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 38, 15, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 40, 38, 25, 255, 0, 0, 255);
    SDL_free(px);
}

/* Dotted top side, 4 px wide, 100 px long: n = round(100 / 8) + 1 = 13 dots with centers every
 * 100 / 12 px, the first at x = 0 and the last at x = 100. Each dot has radius 2 and is centered
 * at y = 2. */
EMB_TEST(border_dotted_top_places_round_dots_on_a_fitted_period)
{
    bscene s = {{0, 0, 100, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DOTTED);
    uint8_t *px = emb_test_render(100, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    /* 14 dots (round(100 / 8) + 1 with the half rounded up), so the period is 100 / 13 =
     * 7.69 px: centers at 0, 7.69, 15.38, 23.08, 30.77, 38.46, 46.15, 53.85, ... 100. */
    /* Dot at x = 0: the pixel at (0, 1) is 0.71 px from the center. */
    EMB_CHECK_PIXEL(px, 100, 0, 1, 255, 0, 0, 255);
    /* x = 4 is 3.2 px from the nearest dot at 7.69: outside the radius of 2. */
    EMB_CHECK_PIXEL(px, 100, 4, 1, 0, 0, 0, 0);
    /* Dot at 15.38: x = 16 is 1.2 px from it. */
    EMB_CHECK_PIXEL(px, 100, 16, 1, 255, 0, 0, 255);
    /* x = 20 is 2.6 px from the dot at 23.08. */
    EMB_CHECK_PIXEL(px, 100, 20, 1, 0, 0, 0, 0);
    /* Dot at 38.46: the middle row reaches it, and so does the top row (1.5 px away). */
    EMB_CHECK_PIXEL(px, 100, 38, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 100, 38, 0, 255, 0, 0, 255);
    /* x = 42 is 3.7 px from the nearest dot at 46.15. */
    EMB_CHECK_PIXEL(px, 100, 42, 1, 0, 0, 0, 0);
    /* Dot at 53.85: x = 54 is 0.8 px from it. */
    EMB_CHECK_PIXEL(px, 100, 54, 1, 255, 0, 0, 255);
    /* The last dot is at the far corner. */
    EMB_CHECK_PIXEL(px, 100, 99, 1, 255, 0, 0, 255);
    SDL_free(px);
}

/* Double top side, 6 px: two lines of 2 px at the outer and inner edges, and a 2 px gap. */
EMB_TEST(border_double_top_is_two_lines_with_a_gap)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {6, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DOUBLE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 2, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 40, 20, 3, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 40, 20, 4, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 5, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 6, 0, 0, 0, 0);
    SDL_free(px);
}

/* Double below 3 px is solid. */
EMB_TEST(border_double_under_three_px_is_solid)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {2, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_DOUBLE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 2, 0, 0, 0, 0);
    SDL_free(px);
}

/* Groove top side, 4 px: the outer half (rows 0 and 1) is dark, the inner half (rows 2 and 3)
 * is the color. */
EMB_TEST(border_groove_top_is_dark_outside_and_light_inside)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 171, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 1, 171, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 3, 255, 0, 0, 255);
    SDL_free(px);
}

/* Ridge is the reverse of groove on the top side. */
EMB_TEST(border_ridge_top_is_light_outside_and_dark_inside)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_RIDGE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 2, 171, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 3, 171, 0, 0, 255);
    SDL_free(px);
}

/* A 3 px groove has its half boundary on row 1's center: the two shades mix equally there. */
EMB_TEST(border_groove_half_boundary_is_anti_aliased)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {3, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 171, 0, 0, 255);
    /* The boundary is at y = 1.5: half of each shade, (171 + 255) / 2 = 213. */
    CHECK_PX_NEAR(px, 40, 20, 1, 213, 0, 0, 255, 2);
    EMB_CHECK_PIXEL(px, 40, 20, 2, 255, 0, 0, 255);
    SDL_free(px);
}

/* Inset top side: the whole side is dark. Outset top side: the whole side is the color. */
EMB_TEST(border_inset_top_is_dark_and_outset_top_is_the_color)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, BLUE);
    all4(s.styles, EMBI_BSTYLE_INSET);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 0, 0, 171, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 3, 0, 0, 171, 255);
    SDL_free(px);

    bscene o = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(o.colors, RED);
    all4(o.styles, EMBI_BSTYLE_OUTSET);
    px = emb_test_render(40, 40, scene_border, &o);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 3, 255, 0, 0, 255);
    SDL_free(px);
}

/* Bottom side is the other half of the 3D rule: inset makes it the color, outset dark. */
EMB_TEST(border_inset_and_outset_bottom_are_mirrored)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {0, 0, 4, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_INSET);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 38, 255, 0, 0, 255);
    SDL_free(px);

    bscene o = {{0, 0, 40, 40}, {0, 0, 0, 0}, {0, 0, 4, 0}, {0}, {0}};
    all4(o.colors, RED);
    all4(o.styles, EMBI_BSTYLE_OUTSET);
    px = emb_test_render(40, 40, scene_border, &o);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 38, 171, 0, 0, 255);
    SDL_free(px);
}

/* Groove on the bottom side: the outer half is the color (near the bottom edge, rows 38 and 39)
 * and the inner half dark (rows 36 and 37). */
EMB_TEST(border_groove_bottom_is_light_outside_and_dark_inside)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {0, 0, 4, 0}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 39, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 38, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 37, 171, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 40, 20, 36, 171, 0, 0, 255);
    SDL_free(px);
}

/* Groove on all four sides with one color. Top and left (lit side) are dark outside, the
 * right and bottom sides are dark inside. Mid-side samples, one px from the edge (outer) and
 * 2.5 px from it (inner). */
EMB_TEST(border_groove_all_sides_split_by_the_lit_rule)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 4, 4, 4}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 0, 171, 0, 0, 255);   /* top, outer */
    EMB_CHECK_PIXEL(px, 40, 20, 2, 255, 0, 0, 255);   /* top, inner */
    EMB_CHECK_PIXEL(px, 40, 0, 20, 171, 0, 0, 255);   /* left, outer */
    EMB_CHECK_PIXEL(px, 40, 2, 20, 255, 0, 0, 255);   /* left, inner */
    EMB_CHECK_PIXEL(px, 40, 39, 20, 255, 0, 0, 255);  /* right, outer */
    EMB_CHECK_PIXEL(px, 40, 37, 20, 171, 0, 0, 255);  /* right, inner */
    EMB_CHECK_PIXEL(px, 40, 20, 39, 255, 0, 0, 255);  /* bottom, outer */
    EMB_CHECK_PIXEL(px, 40, 20, 37, 171, 0, 0, 255);  /* bottom, inner */
    SDL_free(px);
}

/* A zero-width or none side paints nothing. */
EMB_TEST(border_none_style_paints_nothing)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 4, 4, 4}, {0}, {0}};
    all4(s.colors, RED);
    all4(s.styles, EMBI_BSTYLE_NONE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_PIXEL(px, 40, 20, 1, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 40, 1, 20, 0, 0, 0, 0);
    SDL_free(px);
}

/* The dark shade of blue is (0, 0, 171): the shade scales each channel by 1 - 0.33 / max. */
EMB_TEST(border_dark_shade_scales_the_largest_channel)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, BLUE);
    all4(s.styles, EMBI_BSTYLE_INSET);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, 40, 20, 1, 0, 0, 171, 255, 1);
    SDL_free(px);
}

/* groove-default: a black groove is visible. The dark half stays black (it is nearer to black than #202020 is) and the
 * light half is Chromium's lightened black, #545454; a mid gray gets a lighter light side. */
EMB_TEST(border_groove_of_black_has_a_lightened_half)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RGBA(0, 0, 0, 255));
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, 40, 20, 0, 0, 0, 0, 255, 1);
    CHECK_PX_NEAR(px, 40, 20, 3, 84, 84, 84, 255, 1);
    SDL_free(px);
}

/* Chromium darkens a color only when its distance from black is more than that of #202020: a #1c1c1c groove keeps its
 * dark half unchanged (it would become black if the test compared against the distance to #202020 itself). */
EMB_TEST(border_dark_shade_leaves_a_near_black_color_unchanged)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RGBA(28, 28, 28, 255));
    all4(s.styles, EMBI_BSTYLE_GROOVE);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, 40, 20, 0, 28, 28, 28, 255, 1);
    SDL_free(px);
}

/* A mid gray (128) outset border: the light side is scaled by (0.502 + 0.33) / 0.502, up to 212. */
EMB_TEST(border_outset_of_gray_lightens_the_light_side)
{
    bscene s = {{0, 0, 40, 40}, {0, 0, 0, 0}, {4, 0, 0, 0}, {0}, {0}};
    all4(s.colors, RGBA(128, 128, 128, 255));
    all4(s.styles, EMBI_BSTYLE_OUTSET);
    uint8_t *px = emb_test_render(40, 40, scene_border, &s);
    EMB_RENDER_OR_SKIP(px);
    CHECK_PX_NEAR(px, 40, 20, 1, 212, 212, 212, 255, 1);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
