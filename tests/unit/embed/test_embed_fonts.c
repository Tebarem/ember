/*
 * System fonts through the embedding layer (W4i): the runtime flag and the environment opt-out, the stats, the
 * generic family override, trim, local() in @font-face (resolved by full or PostScript name, a miss falls through to
 * the next source), and the restyle of a document whose layout used a fallback when the system font index arrives.
 *
 * The index scans tests/fonts/sysfonts (embi_fonts_test_dirs), see tests/unit/text/sysfont_fixture.h.
 */
#include "emb_test.h"

#include "embed/embed_int.h"
#include "embed/netload.h"
#include "embed/script_int.h"
#include "gpu/gpu.h"

#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define FIXTURE_DIR EMBER_SOURCE_DIR "/tests/fonts/sysfonts"

typedef struct env {
    embi_gpu *g;
    emb_runtime *rt;
    emb_view *v;
} env;

/* A runtime that indexes the fixture directory (delay_ms: how long the index job waits before it starts). */
static emb_runtime *make_rt(uint32_t flags, uint32_t delay_ms)
{
    static const char *const dirs[] = {FIXTURE_DIR};
    SDL_unsetenv_unsafe("EMBER_NO_SYSTEM_FONTS"); /* the harness sets it; these tests want system fonts */
    embi_fonts_test_dirs(dirs, 1, delay_ms);
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.flags = flags;
    emb_runtime *rt = emb_runtime_create(&rd);
    embi_fonts_test_dirs(nullptr, 0, 0);
    return rt;
}

static emb_runtime_stats stats_of(emb_runtime *rt)
{
    emb_runtime_stats s;
    memset(&s, 0, sizeof s);
    s.size = sizeof s;
    emb_runtime_stats_get(rt, &s);
    return s;
}

static bool wait_ready(emb_runtime *rt)
{
    for (int i = 0; i < 1000; i++) {
        if (stats_of(rt).system_index_ready != 0) {
            return true;
        }
        SDL_Delay(10);
    }
    return false;
}

/* A runtime and an offscreen view on the machine's GPU. False (the test is skipped) without a device. */
static bool setup(env *e, uint32_t flags, uint32_t delay_ms)
{
    memset(e, 0, sizeof *e);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        emb_test_cur_skip = "no video subsystem";
        return false;
    }
    e->g = embi_gpu_create(nullptr, false);
    if (e->g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return false;
    }
    e->rt = make_rt(flags, delay_ms);
    if (e->rt == nullptr) {
        return false;
    }
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 200;
    vd.height = 100;
    vd.scale = 1.0f;
    vd.device = embi_gpu_device(e->g);
    e->v = emb_view_create(e->rt, &vd);
    return e->v != nullptr;
}

static void teardown(env *e)
{
    emb_view_destroy(e->v);
    emb_runtime_destroy(e->rt);
    embi_gpu_destroy(e->g);
}

static void load(env *e, const char *html)
{
    (void)emb_view_load_html(e->v, html, strlen(html), "http://t.test/page.html");
}

/* True when the view laid its document out again (the style, layout and paint changed). */
static bool relayout(env *e)
{
    e->v->force_full = false;
    embi_view_layout(e->v);
    return e->v->force_full;
}

EMB_TEST(index_is_built_and_reported_in_the_stats)
{
    emb_runtime *rt = make_rt(0, 0);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(wait_ready(rt));
    emb_runtime_stats s = stats_of(rt);
    EMB_CHECK_EQ_INT(s.system_fonts, 1);
    EMB_CHECK_EQ_INT(s.system_files, 11);
    EMB_CHECK_EQ_INT(s.system_index_entries, 12);
    EMB_CHECK_EQ_INT(s.system_faces_loaded, 0);
    EMB_CHECK_EQ_INT(s.system_font_bytes, 0);
    EMB_CHECK(s.system_index_bytes > 0);
    /* A struct that is too small, or no struct, is left alone. */
    emb_runtime_stats small;
    memset(&small, 0, sizeof small);
    small.size = 8;
    emb_runtime_stats_get(rt, &small);
    EMB_CHECK_EQ_INT(small.system_files, 0);
    emb_runtime_stats_get(rt, nullptr);
    emb_runtime_destroy(rt);
}

EMB_TEST(no_system_fonts_flag_and_environment)
{
    emb_runtime *rt = make_rt(EMB_RUNTIME_NO_SYSTEM_FONTS, 0);
    EMB_REQUIRE(rt != nullptr);
    SDL_Delay(30);
    emb_runtime_stats s = stats_of(rt);
    EMB_CHECK_EQ_INT(s.system_fonts, 0);
    EMB_CHECK_EQ_INT(s.system_index_ready, 0);
    EMB_CHECK_EQ_INT(s.system_index_entries, 0);
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_CHECK(!embi_rt_fetch_font(rt, EMBI_STR("local:Ember Sans Bold"), &data, &len)); /* no index: local() never matches */
    emb_runtime_destroy(rt);

    /* The environment variable has the same effect, and "0" or empty does not. */
    static const char *const dirs[] = {FIXTURE_DIR};
    embi_fonts_test_dirs(dirs, 1, 0);
    SDL_setenv_unsafe("EMBER_NO_SYSTEM_FONTS", "1", 1);
    rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK_EQ_INT(stats_of(rt).system_fonts, 0);
    emb_runtime_destroy(rt);
    SDL_setenv_unsafe("EMBER_NO_SYSTEM_FONTS", "0", 1);
    rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK_EQ_INT(stats_of(rt).system_fonts, 1);
    EMB_CHECK(wait_ready(rt));
    emb_runtime_destroy(rt);
    embi_fonts_test_dirs(nullptr, 0, 0);
    SDL_setenv_unsafe("EMBER_NO_SYSTEM_FONTS", "1", 1); /* back to the harness's value */
}

EMB_TEST(local_sources_resolve_by_full_and_postscript_name)
{
    emb_runtime *rt = make_rt(0, 0);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(wait_ready(rt));
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_CHECK(embi_rt_fetch_font(rt, EMBI_STR("local:Ember Sans Bold"), &data, &len));
    EMB_CHECK(data != nullptr && len > 100);
    SDL_free(data);
    data = nullptr;
    EMB_CHECK(embi_rt_fetch_font(rt, EMBI_STR("local:embersans-bold"), &data, &len));
    SDL_free(data);
    data = nullptr;
    EMB_CHECK(!embi_rt_fetch_font(rt, EMBI_STR("local:Ember Sans"), &data, &len)); /* a family name is not a face name */
    EMB_CHECK(!embi_rt_fetch_font(rt, EMBI_STR("local:"), &data, &len));
    EMB_CHECK(!embi_rt_fetch_font(rt, EMBI_STR("local:No Such Font"), &data, &len));
    emb_runtime_destroy(rt);
}

EMB_TEST(font_face_local_registers_the_system_face)
{
    env e;
    if (!setup(&e, 0, 0)) {
        return;
    }
    EMB_REQUIRE(wait_ready(e.rt));
    uint32_t before = embi_fonts_face_count(e.rt->fonts);
    /* A hit registers a face under the @font-face family. */
    load(&e, "<style>@font-face{font-family:LocalOne;src:local(\"Ember Sans Bold\")}</style><p style=\"font-family:LocalOne\">AAA</p>");
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before + 1);
    /* The PostScript name, with a miss before it: the next source is tried. */
    load(&e, "<style>@font-face{font-family:LocalTwo;src:local(\"No Such Font\"),local(EmberWoff-Regular)}</style>"
             "<p style=\"font-family:LocalTwo\">AAA</p>");
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before + 2);
    /* Only misses (and a url() that does not exist): no face is added. */
    load(&e, "<style>@font-face{font-family:LocalThree;src:local(\"Missing One\"),local(\"Missing Two\"),url(nothere.ttf)}"
             "</style><p style=\"font-family:LocalThree\">AAA</p>");
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before + 2);
    teardown(&e);
}

/* fonts.c test hook: the weight of the face a family resolves to (0: none). */
uint32_t embi_fonts_family_weight(embi_fontdb *db, const char *family);

EMB_TEST(font_face_local_before_the_index_picks_the_system_face_when_it_arrives)
{
    env e;
    if (!setup(&e, 0, 800)) { /* the index job waits 800 ms: the page loads and paints first (the normal cold start) */
        return;
    }
    if (stats_of(e.rt).system_index_ready != 0) {
        teardown(&e);
        EMB_SKIP("machine too slow for the timing");
    }
    uint32_t before = embi_fonts_face_count(e.rt->fonts);
    uint64_t t0 = SDL_GetTicks();
    load(&e, "<style>@font-face{font-family:LocalCold;src:local(\"Ember Sans Bold\")}"
             "@font-face{font-family:LocalCold2;src:local(\"No Such Font\"),local(EmberSans-Bold),url(nothere.ttf)}</style>"
             "<p style=\"font-family:LocalCold\">AAA</p><p style=\"font-family:LocalCold2\">BBB</p>");
    EMB_CHECK(SDL_GetTicks() - t0 < (EMB_TEST_ASAN ? 700 : 500)); /* first paint did not wait for the index */
    if (stats_of(e.rt).system_index_ready != 0) {
        teardown(&e);
        EMB_SKIP("machine too slow for the timing");
    }
    /* Pending: no face yet, the text used the fallback. */
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before);
    EMB_CHECK(!relayout(&e));
    EMB_REQUIRE(wait_ready(e.rt));
    /* The index arrived: the document lays out again, and the local() sources resolve to the installed face. */
    EMB_CHECK(relayout(&e));
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before + 2);
    EMB_CHECK_EQ_INT(embi_fonts_family_weight(e.rt->fonts, "LocalCold"), 700); /* Ember Sans Bold */
    EMB_CHECK_EQ_INT(embi_fonts_family_weight(e.rt->fonts, "localcold2"), 700);
    EMB_CHECK(!relayout(&e)); /* and only once */
    teardown(&e);
}

EMB_TEST(layout_that_used_a_fallback_restyles_when_the_index_arrives)
{
    env e;
    if (!setup(&e, 0, 800)) { /* the index job waits 800 ms: the first layout runs without it */
        return;
    }
    uint64_t t0 = SDL_GetTicks();
    load(&e, "<p style=\"font-family:'Ember Sans'\">AAAA</p>");
    EMB_CHECK(SDL_GetTicks() - t0 < (EMB_TEST_ASAN ? 700 : 500)); /* the load did not wait for the index */
    emb_runtime_stats s = stats_of(e.rt);
    if (s.system_index_ready != 0) { /* a loaded sanitizer box can take longer than the job's delay */
        teardown(&e);
        EMB_SKIP("machine too slow for the timing");
    }
    EMB_CHECK_EQ_INT(s.system_faces_loaded, 0); /* first paint: the bundled face only */
    EMB_CHECK(!relayout(&e)); /* nothing changed yet: no work */
    EMB_REQUIRE(wait_ready(e.rt));
    /* The index arrived: the document is laid out again, and now with the system face. */
    EMB_CHECK(relayout(&e));
    s = stats_of(e.rt);
    EMB_CHECK_EQ_INT(s.system_faces_loaded, 1);
    EMB_CHECK(s.system_font_bytes > 1000);
    EMB_CHECK(!relayout(&e)); /* and only once */
    teardown(&e);
}

EMB_TEST(document_loaded_after_the_index_is_ready_lays_out_once)
{
    env e;
    if (!setup(&e, 0, 0)) {
        return;
    }
    EMB_REQUIRE(wait_ready(e.rt));
    /* U+E000 is covered by no face: the load's layout uses the fallback path, and the index is already there. */
    load(&e, "<p style=\"font-family:'Ember Missing'\">AA" "\xEE\x80\x80" "AA</p>");
    EMB_CHECK(!relayout(&e)); /* no second full style and layout pass after the first paint */
    EMB_CHECK(!relayout(&e));
    teardown(&e);
}

EMB_TEST(local_is_refused_outside_font_sources)
{
    emb_runtime *rt = make_rt(0, 0);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(wait_ready(rt));
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_CHECK(!embi_rt_fetch(rt, EMBI_STR("local:Ember Sans Bold"), &data, &len)); /* stylesheets and images */
    EMB_CHECK(embi_rt_fetch_font(rt, EMBI_STR("local:Ember Sans Bold"), &data, &len));
    SDL_free(data);
    emb_runtime_destroy(rt);
}

EMB_TEST(script_built_text_that_used_a_fallback_restyles_when_the_index_arrives)
{
    env e;
    if (!setup(&e, 0, 2000)) { /* the index job waits 2 s, so a loaded machine still runs the timer first */
        return;
    }
    /* The initial HTML has no text: the DOM is built by a timer after the load, while the index is still pending. */
    load(&e, "<body><script>setTimeout(function(){var p=document.createElement('p');"
             "p.style.fontFamily=\"'Ember Sans'\";p.textContent='AAAA';document.body.appendChild(p);},20);</script></body>");
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_index_ready, 0);
    uint64_t t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < 600) { /* the timer fires and the update lays the new text out with a fallback */
        (void)relayout(&e);
        SDL_Delay(5);
    }
    if (stats_of(e.rt).system_index_ready != 0) {
        teardown(&e);
        EMB_SKIP("machine too slow for the timing");
    }
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_faces_loaded, 0);
    EMB_REQUIRE(wait_ready(e.rt));
    EMB_CHECK(relayout(&e)); /* the index arrived: the script-built text is laid out again with the system face */
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_faces_loaded, 1);
    teardown(&e);
}

EMB_TEST(layout_without_fallback_does_not_restyle_for_the_index)
{
    env e;
    if (!setup(&e, 0, 800)) {
        return;
    }
    load(&e, "<style>html{font-family:'Noto Sans'}</style><p>AAAA</p>"); /* the bundled face's own family: no fallback */
    if (stats_of(e.rt).system_index_ready != 0) { /* a loaded sanitizer box can take longer than the job's delay */
        teardown(&e);
        EMB_SKIP("machine too slow for the timing");
    }
    EMB_REQUIRE(wait_ready(e.rt));
    EMB_CHECK(!relayout(&e));
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_faces_loaded, 0);
    teardown(&e);
}

EMB_TEST(generic_override_and_trim_through_the_runtime_api)
{
    env e;
    if (!setup(&e, 0, 0)) {
        return;
    }
    EMB_REQUIRE(wait_ready(e.rt));
    EMB_CHECK(!emb_runtime_set_generic_family(e.rt, "no-such-generic", nullptr, 0));
    EMB_CHECK(!emb_runtime_set_generic_family(e.rt, nullptr, nullptr, 0));
    const char *fams[] = {"Ember Missing", "Ember Woff"};
    EMB_CHECK(!emb_runtime_set_generic_family(e.rt, "monospace", nullptr, 2)); /* a count without a list */
    EMB_CHECK(emb_runtime_set_generic_family(e.rt, "MONOSPACE", fams, 2));
    load(&e, "<p style=\"font-family:monospace\">AAAA</p>");
    emb_runtime_stats s = stats_of(e.rt);
    EMB_CHECK_EQ_INT(s.system_faces_loaded, 1); /* Ember Woff, the first available family of the override */
    EMB_CHECK(emb_runtime_set_generic_family(e.rt, "monospace", nullptr, 0)); /* the platform's list again */
    /* The document's layout holds the face: trim frees nothing, until the document is replaced. */
    emb_runtime_trim_fonts(e.rt);
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_faces_loaded, 1);
    load(&e, "<style>html{font-family:'Noto Sans'}</style><p>plain</p>");
    emb_runtime_trim_fonts(e.rt);
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_faces_loaded, 0);
    EMB_CHECK_EQ_INT(stats_of(e.rt).system_font_bytes, 0);
    emb_runtime_trim_fonts(nullptr);
    teardown(&e);
}

/* Pumps the view's tasks until every network record of the document has completed and its arrival has been handled. */
static bool pump_until_quiet(env *e)
{
    for (int i = 0; i < 3000; i++) {
        (void)embi_script_pump(embi_doc_script(e->v->doc));
        if (embi_netload_quiet(e->v)) {
            return true;
        }
        SDL_Delay(2);
    }
    return false;
}

EMB_TEST(failed_font_url_does_not_lay_the_page_out_again)
{
    env e;
    if (!setup(&e, EMB_RUNTIME_NO_SYSTEM_FONTS, 0)) {
        return;
    }
    /* t.test is not on the allowlist, so the font request fails; its arrival says "font" (and "script", which every record
     * carries) and must not make the page relayout when it left no other face in its place. */
    uint32_t before = embi_fonts_face_count(e.rt->fonts);
    load(&e, "<style>@font-face{font-family:Gone;src:url(gone.ttf)}</style><p style=\"font-family:Gone\">AAA</p>");
    (void)relayout(&e); /* the load's own layout */
    EMB_REQUIRE(pump_until_quiet(&e));
    EMB_CHECK(!relayout(&e));
    EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), before);
    teardown(&e);
}

EMB_TEST_MAIN()
