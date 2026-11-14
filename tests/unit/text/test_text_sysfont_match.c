#include "sysfont_fixture.h"

#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_timer.h>

/* Matching with system faces (W4i), over tests/fonts/sysfonts:
 *   - the family list resolves registered faces first, then system faces by typographic, legacy and localized name;
 *   - the generic families map through the platform tables or the embedder's override;
 *   - per-character fallback: the generic's list, the per-script preference (Han by lang), then any covering face by
 *     closeness to weight, style and stretch; the embedder's fallback families come before it;
 *   - the first-paint path never waits for the index, and the lookup is redone once the index arrives. */

#define UTF8_HAN "\xE4\xB8\x80"      /* U+4E00 */
#define UTF8_ALEF "\xD8\xA7"         /* U+0627 */
#define JA_NAME "\xE3\x82\xA8\xE3\x83\xB3\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xBB\xE3\x83\xAA\xE3\x83\x95"

/* True when some run of the layout uses a face whose family is name. */
static bool lay_has_family(const embi_text_layout *lay, const char *name)
{
    for (uint32_t r = 0; r < lay->runs.len; r++) {
        if (sysfx_family_is(lay->runs.data[r].face, name)) {
            return true;
        }
    }
    return false;
}

/* The family of the face of the first run. */
static bool lay_first_is(const embi_text_layout *lay, const char *name)
{
    return lay->runs.len > 0 && sysfx_family_is(lay->runs.data[0].face, name);
}

/* Shapes text for a family list and lang. */
static bool shape_with(embi_text *t, const char *const *fams, uint32_t nf, const char *text, const char *lang,
                       float weight, uint8_t font_style, embi_text_layout *out)
{
    embi_str fs[8];
    for (uint32_t i = 0; i < nf; i++) {
        fs[i] = tx_s(fams[i]);
    }
    embi_text_style st;
    tx_style(&st, fs, nf, 20.0f);
    st.weight = weight;
    st.font_style = font_style;
    st.lang = tx_s(lang);
    return tx_shape(t, text, &st, out);
}

static embi_text *text_with_index(const char *const *dirs, uint32_t nd)
{
    embi_text *t = embi_text_create();
    if (t != nullptr && !sysfx_start(embi_text_fonts(t), dirs, nd, nullptr, true)) {
        embi_text_destroy(t);
        return nullptr;
    }
    return t;
}

EMB_TEST(family_resolves_by_typographic_legacy_and_localized_name)
{
    size_t base = embi_mem_live();
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_text_layout lay;
    const char *typo[] = {"Ember Serif Display"};
    EMB_REQUIRE(shape_with(t, typo, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    const char *legacy[] = {"ember serif"};
    EMB_REQUIRE(shape_with(t, legacy, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    const char *ja[] = {JA_NAME};
    EMB_REQUIRE(shape_with(t, ja, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    /* A family with several faces: the weight picks the face (CSS Fonts 4 section 5.2). */
    const char *sans[] = {"Ember Sans"};
    EMB_REQUIRE(shape_with(t, sans, 1, "AB", "", 700, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    EMB_CHECK_EQ_INT(lay.runs.data[0].face->weight_min, 700);
    embi_text_layout_free(&lay);
    EMB_REQUIRE(shape_with(t, sans, 1, "AB", "", 300, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK_EQ_INT(lay.runs.data[0].face->weight_min, 400);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(registered_face_wins_over_a_system_face)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Ember Sans");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    EMB_REQUIRE(sysfx_start(embi_text_fonts(t), nullptr, 0, nullptr, true));
    embi_str fam = tx_s("Ember Sans");
    embi_style_match m = {&fam, 1, 400.0f, EMBI_STYLE_NORMAL, 100.0f};
    embi_face *f = embi_fontdb_resolve_family(embi_text_fonts(t), fam, &m);
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(f->sys_ent1 == 0); /* the registered face, not a system face */
    embi_sys_stats s;
    embi_sys_stats_get(embi_text_fonts(t), &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0);
    embi_text_destroy(t);
}

EMB_TEST(first_paint_does_not_wait_and_the_lookup_is_redone)
{
    size_t base = embi_mem_live();
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    embi_face *def = embi_fontdb_default_face(db);
    if (def == nullptr) {
        embi_text_destroy(t);
        EMB_SKIP("built without the default font");
    }
    embi_sys_opts o;
    SDL_memset(&o, 0, sizeof o);
    static const char *const dirs[] = {SYSFX_DIR};
    o.dirs = dirs;
    o.ndirs = 1;
    o.delay_ms = 400; /* the index job is still waiting while the first layout runs */
    EMB_REQUIRE(embi_sys_start(db, &o));
    EMB_CHECK(!embi_sys_ready(db));
    uint64_t uses0 = embi_fontdb_fallback_uses(db);
    uint64_t t0 = SDL_GetTicksNS();
    embi_text_layout lay;
    const char *sans[] = {"Ember Sans"};
    EMB_REQUIRE(shape_with(t, sans, 1, "ABC" UTF8_HAN, "ja", 400, EMBI_STYLE_NORMAL, &lay));
    double ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
    EMB_CHECK(ms < (EMB_TEST_ASAN ? 380.0 : 300.0)); /* not blocked by the 400 ms job */
    EMB_CHECK(!embi_sys_ready(db));
    /* Until the index is ready only the bundled face is seen, and the layout records that it used a fallback. */
    for (uint32_t r = 0; r < lay.runs.len; r++) {
        EMB_CHECK(lay.runs.data[r].face == def);
    }
    EMB_CHECK(embi_fontdb_fallback_uses(db) > uses0);
    EMB_CHECK_EQ_INT(embi_fontdb_sys_epoch(db), 0);
    embi_text_layout_free(&lay);

    /* The index arrives (epoch 1): the same style now resolves to system faces. */
    for (int i = 0; i < 1000 && !embi_sys_ready(db); i++) {
        SDL_Delay(10);
    }
    EMB_REQUIRE(embi_sys_ready(db));
    EMB_CHECK_EQ_INT(embi_fontdb_sys_epoch(db), 1);
    uint64_t uses1 = embi_fontdb_fallback_uses(db);
    EMB_REQUIRE(shape_with(t, sans, 1, "ABC" UTF8_HAN, "ja", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_has_family(&lay, "Ember Sans"));
    EMB_CHECK(lay_has_family(&lay, "Noto Sans CJK JP"));
    EMB_CHECK_EQ_INT(embi_fontdb_fallback_uses(db) - uses1, 0); /* nothing used a fallback now */
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

/* Registers one @font-face source the way src/style/fontface.c does: the loader's bytes go to embi_fontdb_add. */
static uint32_t face_source_local(embi_fontdb *db, const char *name, const embi_face_desc *d)
{
    uint8_t *data = nullptr;
    size_t len = 0;
    if (!embi_fontdb_local_source(db, tx_s(name), &data, &len)) {
        return UINT32_MAX;
    }
    return embi_fontdb_add(db, data, len, true, d);
}

EMB_TEST(local_source_before_the_index_resolves_when_it_arrives)
{
    size_t base = embi_mem_live();
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    embi_sys_opts o;
    SDL_memset(&o, 0, sizeof o);
    static const char *const dirs[] = {SYSFX_DIR};
    o.dirs = dirs;
    o.ndirs = 1;
    o.delay_ms = 300; /* the stylesheet is collected before the index is ready */
    EMB_REQUIRE(embi_sys_start(db, &o));
    EMB_REQUIRE(!embi_sys_ready(db));
    uint32_t faces0 = embi_fontdb_face_count(db);
    embi_style_match m400 = {nullptr, 0, 400.0f, EMBI_STYLE_NORMAL, 100.0f};

    /* 1. local() only: pending, no face, the next source (none) is tried. */
    embi_face_desc d1 = {0};
    d1.family = tx_s("Late Local");
    EMB_CHECK_EQ_INT(face_source_local(db, "Ember Sans Bold", &d1), 0);
    /* 2. local() then a url() source of the same rule: the url face registers for now. */
    embi_face_desc d2 = {0};
    d2.family = tx_s("Late Both");
    EMB_CHECK_EQ_INT(face_source_local(db, "No Such Face", &d2), 0);
    EMB_CHECK_EQ_INT(face_source_local(db, "EmberSans-Bold", &d2), 0); /* a second local() of the rule */
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d2) == 1);
    /* 3. A local() that is not installed, then a url(): the url face stays once the index is ready. */
    embi_face_desc d3 = {0};
    d3.family = tx_s("Late Miss");
    EMB_CHECK_EQ_INT(face_source_local(db, "Missing Face", &d3), 0);
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d3) == 1);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), faces0 + 2);
    EMB_CHECK_EQ_INT(db->lpend.len, 4);
    EMB_CHECK_EQ_INT(db->lp_waiting, 4);

    /* Before the index: the family of a pending local() counts as a fallback (the layout is redone when the index
     * arrives); the url face answers for now. */
    uint64_t uses = embi_fontdb_fallback_uses(db);
    EMB_CHECK(embi_fontdb_resolve_family(db, tx_s("Late Local"), &m400) == nullptr);
    EMB_CHECK(embi_fontdb_fallback_uses(db) > uses);
    uses = embi_fontdb_fallback_uses(db);
    embi_face *url_face = embi_fontdb_resolve_family(db, tx_s("late both"), &m400);
    EMB_REQUIRE(url_face != nullptr);
    EMB_CHECK_EQ_INT(url_face->weight_min, 400);
    EMB_CHECK(embi_fontdb_fallback_uses(db) > uses);
    uses = embi_fontdb_fallback_uses(db);
    EMB_CHECK(embi_fontdb_resolve_family(db, tx_s("Ember Missing"), &m400) == nullptr); /* no pending source: no count */
    EMB_CHECK_EQ_INT(embi_fontdb_fallback_uses(db), uses);

    for (int i = 0; i < 1000 && !embi_sys_ready(db); i++) {
        SDL_Delay(10);
    }
    EMB_REQUIRE(embi_sys_ready(db));
    /* The index arrived: each family resolves its pending local() sources at its next lookup. */
    embi_face *f1 = embi_fontdb_resolve_family(db, tx_s("Late Local"), &m400);
    EMB_REQUIRE(f1 != nullptr);
    EMB_CHECK(sysfx_family_is(f1, "Late Local"));
    EMB_CHECK_EQ_INT(f1->weight_min, 700); /* Ember Sans Bold's OS/2 weight */
    EMB_CHECK_EQ_INT(f1->sys_ent1, 0);     /* registered under the @font-face family, not an index face */
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), faces0 + 3);
    /* The rule's second local() (a PostScript name) wins over its url() face, which is superseded. */
    embi_face *f2 = embi_fontdb_resolve_family(db, tx_s("Late Both"), &m400);
    EMB_REQUIRE(f2 != nullptr);
    EMB_CHECK(f2 != url_face);
    EMB_CHECK_EQ_INT(f2->weight_min, 700);
    EMB_CHECK(url_face->superseded);
    /* Not installed: the url() face stays. */
    embi_face *f3 = embi_fontdb_resolve_family(db, tx_s("Late Miss"), &m400);
    EMB_REQUIRE(f3 != nullptr);
    EMB_CHECK_EQ_INT(f3->weight_min, 400);
    EMB_CHECK(!f3->superseded);
    EMB_CHECK_EQ_INT(db->lp_waiting, 0);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(db), faces0 + 4);
    uses = embi_fontdb_fallback_uses(db);
    EMB_CHECK(embi_fontdb_resolve_family(db, tx_s("Late Local"), &m400) == f1); /* resolved once */
    EMB_CHECK_EQ_INT(embi_fontdb_fallback_uses(db), uses);
    /* After the index is ready, a local() source resolves at once. */
    embi_face_desc d4 = {0};
    d4.family = tx_s("Late Ready");
    EMB_CHECK_EQ_INT(face_source_local(db, "Ember Sans Bold", &d4), 1);
    EMB_CHECK_EQ_INT(face_source_local(db, "Missing Face", &d4), UINT32_MAX);
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(generic_families_use_the_embedder_override)
{
    size_t base = embi_mem_live();
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    const embi_str over[] = {tx_s("Ember Missing"), tx_s("Ember Serif"), tx_s("Ember Sans")};
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, over, 3));
    embi_text_layout lay;
    const char *g[] = {"sans-serif"};
    EMB_REQUIRE(shape_with(t, g, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif")); /* the first available family of the list */
    embi_text_layout_free(&lay);
    const char *g2[] = {"SANS-SERIF"}; /* the keyword is ASCII case-insensitive */
    EMB_REQUIRE(shape_with(t, g2, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    /* A named family before the generic wins; a missing one is skipped. */
    const char *g3[] = {"Ember Missing", "Ember Sans", "sans-serif"};
    EMB_REQUIRE(shape_with(t, g3, 3, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    embi_text_layout_free(&lay);
    /* Another generic keeps its own list. */
    const embi_str mono[] = {tx_s("Ember Woff")};
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_MONOSPACE, mono, 1));
    const char *gm[] = {"monospace"};
    EMB_REQUIRE(shape_with(t, gm, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Woff"));
    embi_text_layout_free(&lay);
    /* Count 0 restores the platform's list, which names no fixture family: the generic resolves to nothing, and the
     * text falls through to the per-character system fallback (the upright Ember Sans is closest to a normal style). */
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, nullptr, 0));
    EMB_CHECK_EQ_INT(db->gen_over_n[EMBI_GEN_SANS_SERIF], 0);
    EMB_REQUIRE(shape_with(t, g, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(!lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(fallback_cache_follows_generic_and_fallback_changes)
{
    size_t base = embi_mem_live();
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    /* "Ember Missing" is not installed: 'A' is found by the per-character fallback, whose answer is cached. */
    const char *fam[] = {"Ember Missing"};
    embi_text_layout lay;
    const embi_str serif[] = {tx_s("Ember Serif")};
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, serif, 1));
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    /* A new generic list must not be answered from the old cache entry. */
    const embi_str sans[] = {tx_s("Ember Sans")};
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, sans, 1));
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    embi_text_layout_free(&lay);
    /* The same for the embedder's fallback families. */
    const embi_str fb[] = {tx_s("Ember Woff")};
    EMB_REQUIRE(embi_fontdb_set_fallback(db, fb, 1));
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Woff"));
    embi_text_layout_free(&lay);
    EMB_REQUIRE(embi_fontdb_set_fallback(db, nullptr, 0));
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    embi_text_layout_free(&lay);
    /* Removing an override (count 0) drops the cached answers too. The platform's list names no fixture family (on Linux
     * it names Noto Sans, the bundled face's family), so 'A' comes from that list or the any-face path, where an upright
     * face beats the italic Ember Serif of the old override. */
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, serif, 1));
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    EMB_CHECK(db->cover.len > 0);
    EMB_REQUIRE(embi_fontdb_set_generic(db, EMBI_GEN_SANS_SERIF, nullptr, 0));
    EMB_CHECK_EQ_INT(db->cover.len, 0);
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(!lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    /* A registered face is a candidate at once: the bundled default is not what a later registration shadows, but the
     * cache is dropped, so the lookup is made again. */
    uint32_t before = db->cover.len;
    EMB_CHECK(before > 0);
    embi_face_desc desc = {0};
    desc.family = tx_s("Ember Added");
    extern const uint8_t embi_default_font_data[];
    extern const size_t embi_default_font_size;
    /* Without the bundled font (EMBER_DEFAULT_FONT=OFF) the same bytes come from the source tree. */
    const uint8_t *fdata = embi_default_font_data;
    size_t fsize = embi_default_font_size;
    void *loaded = nullptr;
    if (fsize == 0) {
        loaded = SDL_LoadFile(EMBER_SOURCE_DIR "/assets/fonts/ember-default.ttf", &fsize);
        fdata = loaded;
    }
    EMB_CHECK(embi_fontdb_add(db, fdata, fsize, false, &desc) > 0);
    EMB_CHECK_EQ_INT(db->cover.len, 0);
    const char *added[] = {"Ember Added"};
    EMB_REQUIRE(shape_with(t, added, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Added"));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
    SDL_free(loaded);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(generic_tables_exist_for_the_platform)
{
    static const char *const names[] = {"serif",      "sans-serif", "monospace",    "cursive", "fantasy",
                                        "system-ui",  "ui-serif",   "ui-sans-serif", "ui-monospace", "ui-rounded",
                                        "emoji",      "math",       "fangsong"};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        int g = embi_gen_index(tx_s(names[i]));
        EMB_CHECK_EQ_INT(g, (int)i);
    }
    EMB_CHECK_EQ_INT(embi_gen_index(tx_s("Monospace")), EMBI_GEN_MONOSPACE);
    EMB_CHECK_EQ_INT(embi_gen_index(tx_s("sans")), -1);
    EMB_CHECK_EQ_INT(embi_gen_index(tx_s("Arial")), -1);
    const char *plat = SDL_GetPlatform();
    const char *first = nullptr;
    if (SDL_strcmp(plat, "Windows") == 0) {
        first = "Arial";
    } else if (SDL_strcmp(plat, "Linux") == 0) {
        first = "DejaVu Sans";
    } else if (SDL_strcmp(plat, "macOS") == 0 || SDL_strcmp(plat, "Mac OS X") == 0) {
        first = "Helvetica";
    }
    for (uint32_t g = 0; g < EMBI_GEN_COUNT; g++) {
        const char *const *l = embi_sys_generic_default(g);
        if (first != nullptr) {
            EMB_REQUIRE(l != nullptr && l[0] != nullptr);
        }
    }
    if (first != nullptr) {
        EMB_CHECK_EQ_STR(embi_sys_generic_default(EMBI_GEN_SANS_SERIF)[0], first);
    }
    EMB_CHECK(embi_sys_generic_default(EMBI_GEN_COUNT) == nullptr);
}

EMB_TEST(han_fallback_follows_lang)
{
    size_t base = embi_mem_live();
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    static const struct {
        const char *lang, *want;
    } cases[] = {{"ja", "Noto Sans CJK JP"},       {"ja-JP", "Noto Sans CJK JP"}, {"ko", "Noto Sans CJK KR"},
                 {"zh-Hans", "Noto Sans CJK SC"}, {"zh-CN", "Noto Sans CJK SC"}, {"zh", "Noto Sans CJK SC"},
                 {"zh-Hant", "Noto Sans CJK TC"}, {"zh-TW", "Noto Sans CJK TC"}, {"zh-HK", "Noto Sans CJK TC"},
                 {"", "Noto Sans CJK SC"},        {"fr", "Noto Sans CJK SC"}};
    const char *sans[] = {"Ember Sans"};
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_text_layout lay;
        EMB_REQUIRE(shape_with(t, sans, 1, "A" UTF8_HAN "B", cases[i].lang, 400, EMBI_STYLE_NORMAL, &lay));
        if (!lay_has_family(&lay, cases[i].want) || !lay_has_family(&lay, "Ember Sans")) {
            SDL_Log("lang '%s': wanted %s", cases[i].lang, cases[i].want);
            EMB_CHECK(false);
        }
        embi_text_layout_free(&lay);
    }
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(arabic_uses_its_script_preference)
{
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_text_layout lay;
    const char *sans[] = {"Ember Sans"};
    EMB_REQUIRE(shape_with(t, sans, 1, UTF8_ALEF, "ar", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_has_family(&lay, "Noto Sans Arabic"));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(any_covering_face_by_closeness)
{
    /* Only Ember Sans (400, 700) and Ember Serif (italic) cover 'A' here; the requested family does not. */
    static const char *const dirs[] = {SYSFX_DIR "/sans", SYSFX_DIR "/deep"};
    embi_text *t = text_with_index(dirs, 2);
    EMB_REQUIRE(t != nullptr);
    /* The platform's sans-serif list names families (Noto Sans on Linux, which is also the bundled face's family): point
     * it at nothing, so the result does not depend on the platform. */
    const embi_str none[] = {tx_s("Ember Missing")};
    EMB_REQUIRE(embi_fontdb_set_generic(embi_text_fonts(t), EMBI_GEN_SANS_SERIF, none, 1));
    const char *cjk[] = {"Ember CJK"}; /* not installed in this tree: nothing in the family list covers 'A' */
    embi_text_layout lay;
    EMB_REQUIRE(shape_with(t, cjk, 1, "A", "", 700, EMBI_STYLE_NORMAL, &lay));
    EMB_REQUIRE(lay.runs.len >= 1 && lay.runs.data[0].face != nullptr);
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    EMB_CHECK_EQ_INT(lay.runs.data[0].face->weight_min, 700);
    embi_text_layout_free(&lay);
    EMB_REQUIRE(shape_with(t, cjk, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Sans"));
    EMB_CHECK_EQ_INT(lay.runs.data[0].face->weight_min, 400);
    embi_text_layout_free(&lay);
    EMB_REQUIRE(shape_with(t, cjk, 1, "A", "", 400, EMBI_STYLE_ITALIC, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Serif"));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(fallback_miss_reads_no_face_whose_block_bit_is_clear)
{
    /* U+4E00 is in no block of the sans fixtures: walking the generic list, the script preferences and "any face" must
     * not read a single system face file (the 256-code-point block bits rule them out). */
    static const char *const dirs[] = {SYSFX_DIR "/sans"};
    embi_text *t = text_with_index(dirs, 1);
    EMB_REQUIRE(t != nullptr);
    const embi_str list[] = {tx_s("Ember Sans"), tx_s("Ember Serif")};
    EMB_REQUIRE(embi_fontdb_set_generic(embi_text_fonts(t), EMBI_GEN_SANS_SERIF, list, 2));
    const char *fam[] = {"Ember Missing"};
    embi_text_layout lay;
    EMB_REQUIRE(shape_with(t, fam, 1, UTF8_HAN, "ja", 400, EMBI_STYLE_NORMAL, &lay));
    embi_text_layout_free(&lay);
    embi_sys_stats s;
    embi_sys_stats_get(embi_text_fonts(t), &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0);
    EMB_CHECK_EQ_INT(s.font_bytes, 0);
    /* U+00E9 is in block 0, whose bit the ASCII fixtures set, but their cmaps do not map it: the cmap probe rules every
     * face out, and no file is loaded whole. */
    uint64_t uses = embi_fontdb_fallback_uses(embi_text_fonts(t));
    EMB_REQUIRE(shape_with(t, fam, 1, "\xC3\xA9", "", 400, EMBI_STYLE_NORMAL, &lay));
    embi_text_layout_free(&lay);
    embi_sys_stats_get(embi_text_fonts(t), &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0);
    EMB_CHECK(embi_fontdb_fallback_uses(embi_text_fonts(t)) > uses); /* it was a fallback miss */
    for (uint32_t i = 0; i < embi_sys_count(embi_text_fonts(t)->sys); i++) {
        EMB_CHECK_EQ_INT(embi_sys_probe(embi_text_fonts(t)->sys, i, 0xE9), 0);
        EMB_CHECK_EQ_INT(embi_sys_probe(embi_text_fonts(t)->sys, i, 'A'), 1);
    }
    /* A covered code point still loads exactly one face through the same path. */
    EMB_REQUIRE(shape_with(t, fam, 1, "A", "", 400, EMBI_STYLE_NORMAL, &lay));
    embi_text_layout_free(&lay);
    embi_sys_stats_get(embi_text_fonts(t), &s);
    EMB_CHECK(s.faces_loaded >= 1);
    embi_text_destroy(t);
}

EMB_TEST(embedder_fallback_comes_before_system_fallback)
{
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    const embi_str fb[] = {tx_s("Ember Woff")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    const char *fams[] = {"Ember Missing"};
    embi_text_layout lay;
    EMB_REQUIRE(shape_with(t, fams, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay_first_is(&lay, "Ember Woff"));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(named_bundled_family_beats_the_system_fallback)
{
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_face *def = embi_fontdb_default_face(embi_text_fonts(t));
    if (def == nullptr) {
        embi_text_destroy(t);
        EMB_SKIP("built without the default font");
    }
    /* A style that names the bundled face's own family gets it, not a system face that also covers the text. */
    const char *noto[] = {"Noto Sans"};
    embi_text_layout lay;
    EMB_REQUIRE(shape_with(t, noto, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay.runs.data[0].face == def);
    embi_text_layout_free(&lay);
    /* A style that does not name it goes through the system fallback before the bundled face (the override keeps the
     * platform's sans-serif list, which names Noto Sans on Linux, out of it). */
    const embi_str nothing[] = {tx_s("Ember Missing")};
    EMB_REQUIRE(embi_fontdb_set_generic(embi_text_fonts(t), EMBI_GEN_SANS_SERIF, nothing, 1));
    const char *none[] = {"Ember Missing"};
    EMB_REQUIRE(shape_with(t, none, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay.runs.data[0].face != def);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(without_an_index_the_bundled_face_is_used)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_fontdb *db = embi_text_fonts(t);
    embi_face *def = embi_fontdb_default_face(db);
    if (def == nullptr) {
        embi_text_destroy(t);
        EMB_SKIP("built without the default font");
    }
    embi_str fam = tx_s("Ember Sans");
    embi_style_match m = {&fam, 1, 400.0f, EMBI_STYLE_NORMAL, 100.0f};
    EMB_CHECK(embi_fontdb_resolve_family(db, fam, &m) == nullptr);
    EMB_CHECK(embi_fontdb_cover_face(db, 0x4E00, &m, tx_s("ja")) == nullptr);
    embi_text_layout lay;
    const char *fams[] = {"Ember Sans"};
    EMB_REQUIRE(shape_with(t, fams, 1, "AB", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK(lay.runs.data[0].face == def);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(grapheme_cluster_is_not_split_across_faces)
{
    /* "A" + U+0301 (combining acute): Ember Sans covers A but not U+0301. The cluster stays in one face. */
    embi_text *t = text_with_index(nullptr, 0);
    EMB_REQUIRE(t != nullptr);
    embi_text_layout lay;
    const char *sans[] = {"Ember Sans"};
    EMB_REQUIRE(shape_with(t, sans, 1, "A\xCC\x81", "", 400, EMBI_STYLE_NORMAL, &lay));
    EMB_CHECK_EQ_INT(lay.runs.len, 1);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
