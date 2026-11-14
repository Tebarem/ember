#include "text_fixture.h"

/* Font registry: registration of files, WOFF/WOFF2 decoding, face metadata, descriptor
 * overrides and ownership. Expected values come from the raw tables (see tests/fonts/README.md
 * and the fixture dump in the R2c notes):
 *   assets/fonts/ember-default.ttf: name 1 "Noto Sans", OS/2 usWeightClass 400, usWidthClass 5.
 *   NotoSans-Regular.ttf: name 1 "Noto Sans", weight 400; 'A' is gid 36, advance 639 (font units).
 *   NotoSans-VF.ttf: fvar wght 100..400..900, wdth 62.5..100..100 (in percent). */

EMB_TEST(default_font_is_registered)
{
    if (!embi_text_has_default_font()) {
        EMB_SKIP("built with EMBER_NO_DEFAULT_FONT");
    }
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *f = embi_fontdb_default_face(embi_text_fonts(t));
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(embi_text_fonts(t)), 1);
    EMB_CHECK(embi_str_eq(embi_face_family(f), tx_s("Noto Sans")));
    EMB_CHECK_EQ_INT(f->weight_min, 400);
    EMB_CHECK_EQ_INT(f->weight_max, 400);
    EMB_CHECK_EQ_INT(f->style, EMBI_STYLE_NORMAL);
    EMB_CHECK(f->stretch_min == 100.0f);
    EMB_CHECK_EQ_INT(f->font.axis_count, 0);
    embi_text_destroy(t);
}

EMB_TEST(static_file_registers_one_face)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_CHECK_EQ_INT(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", nullptr), 1);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(embi_text_fonts(t)), before + 1);
    embi_face *f = embi_fontdb_default_face(embi_text_fonts(t));
    (void)f;
    /* 'A' is glyph 36 with advance 639 in this font (test_font_metrics, same fixture). */
    embi_face *nf = embi_text_fonts(t)->faces.data[before];
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&nf->font, 'A'), 36);
    float adv = 0.0f;
    EMB_REQUIRE(embi_font_glyph_advance(&nf->font, 36, nullptr, &adv));
    EMB_CHECK(adv == 639.0f);
    EMB_CHECK_EQ_INT(nf->weight_min, 400);
    EMB_CHECK(embi_str_eq(embi_face_family(nf), tx_s("Noto Sans")));
    embi_text_destroy(t);
}

/* A WOFF or WOFF2 file of the same font decodes to the same glyphs and advances. */
static void check_compressed_twin(const char *rel)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, rel, nullptr) == 1);
    embi_face *nf = embi_text_fonts(t)->faces.data[before];
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&nf->font, 'A'), 36);
    float adv = 0.0f;
    EMB_REQUIRE(embi_font_glyph_advance(&nf->font, 36, nullptr, &adv));
    EMB_CHECK(adv == 639.0f);
    embi_text_destroy(t);
}

EMB_TEST(woff_round_trip_registers)
{
    check_compressed_twin("tests/fonts/NotoSans-Regular.woff");
}

EMB_TEST(woff2_round_trip_registers)
{
    check_compressed_twin("tests/fonts/NotoSans-Regular.woff2");
}

EMB_TEST(variable_face_axes_and_ranges)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", nullptr) == 1);
    embi_face *f = embi_text_fonts(t)->faces.data[before];
    EMB_CHECK_EQ_INT(f->font.axis_count, 2);
    /* fvar wght 100..900, wdth 62.5..100 (percent). */
    EMB_CHECK_EQ_INT(f->weight_min, 100);
    EMB_CHECK_EQ_INT(f->weight_max, 900);
    EMB_CHECK(f->stretch_min == 62.5f);
    EMB_CHECK(f->stretch_max == 100.0f);
    EMB_CHECK(f->ax_wght >= 0 && f->ax_wdth >= 0);
    EMB_CHECK_EQ_INT(f->ax_slnt, -1);
    embi_text_destroy(t);
}

EMB_TEST(descriptor_overrides_apply)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Test Family");
    d.weight_min = 700;
    d.weight_max = 700;
    d.has_style = true;
    d.style = EMBI_STYLE_ITALIC;
    embi_unicode_range r[1] = {{0x41, 0x5A}};
    d.ranges = r;
    d.range_count = 1;
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    embi_face *f = embi_text_fonts(t)->faces.data[before];
    EMB_CHECK(embi_str_eq(embi_face_family(f), tx_s("Test Family")));
    EMB_CHECK_EQ_INT(f->weight_min, 700);
    EMB_CHECK_EQ_INT(f->weight_max, 700);
    EMB_CHECK_EQ_INT(f->style, EMBI_STYLE_ITALIC);
    /* unicode-range limits coverage: 'A' is covered, 'a' (0x61) is not. */
    EMB_CHECK(embi_face_covers(f, 'A'));
    EMB_CHECK(!embi_face_covers(f, 'a'));
    embi_text_destroy(t);
}

EMB_TEST(garbage_is_rejected_without_faces)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    uint8_t *junk = SDL_malloc(64);
    SDL_memset(junk, 0x5A, 64);
    EMB_CHECK_EQ_INT(embi_fontdb_add(embi_text_fonts(t), junk, 64, true, nullptr), 0);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(embi_text_fonts(t)), before);
    embi_text_destroy(t);
}

EMB_TEST(create_destroy_returns_all_memory)
{
    size_t base = embi_mem_live();
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_text_destroy(t);
    EMB_CHECK(embi_mem_live() == base);
}
EMB_TEST_MAIN()
