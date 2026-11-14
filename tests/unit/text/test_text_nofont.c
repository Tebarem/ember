/*
 * EMBER_DEFAULT_FONT=OFF (SZ3b): built only in such a build, and without the test font hook (the registry
 * starts empty). Text that no face can draw is skipped without a crash, the registry warns once, and a registered font
 * (what an embedder provides through emb_runtime_add_font) draws it.
 */
#include "text_fixture.h"
#include "text/text_int.h"

#include <string.h>

#include <SDL3/SDL_iostream.h>

static int g_warns;
static char g_msg[400];

static void warn_cb(void *user, const char *msg)
{
    (void)user;
    g_warns++;
    SDL_snprintf(g_msg, sizeof g_msg, "%s", msg);
}

EMB_TEST(no_font_at_all_does_not_crash_and_warns_once)
{
    g_warns = 0;
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(!embi_text_has_default_font());
    embi_fontdb *db = embi_text_fonts(t);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), 0);
    EMB_CHECK(db->sys == nullptr); /* the system index is started only by a runtime: no machine fonts can be picked up */
    embi_fontdb_set_warn(db, warn_cb, nullptr);
    embi_str fam = tx_s("sans-serif");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    for (int round = 0; round < 3; round++) {
        embi_text_layout lay;
        EMB_REQUIRE(tx_shape(t, "Hello", &st, &lay));
        EMB_CHECK_EQ_INT((int)lay.glyphs.len, 5);
        EMB_CHECK(lay.runs.data[0].face == nullptr);
        EMB_CHECK(lay.glyphs.data[0].advance == 10.0f); /* half an em each */
        embi_text_layout_free(&lay);
    }
    EMB_CHECK_EQ_INT(g_warns, 1);
    EMB_CHECK(strstr(g_msg, "EMBER_DEFAULT_FONT") != nullptr);
    embi_text_destroy(t);
}

EMB_TEST(a_registered_font_draws_the_text)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    g_warns = 0;
    embi_fontdb *db = embi_text_fonts(t);
    embi_fontdb_set_warn(db, warn_cb, nullptr);
    size_t n = 0;
    void *bytes = SDL_LoadFile(EMBER_SOURCE_DIR "/assets/fonts/ember-default.ttf", &n);
    EMB_REQUIRE(bytes != nullptr);
    uint32_t before = embi_fontdb_face_count(db);
    EMB_REQUIRE(embi_fontdb_add(db, bytes, n, true, nullptr) > 0);
    embi_fontdb_default_if_none(db, before); /* what emb_runtime_add_font does */
    EMB_CHECK(embi_fontdb_default_face(db) != nullptr);
    embi_str fam = tx_s("sans-serif");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "Hello", &st, &lay));
    EMB_CHECK_EQ_INT((int)lay.glyphs.len, 5);
    EMB_CHECK(lay.runs.data[0].face != nullptr);
    EMB_CHECK(lay.glyphs.data[0].gid != 0);
    EMB_CHECK_EQ_INT(g_warns, 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
