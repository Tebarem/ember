/*
 * EMBER_DEFAULT_FONT=OFF (SZ3b): built only in such a build, and without the test font hook. A runtime with
 * no system fonts and no registered font lays a page out without a crash and logs one EMB_LOG_WARN; a runtime whose embedder registers a font
 * (emb_runtime_add_font) before loading never warns and resolves generic and unknown families to it.
 */
#include "emb_test.h"

#include "embed/embed_int.h"
#include "gpu/gpu.h"
#include "text/text.h"

#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

static int g_warns;

static void log_cb(void *user, emb_log_level level, const char *message)
{
    (void)user;
    if (level == EMB_LOG_WARN && strstr(message, "no font can draw") != nullptr) {
        g_warns++;
    }
}

EMB_TEST(no_font_warns_once)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        emb_test_cur_skip = "no video subsystem";
        return;
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.flags = EMB_RUNTIME_NO_SYSTEM_FONTS;
    rd.log = log_cb;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 200;
    vd.height = 100;
    vd.scale = 1.0f;
    vd.device = embi_gpu_device(g);
    emb_view *v = emb_view_create(rt, &vd);
    EMB_REQUIRE(v != nullptr);

    static const char html[] = "<!doctype html><p style=\"font: 16px sans-serif\">Hello world</p>";
    g_warns = 0;
    EMB_CHECK(emb_view_load_html(v, html, sizeof html - 1, "http://t.test/a.html"));
    embi_view_layout(v);
    EMB_CHECK_EQ_INT(g_warns, 1);
    EMB_CHECK(emb_view_load_html(v, html, sizeof html - 1, "http://t.test/b.html"));
    embi_view_layout(v);
    EMB_CHECK_EQ_INT(g_warns, 1); /* once per runtime */

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

/* The public path: a runtime whose embedder registers a font before loading anything never warns, and both a generic family and
 * an unknown family resolve to that face (the first registered font is the default). */
EMB_TEST(a_font_registered_before_loading_is_used_without_a_warning)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        emb_test_cur_skip = "no video subsystem";
        return;
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.flags = EMB_RUNTIME_NO_SYSTEM_FONTS;
    rd.log = log_cb;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    size_t n = 0;
    void *bytes = SDL_LoadFile(EMBER_SOURCE_DIR "/assets/fonts/ember-default.ttf", &n);
    EMB_REQUIRE(bytes != nullptr);
    EMB_CHECK(emb_runtime_add_font(rt, bytes, n));
    SDL_free(bytes);
    EMB_CHECK_EQ_INT(embi_fonts_face_count(rt->fonts), 1);

    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 200;
    vd.height = 100;
    vd.scale = 1.0f;
    vd.device = embi_gpu_device(g);
    emb_view *v = emb_view_create(rt, &vd);
    EMB_REQUIRE(v != nullptr);
    static const char html[] = "<!doctype html><p style=\"font: 16px sans-serif\">Hello</p>"
                               "<p style=\"font: 16px NoSuchFamily\">World</p>";
    g_warns = 0;
    EMB_CHECK(emb_view_load_html(v, html, sizeof html - 1, "http://t.test/a.html"));
    embi_view_layout(v);
    EMB_CHECK_EQ_INT(g_warns, 0);

    const char *names[] = {"sans-serif", "NoSuchFamily"};
    embi_face *first = nullptr;
    for (int i = 0; i < 2; i++) {
        embi_str fam = {names[i], (uint32_t)strlen(names[i])};
        embi_style_match m = {.families = &fam, .family_count = 1, .weight = 400.0f, .style = EMBI_STYLE_NORMAL, .stretch = 100.0f};
        embi_face *cand[4];
        uint32_t nc = embi_fontdb_candidates(rt->fonts, &m, cand, 4);
        EMB_REQUIRE(nc >= 1);
        EMB_CHECK(cand[0] != nullptr);
        EMB_CHECK(cand[0] == embi_fontdb_default_face(rt->fonts));
        if (first == nullptr) {
            first = cand[0];
        }
        EMB_CHECK(cand[0] == first);
    }
    EMB_CHECK(first != nullptr);

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

EMB_TEST_MAIN()
