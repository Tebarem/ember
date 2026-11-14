#include "text_fixture.h"

/* Long-text paths of the shaper (W3q): the break loop that replaced kbts_BreakEntireString, the
 * analysis-only mode, the 1024/4096-byte shaping cuts and the shape-config cache. */

static bool eq_breaks(const char *text, uint32_t len, uint8_t dir, uint8_t lb)
{
    uint32_t *f1 = SDL_calloc(len + 1, 4), *s1 = SDL_calloc(len + 1, 4);
    uint32_t *f2 = SDL_calloc(len + 1, 4), *s2 = SDL_calloc(len + 1, 4);
    embi_shape_breaks(text, len, dir, lb, f1, s1);
    embi_shape_breaks_reference(text, len, dir, lb, f2, s2);
    bool ok = true;
    for (uint32_t i = 0; i <= len && ok; i++) {
        if (f1[i] != f2[i] || s1[i] != s2[i]) {
            SDL_Log("break mismatch at %u: flags %x/%x script %u/%u", i, f1[i], f2[i], s1[i], s2[i]);
            ok = false;
        }
    }
    SDL_free(f1);
    SDL_free(s1);
    SDL_free(f2);
    SDL_free(s2);
    return ok;
}

EMB_TEST(break_loop_matches_break_entire_string)
{
    static const char *const samples[] = {
        "",
        "hello world, this is a line.\nnext paragraph\r\nand more",
        /* Latin, Arabic, Devanagari, Han, Thai and emoji mixed. */
        "abc \xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9 "
        "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 "
        "\xe6\xbc\xa2\xe5\xad\x97\xe4\xbb\xae\xe5\x90\x8d\xe3\x81\x8b\xe3\x81\xaa "
        "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5 "
        "\xf0\x9f\x98\x80\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 e\xcc\x81x",
        /* Invalid UTF-8: stray continuation, truncated sequence, overlong, surrogate, 0xff. */
        "a\x80z \xe2\x82 \xc0\xaf \xed\xa0\x80 \xff\xfe end \xf0\x9f",
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d world \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d 123 .,;",
    };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        uint32_t len = (uint32_t)SDL_strlen(samples[i]);
        for (uint8_t dir = EMBI_DIR_LTR; dir <= EMBI_DIR_AUTO; dir++) {
            for (uint8_t lb = EMBI_LINE_BREAK_AUTO; lb <= EMBI_LINE_BREAK_STRICT; lb++) {
                EMB_CHECK(eq_breaks(samples[i], len, dir, lb));
            }
        }
    }
}

EMB_TEST(break_loop_matches_on_long_mixed_text)
{
    /* About 3 KB of repeated mixed-script text (the reference is quadratic: keep it small). */
    const char *unit = "word \xd8\xa7\xd9\x84\xd8\xb9 \xe6\xbc\xa2\xe5\xad\x97 x\xcc\x81y \xf0\x9f\x98\x80\n";
    char buf[3200];
    uint32_t n = 0, ul = (uint32_t)SDL_strlen(unit);
    while (n + ul < sizeof buf) {
        SDL_memcpy(buf + n, unit, ul);
        n += ul;
    }
    EMB_CHECK(eq_breaks(buf, n, EMBI_DIR_AUTO, EMBI_LINE_BREAK_AUTO));
}

static embi_face *reg_face(embi_text *t, const char *family, const char *file)
{
    embi_face_desc d = {0};
    d.family = tx_s(family);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, file, &d) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

/* The level of every byte, from the runs (logical order, covering the text). */
static void byte_levels(const embi_text_layout *lay, uint8_t *out)
{
    SDL_memset(out, 0xff, lay->text_len + 1);
    for (uint32_t i = 0; i < lay->runs.len; i++) {
        const embi_text_run *r = &lay->runs.data[i];
        for (uint32_t b = r->text_begin; b < r->text_end; b++) {
            out[b] = r->level;
        }
    }
}

static bool same_breaks(const embi_text_layout *a, const embi_text_layout *b)
{
    if (a->breaks.len != b->breaks.len) {
        return false;
    }
    for (uint32_t i = 0; i < a->breaks.len; i++) {
        if (a->breaks.data[i].offset != b->breaks.data[i].offset ||
            a->breaks.data[i].flags != b->breaks.data[i].flags) {
            return false;
        }
    }
    return true;
}

/* About 12 KB: 1500 words of 7 letters (with a space, or none), and an Arabic stretch in the middle. */
static char *make_long(uint32_t *len_out, bool unbreakable)
{
    size_t cap = 24000;
    char *s = SDL_calloc(1, cap);
    size_t n = 0;
    for (int i = 0; i < 1500; i++) {
        n += (size_t)SDL_snprintf(s + n, cap - n, unbreakable ? "abcdefg" : "abcdefg ");
        if (i == 700) {
            n += (size_t)SDL_snprintf(s + n, cap - n,
                                      " \xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9 ");
        }
    }
    *len_out = (uint32_t)n;
    return s;
}

EMB_TEST(cut_runs_keep_breaks_levels_and_advance)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg_face(t, "Noto Test", "tests/fonts/NotoSans-Regular.ttf") != nullptr);
    embi_face *ar = reg_face(t, "Arabic Test", "tests/fonts/NotoSansArabic-Regular.ttf");
    EMB_REQUIRE(ar != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Arabic Test")};
    embi_text_style st;
    tx_style(&st, fams, 2, 1000.0f);
    st.kerning = EMBI_KERN_NONE;
    st.ligatures = false;

    embi_text_layout w;
    EMB_REQUIRE(tx_shape(t, "abcdefg", &st, &w));
    float word = w.advance;
    embi_text_layout_free(&w);
    embi_text_layout sp;
    EMB_REQUIRE(tx_shape(t, " ", &st, &sp));
    float space = sp.advance;
    embi_text_layout_free(&sp);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t len;
        char *text = make_long(&len, pass == 1);
        EMB_REQUIRE(len > 4096 * 2);
        embi_text_layout full, ana;
        st.analysis_only = false;
        EMB_REQUIRE(embi_text_shape(t, text, len, &st, &full));
        st.analysis_only = true;
        EMB_REQUIRE(embi_text_shape(t, text, len, &st, &ana));
        st.analysis_only = false;

        /* Analysis only: no glyphs, no faces; same breaks, paragraphs and levels. */
        EMB_CHECK_EQ_INT(ana.glyphs.len, 0);
        for (uint32_t i = 0; i < ana.runs.len; i++) {
            EMB_CHECK(ana.runs.data[i].face == nullptr);
            EMB_CHECK_EQ_INT(ana.runs.data[i].glyph_begin, ana.runs.data[i].glyph_end);
        }
        EMB_CHECK(same_breaks(&full, &ana));
        EMB_CHECK_EQ_INT(full.paras.len, ana.paras.len);
        uint8_t *l1 = SDL_malloc(len + 1), *l2 = SDL_malloc(len + 1);
        byte_levels(&full, l1);
        byte_levels(&ana, l2);
        EMB_CHECK(SDL_memcmp(l1, l2, len) == 0);
        SDL_free(l1);
        SDL_free(l2);

        /* The cut result: runs tile the text, clusters are in range, and the Latin advance is the
         * uncut one (no kerning, so it is the sum of the word and space advances) within 1%. */
        EMB_CHECK(full.glyphs.len > 0);
        uint32_t pos = 0;
        for (uint32_t i = 0; i < full.runs.len; i++) {
            EMB_CHECK_EQ_INT(full.runs.data[i].text_begin, pos);
            EMB_CHECK(full.runs.data[i].text_end > pos);
            pos = full.runs.data[i].text_end;
        }
        EMB_CHECK_EQ_INT(pos, len);
        for (uint32_t i = 0; i < full.glyphs.len; i++) {
            EMB_CHECK(full.glyphs.data[i].cluster < len);
        }
        float latin = 0.0f;
        for (uint32_t i = 0; i < full.runs.len; i++) {
            if (full.runs.data[i].face != ar) {
                latin += full.runs.data[i].advance;
            }
        }
        float want = 1500.0f * word + (pass == 0 ? 1500.0f * space : 0.0f);
        EMB_CHECK(latin >= want - 0.01f * want);
        EMB_CHECK(latin <= want + 2.0f * space + 0.01f * want);
        embi_text_layout_free(&full);
        embi_text_layout_free(&ana);
        SDL_free(text);
    }
    embi_text_destroy(t);
}

EMB_TEST(analysis_only_has_no_glyphs_and_needs_no_font)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_str fam = tx_s("No Such Family");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.analysis_only = true;
    embi_text_layout lay;
    const char *s = "one two\nthree \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d four";
    EMB_REQUIRE(tx_shape(t, s, &st, &lay));
    EMB_CHECK_EQ_INT(lay.glyphs.len, 0);
    EMB_CHECK(lay.breaks.len > 0);
    EMB_CHECK_EQ_INT(lay.paras.len, 2);
    bool rtl = false;
    for (uint32_t i = 0; i < lay.runs.len; i++) {
        EMB_CHECK(lay.runs.data[i].face == nullptr);
        rtl = rtl || (lay.runs.data[i].level & 1) != 0;
    }
    EMB_CHECK(rtl);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(shape_config_cache_survives_many_combinations)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg_face(t, "Test VF", "tests/fonts/NotoSans-VF.ttf") != nullptr);
    embi_str fam = tx_s("Test VF");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    st.kerning = EMBI_KERN_NONE;

    enum { N = 300 };
    static float res[3][N];
    static char lang[N][8];
    /* 300 (instance, language) combinations: more than the 128 kept configs. */
    for (int round = 0; round < 3; round++) {
        if (round == 2) {
            embi_text_clear_caches(t);
        }
        for (int k = 0; k < N; k++) {
            int i = round == 1 ? N - 1 - k : k;
            st.weight = 100.0f + (float)(i % 100) * 8.0f;
            SDL_snprintf(lang[i], sizeof lang[i], "%c%c", 'a' + (i / 26) % 26, 'a' + i % 26);
            st.lang = tx_s(lang[i]);
            embi_text_layout lay;
            EMB_REQUIRE(tx_shape(t, "Hamburgefonts AVA", &st, &lay));
            res[round][i] = lay.advance;
            embi_text_layout_free(&lay);
        }
    }
    for (int i = 0; i < N; i++) {
        EMB_CHECK(res[0][i] == res[1][i]);
        EMB_CHECK(res[0][i] == res[2][i]);
    }
    /* The weight axis still changes the result: the configs are not mixed up. */
    EMB_CHECK(res[0][99] > res[0][0]);
    EMB_CHECK(res[0][100] == res[0][0]);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
