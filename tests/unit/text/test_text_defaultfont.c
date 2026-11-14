#include "text_fixture.h"

/* The bundled default face (assets/fonts/ember-default.ttf). This test runs in both builds: with
 * the default font (the normal build) and with EMBER_NO_DEFAULT_FONT=ON, where the registry starts
 * empty and text falls back to the .notdef path (no face, half an em per code point). */

EMB_TEST(default_face_follows_the_build_option)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    if (embi_text_has_default_font()) {
        EMB_REQUIRE(embi_fontdb_default_face(db) != nullptr);
        EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), 1);
        EMB_CHECK(embi_fontdb_default_face(db) == db->faces.data[0]);
    } else {
        EMB_CHECK(embi_fontdb_default_face(db) == nullptr);
        EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), 0);
    }
    embi_text_destroy(t);
}

EMB_TEST(text_without_any_face_still_lays_out)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_str fam = tx_s("No Such Family");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "Hi", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 2);
    EMB_REQUIRE(lay.runs.len >= 1);
    if (embi_text_has_default_font()) {
        /* The default face takes the text: real glyphs from its own cmap. */
        EMB_CHECK(lay.runs.data[0].face == embi_fontdb_default_face(embi_text_fonts(t)));
        EMB_CHECK(lay.glyphs.data[0].gid != 0);
    } else {
        /* Nothing covers the text: empty glyphs, half an em each, one per code point. */
        EMB_CHECK(lay.runs.data[0].face == nullptr);
        EMB_CHECK(lay.glyphs.data[0].gid == 0);
        EMB_CHECK(lay.glyphs.data[0].advance == 10.0f);
        EMB_CHECK(lay.glyphs.data[1].advance == 10.0f);
    }
    /* Either way the line API works on it. */
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 2, &vr));
    EMB_CHECK(vr.width == lay.advance);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
