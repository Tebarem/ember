#include "text_fixture.h"

/* Line API over NotoSans-Regular ("Noto Test", upem 1000) at 1000 px, so advances equal font
 * units. Break kinds come from kb's UAX #14 and UAX #29 segmentation. */

static embi_face *reg(embi_text *t)
{
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

static bool has_break(const embi_text_layout *lay, uint32_t offset, uint8_t kind)
{
    for (uint32_t i = 0; i < lay->breaks.len; i++) {
        if (lay->breaks.data[i].offset == offset && (lay->breaks.data[i].flags & kind) != 0) {
            return true;
        }
    }
    return false;
}

EMB_TEST(line_break_opportunities_after_spaces)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "hello world", &st, &lay));
    EMB_CHECK(has_break(&lay, 6, EMBI_BREAK_LINE));
    EMB_CHECK(!has_break(&lay, 3, EMBI_BREAK_LINE));
    EMB_CHECK(has_break(&lay, 11, EMBI_BREAK_LINE));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(grapheme_boundaries_keep_marks_with_their_base)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    /* e + combining acute (2 bytes) + x. */
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "e\xCC\x81x", &st, &lay));
    EMB_CHECK(has_break(&lay, 0, EMBI_BREAK_GRAPHEME));
    EMB_CHECK(has_break(&lay, 3, EMBI_BREAK_GRAPHEME));
    EMB_CHECK(!has_break(&lay, 1, EMBI_BREAK_GRAPHEME));
    EMB_CHECK(!has_break(&lay, 2, EMBI_BREAK_GRAPHEME));
    /* Caret stops: the mark's glyph (cluster 1) is not a stop. */
    for (uint32_t g = 0; g < lay.glyphs.len; g++) {
        if (lay.glyphs.data[g].cluster == 1) {
            EMB_CHECK(!lay.glyphs.data[g].safe_to_break);
        }
    }
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(measure_and_lines_add_up)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "hello world", &st, &lay));
    float total = embi_text_measure(&lay, 0, 11);
    EMB_CHECK(total == lay.advance);
    float a = embi_text_measure(&lay, 0, 6);
    float b = embi_text_measure(&lay, 6, 11);
    EMB_CHECK(a + b == total);
    embi_text_vruns l1 = {0}, l2 = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 6, &l1));
    EMB_REQUIRE(embi_text_line(&lay, 6, 11, &l2));
    EMB_CHECK(l1.width == a);
    EMB_CHECK(l2.width == b);
    /* The second line starts at x = 0, and its first glyph is the one at cluster 6. */
    EMB_REQUIRE(l2.items.len > 0);
    EMB_CHECK(l2.items.data[0].x == 0.0f);
    EMB_CHECK_EQ_INT(lay.glyphs.data[l1.items.data[0].glyph_begin].cluster, 0);
    EMB_CHECK_EQ_INT(lay.glyphs.data[l2.items.data[0].glyph_begin].cluster, 6);
    /* An empty range is an empty line. */
    embi_text_vruns none = {0};
    EMB_REQUIRE(embi_text_line(&lay, 4, 4, &none));
    EMB_CHECK_EQ_INT(none.items.len, 0);
    EMB_CHECK(none.width == 0.0f);
    embi_text_vruns_free(&l1);
    embi_text_vruns_free(&l2);
    embi_text_vruns_free(&none);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* Break modes (CSS Text 3 sections 5.2 to 5.5) through embi_text_break_before. */

static uint8_t brk(const embi_text_layout *lay, uint32_t at, uint8_t wb, uint8_t ow, uint8_t lb)
{
    return embi_text_break_before(lay, at, wb, ow, lb);
}

EMB_TEST(break_all_offers_grapheme_breaks_and_keeps_line_breaks)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout abc;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &abc));
    EMB_CHECK((brk(&abc, 1, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_GRAPHEME) == 0);
    EMB_CHECK((brk(&abc, 1, EMBI_WORD_BREAK_BREAK_ALL, 0, 0) & EMBI_BREAK_GRAPHEME) != 0);
    /* line-break: anywhere makes the grapheme boundaries soft too, as break-all does. */
    EMB_CHECK((brk(&abc, 2, EMBI_WORD_BREAK_NORMAL, 0, EMBI_LINE_BREAK_ANYWHERE) &
               EMBI_BREAK_GRAPHEME) != 0);
    EMB_CHECK((brk(&abc, 2, EMBI_WORD_BREAK_NORMAL, 0, EMBI_LINE_BREAK_NORMAL) &
               EMBI_BREAK_GRAPHEME) == 0);
    embi_text_layout_free(&abc);

    embi_text_layout ab;
    EMB_REQUIRE(tx_shape(t, "a b", &st, &ab));
    /* The space is still a line opportunity under break-all. */
    EMB_CHECK((brk(&ab, 2, EMBI_WORD_BREAK_BREAK_ALL, 0, 0) & EMBI_BREAK_LINE) != 0);
    embi_text_layout_free(&ab);
    embi_text_destroy(t);
}

EMB_TEST(keep_all_suppresses_hangul_and_han_breaks)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);

    /* Two Hangul syllables (U+AC00 U+B098, 3 bytes each): UAX #14 allows a break at byte 3. */
    embi_text_layout ko;
    EMB_REQUIRE(tx_shape(t, "\xEA\xB0\x80\xEB\x82\x98", &st, &ko));
    EMB_CHECK((brk(&ko, 3, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_LINE) != 0);
    EMB_CHECK((brk(&ko, 3, EMBI_WORD_BREAK_KEEP_ALL, 0, 0) & EMBI_BREAK_LINE) == 0);
    /* Leading consonant, vowel, leading consonant (jamo): letter-like under keep-all too. */
    embi_text_layout jamo;
    EMB_REQUIRE(tx_shape(t, "\xE1\x84\x80\xE1\x85\xA1\xE1\x84\x80", &st, &jamo));
    EMB_CHECK((brk(&jamo, 6, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_LINE) != 0);
    EMB_CHECK((brk(&jamo, 6, EMBI_WORD_BREAK_KEEP_ALL, 0, 0) & EMBI_BREAK_LINE) == 0);
    embi_text_layout_free(&jamo);
    embi_text_layout_free(&ko);

    /* Two Han ideographs (U+4E2D U+6587, 3 bytes each). */
    embi_text_layout zh;
    EMB_REQUIRE(tx_shape(t, "\xE4\xB8\xAD\xE6\x96\x87", &st, &zh));
    EMB_CHECK((brk(&zh, 3, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_LINE) != 0);
    EMB_CHECK((brk(&zh, 3, EMBI_WORD_BREAK_KEEP_ALL, 0, 0) & EMBI_BREAK_LINE) == 0);
    embi_text_layout_free(&zh);

    /* A space is not letter-like: the break after it stays under keep-all. */
    embi_text_layout sp;
    EMB_REQUIRE(tx_shape(t, "a \xEA\xB0\x80", &st, &sp));
    EMB_CHECK((brk(&sp, 2, EMBI_WORD_BREAK_KEEP_ALL, 0, 0) & EMBI_BREAK_LINE) != 0);
    embi_text_layout_free(&sp);
    embi_text_destroy(t);
}

EMB_TEST(hard_breaks_survive_every_mode)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "a\nb", &st, &lay));
    EMB_CHECK((brk(&lay, 2, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_HARD) != 0);
    EMB_CHECK((brk(&lay, 2, EMBI_WORD_BREAK_KEEP_ALL, EMBI_OVERFLOW_WRAP_ANYWHERE,
                   EMBI_LINE_BREAK_ANYWHERE) & EMBI_BREAK_HARD) != 0);
    EMB_CHECK((brk(&lay, 1, EMBI_WORD_BREAK_NORMAL, 0, 0) & EMBI_BREAK_HARD) == 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(overflow_wrap_offers_emergency_breaks_only_when_asked)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &lay));
    EMB_CHECK((brk(&lay, 1, EMBI_WORD_BREAK_NORMAL, EMBI_OVERFLOW_WRAP_NORMAL, 0) &
               EMBI_BREAK_WORD) == 0);
    EMB_CHECK((brk(&lay, 1, EMBI_WORD_BREAK_NORMAL, EMBI_OVERFLOW_WRAP_BREAK_WORD, 0) &
               EMBI_BREAK_WORD) != 0);
    EMB_CHECK((brk(&lay, 2, EMBI_WORD_BREAK_NORMAL, EMBI_OVERFLOW_WRAP_ANYWHERE, 0) &
               EMBI_BREAK_WORD) != 0);
    embi_text_layout_free(&lay);
    /* Byte 2 is inside the cluster "e" + combining acute: not a break position at all. */
    embi_text_layout mark;
    EMB_REQUIRE(tx_shape(t, "e\xCC\x81x", &st, &mark));
    EMB_CHECK_EQ_INT(brk(&mark, 2, EMBI_WORD_BREAK_BREAK_ALL, EMBI_OVERFLOW_WRAP_ANYWHERE,
                         EMBI_LINE_BREAK_ANYWHERE), 0);
    embi_text_layout_free(&mark);
    embi_text_destroy(t);
}

/* The break lookup is a binary search: every recorded break answers with its own flags, and
 * every other byte answers zero. */
EMB_TEST(break_lookup_matches_the_break_table)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "The quick, brown fox\n jumps e\xCC\x81 over.\n\nEnd", &st, &lay));
    uint32_t next = 0;
    for (uint32_t at = 0; at <= lay.text_len; at++) {
        uint8_t got = brk(&lay, at, EMBI_WORD_BREAK_BREAK_ALL, EMBI_OVERFLOW_WRAP_BREAK_WORD, 0);
        bool listed = next < lay.breaks.len && lay.breaks.data[next].offset == at;
        if (listed) {
            uint8_t fl = lay.breaks.data[next].flags;
            EMB_CHECK((got & EMBI_BREAK_HARD) == (fl & EMBI_BREAK_HARD ? EMBI_BREAK_HARD : 0));
            EMB_CHECK((got & EMBI_BREAK_LINE) == (fl & EMBI_BREAK_LINE ? EMBI_BREAK_LINE : 0));
            next++;
        } else {
            EMB_CHECK_EQ_INT(got, 0);
        }
    }
    EMB_CHECK_EQ_INT(next, lay.breaks.len);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* Greedy line filling over a long paragraph: every line is a range between two breaks, every
 * glyph lands on exactly one line, and the line widths add up to the paragraph's advance. */
EMB_TEST(greedy_lines_cover_every_glyph_once)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    char text[4096];
    uint32_t n = 0;
    for (int i = 0; i < 60; i++) {
        n += (uint32_t)SDL_snprintf(text + n, sizeof text - n, "lorem ipsum dolor, sit amet %d. ",
                                    i);
    }
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, text, &st, &lay));
    const float width = 220.0f;
    uint32_t start = 0, covered = 0;
    float total = 0.0f;
    uint32_t lines = 0;
    while (start < lay.text_len) {
        uint32_t best = 0;
        uint32_t first_after = 0;
        for (uint32_t i = 0; i < lay.breaks.len; i++) {
            uint32_t b = lay.breaks.data[i].offset;
            if (b <= start || (lay.breaks.data[i].flags & EMBI_BREAK_LINE) == 0) {
                continue;
            }
            if (first_after == 0) {
                first_after = b;
            }
            if (embi_text_measure(&lay, start, b) <= width) {
                best = b;
            }
        }
        if (best == 0) {
            best = first_after;
        }
        EMB_REQUIRE(best > start);
        embi_text_vruns vr = {0};
        EMB_REQUIRE(embi_text_line(&lay, start, best, &vr));
        uint32_t glyphs = 0;
        for (uint32_t k = 0; k < vr.items.len; k++) {
            glyphs += vr.items.data[k].glyph_end - vr.items.data[k].glyph_begin;
        }
        covered += glyphs;
        total += vr.width;
        EMB_CHECK(vr.width == embi_text_measure(&lay, start, best));
        embi_text_vruns_free(&vr);
        start = best;
        lines++;
    }
    EMB_CHECK(lines > 5);
    EMB_CHECK_EQ_INT(covered, lay.glyphs.len);
    /* Lines add their advances in a different order than the runs do: float rounding only. */
    float drift = total - lay.advance;
    float tol = 0.01f + lay.advance * 1e-5f;
    EMB_CHECK(drift < tol && drift > -tol);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* A range may end at its paragraph's separator; the separator draws nothing. */
EMB_TEST(line_may_end_at_the_separator)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t) != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "a\nb", &st, &lay));
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 2, &vr));
    EMB_CHECK_EQ_INT(vr.items.len, 1);
    EMB_CHECK(vr.width == embi_text_measure(&lay, 0, 1));
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
