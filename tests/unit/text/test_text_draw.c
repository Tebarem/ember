#include "text_fixture.h"

#include "../gpu/render_check.h"
#include "emb_render_test.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

/* Drawing through the renderer (gpu/render.h) with the Slug path. Two checks:
 *   - an outline glyph ('I' from NotoSans-Regular, upem 1000) covers its stem at the centre
 *     pixel and leaves the corner alone;
 *   - a COLRv1 emoji (Noto-COLRv1.ttf) draws its own colours, not the text colour.
 * Without a GPU device the tests are skipped (exit code 77). */

/* The tests draw to an offscreen target, so they do not need a display: unless the environment names a video driver
 * (SDL_VIDEO_DRIVER already set stays), the offscreen one is used and the tests run on a headless
 * machine instead of being skipped. */
static bool init_video(void)
{
    (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_VIDEO_DRIVER", "offscreen", false); /* an existing choice stays */
    return SDL_Init(SDL_INIT_VIDEO);
}

typedef struct draw_case {
    embi_text *t;
    const embi_text_layout *lay;
    embi_text_vruns vr;
    float x, y;
    uint32_t color;
} draw_case;

static void scene_text(embi_frame *f, void *user)
{
    draw_case *c = user;
    embi_text_draw(c->t, f, c->lay, &c->vr, c->x, c->y, c->color, 0, 0);
}

static uint32_t px_u32(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y)
{
    const uint8_t *p = px + ((size_t)y * w + x) * 4u;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

EMB_TEST(outline_glyph_fills_its_stem)
{
    /* The GPU device needs the video subsystem; without it the test is skipped. */
    if (!init_video()) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    embi_face *face = embi_text_fonts(t)->faces.data[before];

    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 100.0f); /* 0.1 px per font unit */
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "I", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 1);

    /* Outline box of 'I' in font units, for the centre of the stem. */
    embi_outline ol;
    SDL_memset(&ol, 0, sizeof ol);
    EMB_REQUIRE(embi_font_glyph_outline(&face->font, lay.glyphs.data[0].gid, nullptr, &ol));
    float cx_units = 0.5f * (ol.xmin + ol.xmax);
    float ymax = ol.ymax;
    embi_outline_free(&ol);

    draw_case c = {0};
    c.t = t;
    c.lay = &lay;
    c.x = 20.0f;
    c.y = 120.0f; /* baseline */
    c.color = RGBA(255, 0, 0, 255);
    EMB_REQUIRE(embi_text_line(&lay, 0, 1, &c.vr));

    uint8_t *px = emb_test_render(200, 160, scene_text, &c);
    EMB_RENDER_OR_SKIP(px);

    /* Stem centre in device px: x = 20 + 0.1 * cx, y = 120 - 0.1 * (ymax / 2). */
    uint32_t sx = (uint32_t)(20.0f + 0.1f * cx_units);
    uint32_t sy = (uint32_t)(120.0f - 0.1f * (0.5f * ymax));
    CHECK_PX_NEAR(px, 200, sx, sy, 255, 0, 0, 255, 2);
    /* Far from the glyph: transparent. */
    CHECK_PX_NEAR(px, 200, 5, 5, 0, 0, 0, 0, 0);
    /* Right of the glyph's advance: transparent too. */
    CHECK_PX_NEAR(px, 200, 190, 100, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_text_vruns_free(&c.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(control_character_draws_a_hollow_box)
{
    if (!init_video()) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 100.0f); /* 0.1 px per font unit */
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "\x01", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 1);
    EMB_CHECK(lay.glyphs.data[0].gid == 0xFFFFu);

    draw_case c = {0};
    c.t = t;
    c.lay = &lay;
    c.x = 20.0f;
    c.y = 120.0f;
    c.color = RGBA(255, 0, 0, 255);
    EMB_REQUIRE(embi_text_line(&lay, 0, 1, &c.vr));
    uint8_t *px = emb_test_render(200, 160, scene_text, &c);
    EMB_RENDER_OR_SKIP(px);
    /* The box spans x 30..70 and y 50..120 with a 5 px frame: the left and top bars are filled, the centre is empty. */
    CHECK_PX_NEAR(px, 200, 32, 85, 255, 0, 0, 255, 2);
    CHECK_PX_NEAR(px, 200, 68, 85, 255, 0, 0, 255, 2);
    CHECK_PX_NEAR(px, 200, 50, 52, 255, 0, 0, 255, 2);
    CHECK_PX_NEAR(px, 200, 50, 85, 0, 0, 0, 0, 0);
    CHECK_PX_NEAR(px, 200, 5, 5, 0, 0, 0, 0, 0);
    SDL_free(px);
    embi_text_vruns_free(&c.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(colr_emoji_uses_its_own_layers)
{
    if (!init_video()) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/Noto-COLRv1.ttf", nullptr) == 1);
    embi_face *emoji = embi_text_fonts(t)->faces.data[before];
    EMB_CHECK(emoji->font.metrics.has_colr);

    embi_str fam = tx_s("Noto Color Emoji");
    embi_text_style st;
    tx_style(&st, &fam, 1, 64.0f);
    embi_text_layout lay;
    /* U+1F600 GRINNING FACE */
    EMB_REQUIRE(tx_shape(t, "\xF0\x9F\x98\x80", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 1);
    EMB_REQUIRE(lay.glyphs.data[0].gid != 0);

    draw_case c = {0};
    c.t = t;
    c.lay = &lay;
    c.x = 10.0f;
    c.y = 70.0f;
    c.color = RGBA(0, 0, 255, 255);
    EMB_REQUIRE(embi_text_line(&lay, 0, 4, &c.vr));

    uint8_t *px = emb_test_render(100, 100, scene_text, &c);
    EMB_RENDER_OR_SKIP(px);

    uint32_t covered = 0, layered = 0;
    for (uint32_t y = 0; y < 100; y++) {
        for (uint32_t x = 0; x < 100; x++) {
            uint32_t v = px_u32(px, 100, x, y);
            uint32_t a = v >> 24;
            if (a > 128) {
                covered++;
                uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF;
                /* The face is yellow: red and green dominate blue. */
                if (g > 120 && r > 120) {
                    layered++;
                }
            }
        }
    }
    EMB_CHECK(covered > 200);
    EMB_CHECK(layered > 50);
    SDL_free(px);
    embi_text_vruns_free(&c.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
