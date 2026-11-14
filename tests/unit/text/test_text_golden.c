#include "text_fixture.h"

#include "emb_gold.h"
#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>

/* Golden images of drawn text (tests/gold/text_*.bmp), each after its analytic checks, so the
 * golden cannot hide a shaping bug:
 *   text_latin_kern_liga: "AV office" at 48 px, NotoSans-Regular. The pair AV is kerned (advance
 *     below the sum of the two), and "ffi" / "fi" are single glyphs.
 *   text_mixed_bidi:      "abc " + alef beh jeem + " 123" at 40 px, LTR paragraph. Latin left,
 *     digits (level 2) next, the Arabic (level 1) at the right end of the line.
 * Regenerate with EMBER_UPDATE_GOLDENS=1 after a deliberate change, and read the diff first. */

#define GW 300
#define GH 72

typedef struct gold_case {
    embi_text *t;
    const embi_text_layout *lay;
    embi_text_vruns vr;
} gold_case;

static void scene_gold(embi_frame *f, void *user)
{
    gold_case *c = user;
    embi_text_draw(c->t, f, c->lay, &c->vr, 10.0f, 50.0f, RGBA(20, 20, 20, 255), 0, 0);
}

static bool render_and_check(const char *name, gold_case *c)
{
    uint8_t *px = emb_test_render(GW, GH, scene_gold, c);
    if (px == nullptr) {
        return false;
    }
    bool ok = emb_gold_check(name, px, GW, GH);
    /* Some ink must be there (a golden of a blank image would pass a broken draw path). */
    uint32_t ink = 0;
    for (uint32_t i = 0; i < GW * GH; i++) {
        ink += px[i * 4 + 3] > 128 ? 1u : 0u;
    }
    EMB_CHECK(ink > 400);
    SDL_free(px);
    return ok;
}

EMB_TEST(latin_kerning_and_ligatures_golden)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 48.0f);

    /* Analytic: kerning shortens AV, and ligatures join fi and ffi. */
    embi_text_layout kern_off, kern_on, ligs;
    st.kerning = EMBI_KERN_NONE;
    EMB_REQUIRE(tx_shape(t, "AV", &st, &kern_off));
    st.kerning = EMBI_KERN_NORMAL;
    EMB_REQUIRE(tx_shape(t, "AV", &st, &kern_on));
    EMB_CHECK(kern_on.advance < kern_off.advance);
    st.kerning = EMBI_KERN_AUTO;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &ligs));
    EMB_CHECK_EQ_INT(ligs.glyphs.len, 1);
    embi_text_layout_free(&kern_off);
    embi_text_layout_free(&kern_on);
    embi_text_layout_free(&ligs);

    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "AV office", &st, &lay));
    EMB_CHECK(lay.glyphs.len < 9); /* nine code points: the fi ligature joins at least two */
    gold_case c = {0};
    c.t = t;
    c.lay = &lay;
    EMB_REQUIRE(embi_text_line(&lay, 0, lay.text_len, &c.vr));
    EMB_CHECK(render_and_check("text_latin_kern_liga", &c));
    embi_text_vruns_free(&c.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(mixed_bidi_golden)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    d.family = tx_s("Noto Sans Arabic");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSansArabic-Regular.ttf", &d) == 1);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 40.0f);
    st.direction = EMBI_DIR_LTR;
    /* "abc " + alef beh jeem + " 123" */
    const char *text = "abc \xD8\xA7\xD8\xA8\xD8\xAC 123";
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, text, &st, &lay));
    EMB_REQUIRE(lay.bidi != nullptr);
    /* Analytic: three levels in logical order, Latin 0, Arabic 1, digits 2. */
    bool arabic = false, digits = false;
    for (uint32_t i = 0; i < lay.runs.len; i++) {
        if (lay.runs.data[i].text_begin == 4) {
            EMB_CHECK_EQ_INT(lay.runs.data[i].level, 1);
            arabic = true;
        }
        if (lay.runs.data[i].text_begin == 11) {
            EMB_CHECK_EQ_INT(lay.runs.data[i].level, 2);
            digits = true;
        }
    }
    EMB_CHECK(arabic && digits);
    gold_case c = {0};
    c.t = t;
    c.lay = &lay;
    EMB_REQUIRE(embi_text_line(&lay, 0, lay.text_len, &c.vr));
    /* Visual order (UAX #9 L2): Latin at x = 0, the digits after it, the Arabic ends the line. */
    EMB_REQUIRE(c.vr.items.len >= 3);
    EMB_CHECK(c.vr.items.data[0].x == 0.0f);
    EMB_CHECK_EQ_INT(lay.runs.data[c.vr.items.data[0].run].text_begin, 0);
    EMB_CHECK_EQ_INT(lay.runs.data[c.vr.items.data[1].run].text_begin, 11);
    float drift = c.vr.width - lay.advance; /* visual sums add in another order: rounding only */
    EMB_CHECK(drift < 0.01f && drift > -0.01f);
    EMB_CHECK(render_and_check("text_mixed_bidi", &c));
    embi_text_vruns_free(&c.vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
