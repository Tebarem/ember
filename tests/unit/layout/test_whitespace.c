/*
 * White space processing and text-transform (W3a section 1, CSS Text 3 sections 2.1 and 4).
 * Each case runs embi_lay_ws_process on one string with a given mode, transform and starting
 * collapse state, and compares the output and the final state.
 */
#include "emb_test.h"

#include "layout/layout_int.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* Processes in with the given modes and checks the output and the trailing space state. */
static bool ws_is(uint8_t mode, uint16_t transform, bool prev_in, const char *in, const char *want,
                  bool want_prev)
{
    embi_sb sb = {nullptr, 0, 0};
    bool prev = prev_in;
    bool word = false;
    embi_lay_ws_process(&sb, (embi_str){in, (uint32_t)strlen(in)}, mode, transform, &prev, &word);
    bool ok = sb.len == strlen(want) && (sb.len == 0 || memcmp(sb.data, want, sb.len) == 0) &&
              prev == want_prev;
    embi_sb_free(&sb);
    return ok;
}

EMB_TEST(ws_collapse_drops_leading_space_at_line_start)
{
    /* At the start of a line (prev_space true), leading spaces vanish; runs collapse to one. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, true, "  a   b  ", "a b ", true));
}

EMB_TEST(ws_collapse_keeps_one_leading_space_mid_line)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "  x", " x", false));
}

EMB_TEST(ws_collapse_turns_newlines_and_tabs_into_spaces)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\n\tb", "a b", false));
}

EMB_TEST(ws_pre_keeps_every_space)
{
    EMB_CHECK(ws_is(EMBI_WS_PRESERVE, EMBI_KW_none, false, "a  b", "a  b", false));
}

EMB_TEST(ws_pre_line_collapses_spaces_but_keeps_breaks)
{
    /* A preserved break stays a line feed: inline layout (W3c) turns it into a forced line break. */
    EMB_CHECK(ws_is(EMBI_WS_PRESERVE_BREAKS, EMBI_KW_none, false, "a  b\nc", "a b\nc", false));
}

EMB_TEST(ws_text_transform_uppercase_and_lowercase)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_uppercase, false, "abc xyz", "ABC XYZ", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_lowercase, false, "ABC Xyz", "abc xyz", false));
}

EMB_TEST(ws_text_transform_capitalize_starts_each_word)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_capitalize, false, "hello big  world",
                    "Hello Big World", false));
}

EMB_TEST(ws_discard_removes_every_collapsible_character)
{
    EMB_CHECK(ws_is(EMBI_WS_DISCARD, EMBI_KW_none, false, " a \n\t b ", "ab", false));
    EMB_CHECK(ws_is(EMBI_WS_DISCARD, EMBI_KW_none, true, "\n\n", "", true));
}

EMB_TEST(ws_segment_break_between_east_asian_wide_characters_is_removed)
{
    /* CSS Text 3 section 4.1.2: the break goes between two wide characters, even with spaces around it. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xE6\x97\xA5\n\xE6\x9C\xAC",
                    "\xE6\x97\xA5\xE6\x9C\xAC", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xE6\x97\xA5 \n  \xE6\x9C\xAC",
                    "\xE6\x97\xA5\xE6\x9C\xAC", false));
    /* Only one side wide: a space. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xE6\x97\xA5\nb", "\xE6\x97\xA5 b", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\n\xE6\x97\xA5", "a \xE6\x97\xA5", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\nb", "a b", false));
    /* Fullwidth and halfwidth forms count as wide. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xEF\xBC\xA1\n\xEF\xBC\xA2",
                    "\xEF\xBC\xA1\xEF\xBC\xA2", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xEF\xBD\xB1\n\xEF\xBD\xB2",
                    "\xEF\xBD\xB1\xEF\xBD\xB2", false));
    /* Hangul on either side keeps the break as a space. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xEF\xBC\xA1\n\xEA\xB0\x80",
                    "\xEF\xBC\xA1 \xEA\xB0\x80", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xEA\xB0\x80\n\xE6\x97\xA5",
                    "\xEA\xB0\x80 \xE6\x97\xA5", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xEA\xB0\x80\n\xEA\xB0\x80",
                    "\xEA\xB0\x80 \xEA\xB0\x80", false));
}

EMB_TEST(ws_spaces_around_a_segment_break_collapse_to_one_space)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a  \n  b", "a b", false));
    EMB_CHECK(ws_is(EMBI_WS_PRESERVE_BREAKS, EMBI_KW_none, false, "a  \n  b", "a\nb", false));
}

EMB_TEST(ws_text_transform_maps_non_ascii_letters)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_uppercase, false, "\xC3\xA9t\xC3\xA9",
                    "\xC3\x89T\xC3\x89", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_lowercase, false, "\xC3\x89T\xC3\x89",
                    "\xC3\xA9t\xC3\xA9", false));
    /* The sharp s uppercases to two letters (the special casing table). */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_uppercase, false, "stra\xC3\x9F" "e", "STRASSE", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_uppercase, false, "\xCE\xB1\xCE\xB2", "\xCE\x91\xCE\x92",
                    false));
}

EMB_TEST(ws_text_transform_capitalize_handles_accents_and_punctuation)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_capitalize, false, "\xC3\xA9lan vital",
                    "\xC3\x89lan Vital", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_capitalize, false, "don't stop", "Don't Stop", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_capitalize, false, "(abc) \"def\"", "(Abc) \"Def\"",
                    false));
}

EMB_TEST(ws_text_transform_full_width_maps_ascii_to_fullwidth_forms)
{
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_full_width, false, "Ab!",
                    "\xEF\xBC\xA1\xEF\xBD\x82\xEF\xBC\x81", false));
    /* Letters outside ASCII are left alone. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_full_width, false, "\xC3\xA9", "\xC3\xA9", false));
    /* The space maps to the ideographic space. */
    uint32_t m[3];
    EMB_CHECK_EQ_INT(embi_lay_map_cp(0x20, EMBI_KW_full_width, m), 1);
    EMB_CHECK_EQ_INT(m[0], 0x3000);
}

EMB_TEST(ws_full_width_maps_symbols_and_halfwidth_katakana)
{
    uint32_t m[3];
    static const uint32_t from[] = {0xA2, 0xA3, 0xA5, 0xA6, 0xAC, 0xAF, 0x20A9, 0xFF71, 0xFF76, 0xFF9D, 0xFF65};
    static const uint32_t to[] = {0xFFE0, 0xFFE1, 0xFFE5, 0xFFE4, 0xFFE2, 0xFFE3, 0xFFE6, 0x30A2, 0x30AB, 0x30F3, 0x30FB};
    for (size_t i = 0; i < sizeof from / sizeof from[0]; i++) {
        EMB_CHECK_EQ_INT(embi_lay_map_cp(from[i], EMBI_KW_full_width, m), 1);
        EMB_CHECK_EQ_INT(m[0], to[i]);
    }
}

EMB_TEST(ws_text_transform_full_size_kana_enlarges_small_kana)
{
    /* Small a, ya, katakana small a become their full-size forms; an ASCII letter is unchanged. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_full_size_kana, false,
                    "\xE3\x81\x81\xE3\x82\x83\xE3\x82\xA1\xE3\x81\x82x",
                    "\xE3\x81\x82\xE3\x82\x84\xE3\x82\xA2\xE3\x81\x82x", false));
    /* The small ka and ke (U+3095, U+3096, U+30F5, U+30F6) map to U+304B, U+3051, U+30AB, U+30B1. */
    uint32_t m[3];
    EMB_CHECK_EQ_INT(embi_lay_map_cp(0x3095, EMBI_KW_full_size_kana, m), 1);
    EMB_CHECK_EQ_INT(m[0], 0x304B);
    embi_lay_map_cp(0x3096, EMBI_KW_full_size_kana, m);
    EMB_CHECK_EQ_INT(m[0], 0x3051);
    embi_lay_map_cp(0x30F5, EMBI_KW_full_size_kana, m);
    EMB_CHECK_EQ_INT(m[0], 0x30AB);
    embi_lay_map_cp(0x30F6, EMBI_KW_full_size_kana, m);
    EMB_CHECK_EQ_INT(m[0], 0x30B1);
}

EMB_TEST(ws_text_transform_applies_in_preserving_modes_and_none_is_identity)
{
    EMB_CHECK(ws_is(EMBI_WS_PRESERVE, EMBI_KW_uppercase, false, "a  b", "A  B", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "\xC3\xA9" "Ab", "\xC3\xA9" "Ab", false));
}

EMB_TEST(ws_long_space_runs_are_linear)
{
    /* 200k spaces between two letters: a quadratic scan would take seconds. */
    size_t n = 200000;
    char *in = SDL_malloc(n + 3);
    in[0] = 'a';
    memset(in + 1, ' ', n);
    in[n + 1] = 'b';
    in[n + 2] = '\0';
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, in, "a b", false));
    SDL_free(in);
}

EMB_TEST(ws_segment_break_next_to_a_zero_width_space_is_removed)
{
    /* CSS Text 3 section 4.1.2: a break next to U+200B is removed, not turned into a space. */
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\xE2\x80\x8B\nb", "a\xE2\x80\x8B" "b", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\n\xE2\x80\x8B" "b", "a\xE2\x80\x8B" "b", false));
    EMB_CHECK(ws_is(EMBI_WS_COLLAPSE, EMBI_KW_none, false, "a\nb", "a b", false));
}

EMB_TEST(ws_capitalize_word_state_carries_across_text_nodes)
{
    /* "hel" then "lo wor": the second node continues the word, so only "hel" and "wor" start one. */
    embi_sb a = {nullptr, 0, 0}, b = {nullptr, 0, 0};
    bool prev = true, word = false;
    embi_lay_ws_process(&a, EMBI_STR("hel"), EMBI_WS_COLLAPSE, EMBI_KW_capitalize, &prev, &word);
    EMB_CHECK(word);
    embi_lay_ws_process(&b, EMBI_STR("lo wor"), EMBI_WS_COLLAPSE, EMBI_KW_capitalize, &prev, &word);
    EMB_CHECK(a.len == 3 && memcmp(a.data, "Hel", 3) == 0);
    EMB_CHECK(b.len == 6 && memcmp(b.data, "lo Wor", 6) == 0);
    embi_sb_free(&a);
    embi_sb_free(&b);
    /* A space ends the word state. */
    embi_sb c = {nullptr, 0, 0};
    embi_lay_ws_process(&c, EMBI_STR("ab "), EMBI_WS_COLLAPSE, EMBI_KW_capitalize, &prev, &word);
    EMB_CHECK(!word);
    embi_sb_free(&c);
}

EMB_TEST_MAIN()
