#include "sysfont_fixture.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_storage.h>
#include <SDL3/SDL_timer.h>

/* The system font index over tests/fonts/sysfonts (W4i): discovery and recursion, the file filter, the names of every
 * tier, .ttc faces, WOFF, the coverage bits, local() lookup, lazy loading and trim, and the index cache (cold, warm and
 * a changed file). Tests check that embi_mem_live returns to its baseline. */

#define JA_NAME "\xE3\x82\xA8\xE3\x83\xB3\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xBB\xE3\x83\xAA\xE3\x83\x95"

EMB_TEST(discovers_recursively_and_skips_other_files)
{
    size_t base = embi_mem_live();
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(db != nullptr);
    EMB_CHECK(!embi_sys_ready(db));
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    EMB_REQUIRE(embi_sys_ready(db));
    embi_sys_stats s;
    embi_sys_stats_get(db, &s);
    EMB_CHECK(s.ready);
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.files, SYSFX_FILES);    /* the .fon and the .txt are not font files */
    EMB_CHECK_EQ_INT(s.entries, SYSFX_FACES);  /* the .ttc holds two faces */
    EMB_CHECK_EQ_INT(s.reparsed, SYSFX_FILES); /* cold: every file is parsed */
    EMB_CHECK_EQ_INT(s.epoch, 1);
    EMB_CHECK(s.index_bytes > 0);
    EMB_CHECK(s.index_ms >= 0.0);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0); /* nothing is loaded until matching picks a face */
    /* Each indexed path is a font file of the tree (nested directories were entered). */
    bool deep = false, ignored = false;
    for (uint32_t i = 0; i < embi_sys_count(db->sys); i++) {
        uint32_t fi = 0;
        const char *p = embi_sys_path(db->sys, i, &fi);
        EMB_REQUIRE(p != nullptr);
        deep = deep || SDL_strstr(p, "EmberSerif-Italic.ttf") != nullptr;
        ignored = ignored || SDL_strstr(p, "EmberIgnored.fon") != nullptr || SDL_strstr(p, "readme.txt") != nullptr;
    }
    EMB_CHECK(deep);
    EMB_CHECK(!ignored);
    SDL_Log("fixture index: %u files, %u faces, %.3f ms, %llu bytes of index", s.files, s.entries, s.index_ms,
            (unsigned long long)s.index_bytes);
    embi_fontdb_destroy(db);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(family_names_tiers_and_case)
{
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Sans"), 2);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "ember SANS"), 2);          /* ASCII case-insensitive */
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Serif Display"), 1); /* typographic family (name ID 16) */
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Serif"), 1);         /* the legacy family (name ID 1) */
    EMB_CHECK_EQ_INT(sysfx_family_count(db, JA_NAME), 1);               /* a localized family name */
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember CJK"), 1);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Woff"), 1);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Missing"), 0);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, ""), 0);
    /* The metadata of the entries. */
    uint32_t bold = sysfx_find(db, "Ember Sans Bold");
    uint32_t reg = sysfx_find(db, "Ember Sans Regular");
    uint32_t ital = sysfx_find(db, "EmberSerif-Italic");
    EMB_REQUIRE(bold != UINT32_MAX && reg != UINT32_MAX && ital != UINT32_MAX);
    embi_fmeta m;
    bool color = true;
    EMB_REQUIRE(embi_sys_meta(db->sys, bold, &m, &color));
    EMB_CHECK(m.wlo == 700.0f && m.whi == 700.0f);
    EMB_CHECK_EQ_INT(m.style, EMBI_STYLE_NORMAL);
    EMB_CHECK(m.slo == 100.0f); /* usWidthClass 5 */
    EMB_CHECK(!color);
    EMB_REQUIRE(embi_sys_meta(db->sys, reg, &m, &color));
    EMB_CHECK(m.wlo == 400.0f);
    EMB_REQUIRE(embi_sys_meta(db->sys, ital, &m, &color));
    EMB_CHECK_EQ_INT(m.style, EMBI_STYLE_ITALIC);
    embi_fontdb_destroy(db);
}

EMB_TEST(ttc_faces_are_separate_entries)
{
    size_t base = embi_mem_live();
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    uint32_t a = sysfx_find(db, "Ember Collection A Regular");
    uint32_t b = sysfx_find(db, "EmberCollectionB-Bold");
    EMB_REQUIRE(a != UINT32_MAX && b != UINT32_MAX && a != b);
    uint32_t fa = 99, fb = 99;
    const char *pa = embi_sys_path(db->sys, a, &fa);
    const char *pb = embi_sys_path(db->sys, b, &fb);
    EMB_REQUIRE(pa != nullptr && pb != nullptr);
    EMB_CHECK_EQ_STR(pa, pb);
    EMB_CHECK(SDL_strstr(pa, "collection.ttc") != nullptr);
    EMB_CHECK_EQ_INT(fa, 0);
    EMB_CHECK_EQ_INT(fb, 1);
    /* The loaded face is the right member of the collection. */
    embi_face *f = embi_fontdb_system_face(db, b);
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(sysfx_family_is(f, "Ember Collection B"));
    EMB_CHECK_EQ_INT(f->weight_min, 700);
    embi_fontdb_destroy(db);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(woff_file_is_indexed_and_loads)
{
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    uint32_t w = sysfx_find(db, "Ember Woff Regular");
    EMB_REQUIRE(w != UINT32_MAX);
    embi_face *f = embi_fontdb_system_face(db, w);
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(sysfx_family_is(f, "Ember Woff"));
    EMB_CHECK_EQ_INT(embi_font_glyph_index(&f->font, 'A'), 34); /* U+0020 is gid 1, so 'A' (0x41) is 34 */
    embi_fontdb_destroy(db);
}

EMB_TEST(coverage_bits_are_per_256_block)
{
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    uint32_t sans = sysfx_find(db, "Ember Sans Regular");
    uint32_t cjk = sysfx_find(db, "Ember CJK Regular");
    uint32_t ar = sysfx_find(db, "Noto Sans Arabic Regular");
    EMB_REQUIRE(sans != UINT32_MAX && cjk != UINT32_MAX && ar != UINT32_MAX);
    EMB_CHECK(embi_sys_block(db->sys, sans, 'A'));
    EMB_CHECK(!embi_sys_block(db->sys, sans, 0x4E00));
    EMB_CHECK(!embi_sys_block(db->sys, sans, 0x1F600));
    EMB_CHECK(embi_sys_block(db->sys, cjk, 0x4E02));
    EMB_CHECK(embi_sys_block(db->sys, cjk, 0x4EFF)); /* the same 256-code-point block: a superset */
    EMB_CHECK(!embi_sys_block(db->sys, cjk, 0x4F00));
    EMB_CHECK(!embi_sys_block(db->sys, cjk, 'A'));
    EMB_CHECK(embi_sys_block(db->sys, ar, 0x0628));
    EMB_CHECK(!embi_sys_block(db->sys, ar, 0x4E00));
    EMB_CHECK(!embi_sys_block(db->sys, sans, 0x110000)); /* out of range */
    embi_fontdb_destroy(db);
}

EMB_TEST(local_lookup_by_full_and_postscript_name)
{
    size_t base = embi_mem_live();
    embi_fontdb *db = embi_fontdb_create();
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    EMB_CHECK(sysfx_find(db, "Ember Sans Bold") != UINT32_MAX); /* full name */
    EMB_CHECK(sysfx_find(db, "EmberSans-Bold") != UINT32_MAX);  /* PostScript name */
    EMB_CHECK(sysfx_find(db, "ember sans bold") != UINT32_MAX); /* case-insensitive */
    EMB_CHECK(sysfx_find(db, "EMBERSANS-BOLD") != UINT32_MAX);
    EMB_CHECK(sysfx_find(db, "Ember Sans") == UINT32_MAX); /* a family name is not a full name */
    EMB_CHECK(sysfx_find(db, "Ember Sans Black") == UINT32_MAX);
    EMB_CHECK(sysfx_find(db, "") == UINT32_MAX);
    /* The bytes (the local() source): a font that opens with the right glyphs. */
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_REQUIRE(embi_fontdb_local(db, tx_s("embersans-bold"), &data, &len));
    EMB_REQUIRE(data != nullptr && len > 100);
    embi_font f;
    EMB_REQUIRE(embi_font_open(data, len, 0, &f));
    EMB_CHECK(embi_font_glyph_index(&f, 'A') > 0);
    embi_font_close(&f);
    SDL_free(data);
    data = nullptr;
    EMB_CHECK(!embi_fontdb_local(db, tx_s("No Such Font"), &data, &len));
    EMB_CHECK(data == nullptr);
    embi_fontdb_destroy(db);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(local_miss_without_index)
{
    embi_fontdb *db = embi_fontdb_create();
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_CHECK(!embi_fontdb_local(db, tx_s("Ember Sans Bold"), &data, &len)); /* no index at all */
    embi_fontdb_destroy(db);
}

EMB_TEST(lazy_loading_and_trim)
{
    size_t base = embi_mem_live();
    embi_text *t = embi_text_create();
    embi_fontdb *db = embi_text_fonts(t);
    EMB_REQUIRE(sysfx_start(db, nullptr, 0, nullptr, true));
    embi_sys_stats s;
    embi_sys_stats_get(db, &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0);
    EMB_CHECK_EQ_INT(s.font_bytes, 0);
    /* Shaping with a system family loads exactly the matched face. */
    embi_str fam = tx_s("Ember Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    st.weight = 700.0f;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "ABC", &st, &lay));
    embi_sys_stats_get(db, &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 1);
    EMB_CHECK(s.font_bytes > 1000);
    EMB_REQUIRE(lay.runs.len >= 1);
    EMB_CHECK(sysfx_family_is(lay.runs.data[0].face, "Ember Sans"));
    EMB_CHECK_EQ_INT(lay.runs.data[0].face->weight_min, 700);
    /* A live layout keeps the face: trim frees nothing. */
    EMB_CHECK_EQ_INT(embi_fontdb_trim_system(db, t), 0);
    embi_sys_stats_get(db, &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 1);
    /* Once the layout is gone, trim frees the face, and the next shaping loads it again. */
    embi_text_layout_free(&lay);
    EMB_CHECK_EQ_INT(embi_fontdb_trim_system(db, t), 1);
    embi_sys_stats_get(db, &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 0);
    EMB_CHECK_EQ_INT(s.font_bytes, 0);
    EMB_REQUIRE(tx_shape(t, "ABC", &st, &lay));
    embi_sys_stats_get(db, &s);
    EMB_CHECK_EQ_INT(s.faces_loaded, 1);
    EMB_CHECK_EQ_INT(lay.glyphs.len, 3);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

/* ------------------------------------------------------------------ the cache */

/* A scratch directory under the user's preference path, with a copy of two fixture fonts. */
typedef struct scratch {
    char dir[512];
    char a[560], b[560];
} scratch;

static bool scratch_make(scratch *sc)
{
    char *pref = SDL_GetPrefPath("ember", "w4i-test");
    if (pref == nullptr) {
        return false;
    }
    SDL_snprintf(sc->dir, sizeof sc->dir, "%sfonts", pref);
    SDL_free(pref);
    SDL_RemovePath(sc->dir);
    if (!SDL_CreateDirectory(sc->dir)) {
        return false;
    }
    SDL_snprintf(sc->a, sizeof sc->a, "%s/a.ttf", sc->dir);
    SDL_snprintf(sc->b, sizeof sc->b, "%s/b.ttf", sc->dir);
    return SDL_CopyFile(SYSFX_DIR "/sans/EmberSans-Regular.ttf", sc->a) &&
           SDL_CopyFile(SYSFX_DIR "/sans/EmberSans-Bold.ttf", sc->b);
}

/* Writes the bytes of src to dst as a new file (SDL_CopyFile would keep the modification time on Windows). */
static bool rewrite_file(const char *src, const char *dst)
{
    size_t n = 0;
    void *p = SDL_LoadFile(src, &n);
    bool ok = p != nullptr && SDL_SaveFile(dst, p, n);
    SDL_free(p);
    return ok;
}

static SDL_Storage *cache_storage(void)
{
    SDL_Storage *st = SDL_OpenUserStorage("ember", "fontcache", 0);
    for (int i = 0; st != nullptr && i < 200 && !SDL_StorageReady(st); i++) {
        SDL_Delay(10);
    }
    return st;
}

static void cache_remove(const char *name)
{
    SDL_Storage *st = cache_storage();
    if (st != nullptr) {
        SDL_RemoveStoragePath(st, name);
        SDL_CloseStorage(st);
    }
}

static bool index_once(const char *dir, const char *cache, embi_sys_stats *out, uint32_t *sans)
{
    embi_fontdb *db = embi_fontdb_create();
    const char *dirs[] = {dir};
    bool ok = sysfx_start(db, dirs, 1, cache, true);
    embi_sys_stats_get(db, out);
    if (sans != nullptr) {
        *sans = sysfx_family_count(db, "Ember Sans");
    }
    embi_fontdb_destroy(db);
    return ok;
}

EMB_TEST(cache_cold_warm_and_changed_file)
{
    size_t base = embi_mem_live();
    static const char *const name = "w4i-test-cache.bin";
    scratch sc;
    if (!scratch_make(&sc)) {
        EMB_SKIP("no writable preference path");
    }
    cache_remove(name);

    embi_sys_stats s;
    uint32_t sans = 0;
    /* Cold: no cache file, so both files are parsed, and the cache is written. */
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.files, 2);
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 2);
    EMB_CHECK_EQ_INT(sans, 2);
    double cold_ms = s.index_ms;

    /* Warm: the cache holds every file with the same size and mtime, so nothing is parsed. */
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK(s.warm);
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 0);
    EMB_CHECK_EQ_INT(sans, 2);
    SDL_Log("index cache: cold %.3f ms, warm %.3f ms (2 faces)", cold_ms, s.index_ms);

    /* A changed file (same size, new mtime) is parsed again; the other one is not. */
    SDL_Delay(50);
    EMB_REQUIRE(rewrite_file(SYSFX_DIR "/sans/EmberSans-Regular.ttf", sc.a));
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 1);

    /* The new cache is warm again. */
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK(s.warm);
    EMB_CHECK_EQ_INT(s.reparsed, 0);

    /* A removed file leaves the index; a new file is parsed. */
    EMB_REQUIRE(SDL_RemovePath(sc.b));
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK_EQ_INT(s.entries, 1);
    EMB_CHECK_EQ_INT(s.reparsed, 0);
    EMB_REQUIRE(SDL_CopyFile(SYSFX_DIR "/woff/EmberWoff.woff", sc.b));
    EMB_REQUIRE(index_once(sc.dir, name, &s, &sans));
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 1);

    cache_remove(name);
    SDL_RemovePath(sc.a);
    SDL_RemovePath(sc.b);
    SDL_RemovePath(sc.dir);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(file_without_faces_is_cached_and_not_parsed_again)
{
    size_t base = embi_mem_live();
    static const char *const name = "w4i-test-empty.bin";
    scratch sc;
    if (!scratch_make(&sc)) {
        EMB_SKIP("no writable preference path");
    }
    cache_remove(name);
    char junk_path[600];
    SDL_snprintf(junk_path, sizeof junk_path, "%s/broken.ttf", sc.dir);
    static const char junk[] = "this is not a font file, only its name says so";
    EMB_REQUIRE(SDL_SaveFile(junk_path, junk, sizeof junk));

    embi_sys_stats s;
    EMB_REQUIRE(index_once(sc.dir, name, &s, nullptr));
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.files, 3);
    EMB_CHECK_EQ_INT(s.entries, 2); /* the broken file adds no entry to the index */
    EMB_CHECK_EQ_INT(s.reparsed, 3);
    /* Warm: the cache keeps a zero-face record of the broken file, so nothing is parsed, and the index is the same. */
    EMB_REQUIRE(index_once(sc.dir, name, &s, nullptr));
    EMB_CHECK(s.warm);
    EMB_CHECK_EQ_INT(s.files, 3);
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 0);
    /* A changed broken file is parsed again (and still has no face). */
    SDL_Delay(50);
    EMB_REQUIRE(SDL_SaveFile(junk_path, "still not a font, but longer than before", 41));
    EMB_REQUIRE(index_once(sc.dir, name, &s, nullptr));
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.entries, 2);
    EMB_CHECK_EQ_INT(s.reparsed, 1);

    cache_remove(name);
    SDL_RemovePath(junk_path);
    SDL_RemovePath(sc.a);
    SDL_RemovePath(sc.b);
    SDL_RemovePath(sc.dir);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(corrupt_cache_is_ignored)
{
    static const char *const name = "w4i-test-corrupt.bin";
    SDL_Storage *st = cache_storage();
    if (st == nullptr) {
        EMB_SKIP("no user storage");
    }
    uint8_t junk[64];
    SDL_memset(junk, 0xA5, sizeof junk);
    bool wrote = SDL_WriteStorageFile(st, name, junk, sizeof junk);
    SDL_CloseStorage(st);
    EMB_REQUIRE(wrote);
    embi_sys_stats s;
    EMB_REQUIRE(index_once(SYSFX_DIR, name, &s, nullptr));
    EMB_CHECK(!s.warm);
    EMB_CHECK_EQ_INT(s.entries, SYSFX_FACES);
    EMB_CHECK_EQ_INT(s.reparsed, SYSFX_FILES);
    /* The rewritten cache is good. */
    EMB_REQUIRE(index_once(SYSFX_DIR, name, &s, nullptr));
    EMB_CHECK(s.warm);
    EMB_CHECK_EQ_INT(s.entries, SYSFX_FACES);
    cache_remove(name);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

EMB_TEST(cache_with_an_oversized_alt_count_frees_everything)
{
    /* A cache entry whose alternate-name count is larger than the limit is refused; the strings already read for the
     * entry are freed (embi_mem_live returns to its baseline). */
    static const char *const name = "w4i-test-nalt.bin";
    SDL_Storage *st = cache_storage();
    if (st == nullptr) {
        EMB_SKIP("no user storage");
    }
    cache_remove(name);
    SDL_CloseStorage(st);
    embi_sys_stats s;
    EMB_REQUIRE(index_once(SYSFX_DIR, name, &s, nullptr));
    st = cache_storage();
    EMB_REQUIRE(st != nullptr);
    uint64_t len = 0;
    if (!SDL_GetStorageFileSize(st, name, &len)) {
        len = 0;
    }
    EMB_REQUIRE(len > 64);
    uint8_t *buf = SDL_malloc((size_t)len);
    EMB_REQUIRE(buf != nullptr && SDL_ReadStorageFile(st, name, buf, len));
    /* Entry 0: path, face u32, size u64, mtime u64, legacy, typo, full, ps (strings: u32 length, 0xFFFFFFFF = none), nalt. */
    size_t at = 12;
    uint32_t n = le32(buf + 8);
    EMB_REQUIRE(n > 0);
    for (int k = 0; k < 5; k++) {
        if (k == 1) {
            at += 4 + 8 + 8;
        }
        uint32_t sl = le32(buf + at);
        at += 4 + (sl == 0xFFFFFFFFu ? 0 : sl);
    }
    EMB_REQUIRE(at + 4 < len);
    buf[at] = 0xE8;
    buf[at + 1] = 0x03;
    buf[at + 2] = buf[at + 3] = 0; /* nalt = 1000 */
    EMB_REQUIRE(SDL_WriteStorageFile(st, name, buf, (size_t)len));
    SDL_CloseStorage(st);
    SDL_free(buf);
    size_t base = embi_mem_live();
    EMB_REQUIRE(index_once(SYSFX_DIR, name, &s, nullptr));
    EMB_CHECK(!s.warm); /* refused: built cold */
    EMB_CHECK_EQ_INT(s.entries, SYSFX_FACES);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
    cache_remove(name);
}

EMB_TEST(job_thread_does_not_block_and_delivers)
{
    size_t base = embi_mem_live();
    embi_fontdb *db = embi_fontdb_create();
    embi_sys_opts o;
    SDL_memset(&o, 0, sizeof o);
    static const char *const dirs[] = {SYSFX_DIR};
    o.dirs = dirs;
    o.ndirs = 1;
    o.delay_ms = 300; /* the job waits: start returns at once */
    uint64_t t0 = SDL_GetTicks();
    EMB_REQUIRE(embi_sys_start(db, &o));
    EMB_CHECK(SDL_GetTicks() - t0 < (EMB_TEST_ASAN ? 280 : 200));
    EMB_CHECK(!embi_sys_ready(db));
    EMB_CHECK_EQ_INT(embi_fontdb_sys_epoch(db), 0);
    uint8_t *data = nullptr;
    size_t len = 0;
    EMB_CHECK(!embi_fontdb_local(db, tx_s("Ember Sans Bold"), &data, &len)); /* not ready: a miss, not a wait */
    for (int i = 0; i < 1000 && !embi_sys_ready(db); i++) {
        SDL_Delay(10);
    }
    EMB_CHECK(embi_sys_ready(db));
    EMB_CHECK_EQ_INT(embi_fontdb_sys_epoch(db), 1);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Sans"), 2);
    embi_fontdb_destroy(db);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(destroy_while_indexing_cancels)
{
    size_t base = embi_mem_live();
    embi_fontdb *db = embi_fontdb_create();
    embi_sys_opts o;
    SDL_memset(&o, 0, sizeof o);
    static const char *const dirs[] = {SYSFX_DIR};
    o.dirs = dirs;
    o.ndirs = 1;
    o.delay_ms = 200;
    EMB_REQUIRE(embi_sys_start(db, &o));
    embi_fontdb_destroy(db); /* waits for the job and frees everything */
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(missing_directory_gives_an_empty_index)
{
    embi_fontdb *db = embi_fontdb_create();
    const char *dirs[] = {EMBER_SOURCE_DIR "/tests/fonts/no-such-dir"};
    EMB_REQUIRE(sysfx_start(db, dirs, 1, nullptr, true));
    embi_sys_stats s;
    embi_sys_stats_get(db, &s);
    EMB_CHECK(s.ready);
    EMB_CHECK_EQ_INT(s.entries, 0);
    EMB_CHECK_EQ_INT(sysfx_family_count(db, "Ember Sans"), 0);
    embi_fontdb_destroy(db);
}

EMB_TEST_MAIN()
