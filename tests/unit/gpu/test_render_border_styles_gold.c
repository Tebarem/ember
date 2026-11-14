#include "emb_render_test.h"

#include "render_check.h"

/* Golden image of every border style, in a row: eight rounded 36 px boxes with 4 px borders,
 * one per style (solid, dotted, dashed, double, groove, ridge, inset, outset). The analytic
 * checks for each style are in test_render_border_styles.c; this one catches anything they do not
 * look at, such as corner joins between styles and the rounded outer edge. */

#define GOLD_W 320
#define GOLD_H 40

static void scene_styles(embi_frame *f, void *user)
{
    (void)user;
    const uint32_t color = RGBA(200, 60, 40, 255);
    for (uint32_t i = 0; i < 8; i++) {
        const float rect[4] = {(float)(i * 40u + 2u), 2.0f, 36.0f, 36.0f};
        const float radii[4] = {6.0f, 6.0f, 6.0f, 6.0f};
        const float widths[4] = {4.0f, 4.0f, 4.0f, 4.0f};
        const uint32_t colors[4] = {color, color, color, color};
        const uint32_t styles[4] = {i, i, i, i};
        embi_frame_border_styled(f, rect, radii, widths, colors, styles, 0, 0);
    }
}

EMB_TEST(border_styles_golden_row)
{
    uint8_t *px = emb_test_render(GOLD_W, GOLD_H, scene_styles, nullptr);
    EMB_RENDER_OR_SKIP(px);
    EMB_CHECK(emb_gold_check("render_border_styles", px, GOLD_W, GOLD_H));
    SDL_free(px);
}

EMB_RENDER_TEST_MAIN()
