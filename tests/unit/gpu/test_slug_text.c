#include "emb_render_test.h"

#include "render_check.h"

#include <SDL3/SDL_stdinc.h>

/* The text path of the renderer (W4h): embi_frame_fill_text. A one px stem, wound counter-clockwise
 * and clockwise, at 10.5 .. 11.5: the two columns it touches are half covered. Stem darkening of
 * 0.25 px raises partial coverage by 21% (a boost, not a geometric growth: pixels the outline does
 * not touch stay empty), whichever way the outline is wound. */

#define W 48
#define H 32

static void rect_curves(embi_qcurve out[4], float x0, float y0, float x1, float y1, bool ccw)
{
    const float px[5] = {x0, x1, x1, x0, x0};
    const float py[5] = {y0, y0, y1, y1, y0};
    for (int k = 0; k < 4; k++) {
        const int a = ccw ? k : 4 - k;
        const int c = ccw ? k + 1 : 3 - k;
        out[k] = (embi_qcurve){px[a], py[a], 0.5f * (px[a] + px[c]), 0.5f * (py[a] + py[c]), px[c], py[c]};
    }
}

typedef struct stem_case {
    float darken;
    bool ccw;
    bool lcd_requested;
} stem_case;

static void scene_stem(embi_frame *f, void *user)
{
    const stem_case *c = user;
    embi_qcurve q[4];
    rect_curves(q, 10.5f, 8.0f, 11.5f, 24.0f, c->ccw);
    const float id[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    embi_frame_fill_text(f, c->ccw ? 0x1001u : 0x1002u, q, 4, id, RGBA(0, 0, 0, 255), c->darken,
                         c->lcd_requested, 0, 0);
}

static int alpha_at(const uint8_t *px, int x, int y)
{
    return px[((size_t)y * W + (size_t)x) * 4u + 3u];
}

EMB_TEST(text_stem_darkening_boosts_both_windings)
{
    for (int w = 0; w < 2; w++) {
        stem_case plain = {0.0f, w == 0, false};
        stem_case dark = {0.25f, w == 0, false};
        uint8_t *a = emb_test_render(W, H, scene_stem, &plain);
        EMB_RENDER_OR_SKIP(a);
        uint8_t *b = emb_test_render(W, H, scene_stem, &dark);
        EMB_REQUIRE(b != nullptr);
        const int a10 = alpha_at(a, 10, 16), a11 = alpha_at(a, 11, 16);
        const int b10 = alpha_at(b, 10, 16), b11 = alpha_at(b, 11, 16);
        /* Both columns are half covered, then the contrast curve lifts them a little. */
        EMB_CHECK(a10 > 110 && a10 < 200);
        EMB_CHECK(a11 > 110 && a11 < 200);
        /* Darkening raises the half coverage of each column. */
        EMB_CHECK(b10 > a10 + 15);
        EMB_CHECK(b11 > a11 + 15);
        /* The neighbors outside stay empty. */
        EMB_CHECK_EQ_INT(alpha_at(b, 9, 16), 0);
        EMB_CHECK_EQ_INT(alpha_at(b, 12, 16), 0);
        /* So do the rows beyond the ends of the stem. */
        EMB_CHECK_EQ_INT(alpha_at(b, 10, 7), 0);
        EMB_CHECK_EQ_INT(alpha_at(b, 10, 24), 0);
        SDL_free(a);
        SDL_free(b);
    }
}

/* LCD is honored only where the frame allows it: with no opaque backdrop (the default of a new
 * frame) a request for LCD draws the gray result. */
EMB_TEST(text_lcd_request_without_backdrop_is_gray)
{
    stem_case gray = {0.0f, true, false};
    stem_case lcd = {0.0f, true, true};
    uint8_t *a = emb_test_render(W, H, scene_stem, &gray);
    EMB_RENDER_OR_SKIP(a);
    uint8_t *b = emb_test_render(W, H, scene_stem, &lcd);
    EMB_REQUIRE(b != nullptr);
    for (int x = 8; x < 14; x++) {
        EMB_CHECK_EQ_INT(alpha_at(a, x, 16), alpha_at(b, x, 16));
        EMB_CHECK_EQ_INT(b[((size_t)16 * W + (size_t)x) * 4u], a[((size_t)16 * W + (size_t)x) * 4u]);
    }
    SDL_free(a);
    SDL_free(b);
}

/* embi_frame_text_target: the device matrix, and fit and lcd flags per transform. */
static void scene_target(embi_frame *f, void *user)
{
    int *bad = user;
    embi_text_target t;
    if (!embi_frame_text_target(f, 0, &t) || !t.fit || t.lcd) {
        *bad |= 1; /* device scale: fit, no LCD without a backdrop */
    }
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_LCD_RGB, false);
    embi_frame_set_opaque_backdrop(f, true);
    if (!embi_frame_text_target(f, 0, &t) || !t.fit || !t.lcd) {
        *bad |= 2;
    }
    const float rot[6] = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    if (!embi_frame_text_target(f, embi_frame_xform(f, rot), &t) || t.fit || t.lcd) {
        *bad |= 4; /* a quarter turn is not axis aligned for the fit */
    }
    const float turn[6] = {1.0f, -2.4e-7f, 2.4e-7f, 1.0f, 0.0f, 0.0f};
    if (!embi_frame_text_target(f, embi_frame_xform(f, turn), &t) || !t.fit || !t.lcd) {
        *bad |= 256; /* rotate(360deg): float noise in the matrix is not a rotation */
    }
    const float mirror[6] = {-1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (!embi_frame_text_target(f, embi_frame_xform(f, mirror), &t) || t.fit) {
        *bad |= 8;
    }
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_LCD_RGB, true);
    const float scl[6] = {2.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f};
    const uint32_t xs = embi_frame_xform(f, scl);
    if (!embi_frame_text_target(f, xs, &t) || t.fit || t.lcd) {
        *bad |= 16; /* a transform animation runs */
    }
    if (!embi_frame_text_target(f, 0, &t) || !t.fit || !t.lcd) {
        *bad |= 32; /* the device-scale transform is unaffected */
    }
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_LCD_RGB, false);
    if (!embi_frame_text_target(f, xs, &t) || !t.fit || !t.lcd || t.m[0] != 2.0f || t.m[4] != 3.0f) {
        *bad |= 64;
    }
    embi_frame_set_lcd_allowed(f, false);
    if (!embi_frame_text_target(f, xs, &t) || !t.fit || t.lcd) {
        *bad |= 128;
    }
}

EMB_TEST(text_target_reports_fit_and_lcd)
{
    int bad = 0;
    uint8_t *px = emb_test_render(8, 8, scene_target, &bad);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK_EQ_INT(bad, 0);
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
