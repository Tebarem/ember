#include "emb_test.h"

#include "dom/dstr.h"

#define U8(lit) EMBI_STR(lit)

EMB_TEST(dstr_latin1_text_is_narrow)
{
    embi_slab *s = embi_slab_new();
    embi_dstr d = embi_dstr_from_utf8(s, U8("caf\xC3\xA9"));
    EMB_CHECK(!d.wide);
    EMB_CHECK_EQ_INT(d.len, 4);
    EMB_CHECK_EQ_INT(embi_dstr_unit(d, 3), 0xE9);
    embi_dstr_free(d);
    embi_slab_release(s);
}

EMB_TEST(dstr_unit_above_ff_is_wide)
{
    embi_slab *s = embi_slab_new();
    embi_dstr euro = embi_dstr_from_utf8(s, U8("\xE2\x82\xAC"));
    EMB_CHECK(euro.wide);
    EMB_CHECK_EQ_INT(euro.len, 1);
    EMB_CHECK_EQ_INT(embi_dstr_unit(euro, 0), 0x20AC);
    embi_dstr mixed = embi_dstr_from_utf8(s, U8("a\xE2\x82\xAC"));
    EMB_CHECK(mixed.wide);
    EMB_CHECK_EQ_INT(mixed.len, 2);
    embi_dstr_free(euro);
    embi_dstr_free(mixed);
    embi_slab_release(s);
}

EMB_TEST(dstr_supplementary_uses_surrogate_pair)
{
    embi_slab *s = embi_slab_new();
    embi_dstr d = embi_dstr_from_utf8(s, U8("\xF0\x9F\x98\x80"));
    EMB_CHECK(d.wide);
    EMB_CHECK_EQ_INT(d.len, 2);
    EMB_CHECK_EQ_INT(embi_dstr_unit(d, 0), 0xD83D);
    EMB_CHECK_EQ_INT(embi_dstr_unit(d, 1), 0xDE00);
    embi_dstr_free(d);
    embi_slab_release(s);
}

EMB_TEST(dstr_from_units_is_canonical)
{
    embi_slab *s = embi_slab_new();
    uint16_t low[2] = {0x41, 0xFF};
    uint16_t high[2] = {0x41, 0x100};
    embi_dstr a = embi_dstr_from_units(s, low, 2);
    embi_dstr b = embi_dstr_from_units(s, high, 2);
    EMB_CHECK(!a.wide);
    EMB_CHECK(b.wide);
    embi_dstr_free(a);
    embi_dstr_free(b);
    embi_dstr none = embi_dstr_from_units(s, low, 0);
    EMB_CHECK_EQ_INT(none.len, 0);
    EMB_CHECK(none.ptr == nullptr);
    embi_slab_release(s);
}

EMB_TEST(dstr_eq_cmp_and_hash)
{
    embi_slab *s = embi_slab_new();
    embi_dstr a = embi_dstr_from_utf8(s, U8("abc"));
    embi_dstr b = embi_dstr_from_utf8(s, U8("abc"));
    embi_dstr c = embi_dstr_from_utf8(s, U8("abd"));
    embi_dstr ab = embi_dstr_from_utf8(s, U8("ab"));
    EMB_CHECK(embi_dstr_eq(a, b));
    EMB_CHECK(!embi_dstr_eq(a, c));
    EMB_CHECK(!embi_dstr_eq(a, ab));
    EMB_CHECK(embi_dstr_cmp(a, c) < 0);
    EMB_CHECK(embi_dstr_cmp(ab, a) < 0);
    EMB_CHECK(embi_dstr_cmp(a, b) == 0);
    EMB_CHECK(embi_dstr_hash(a) == embi_dstr_hash(b));

    embi_dstr w1 = embi_dstr_from_utf8(s, U8("x\xE2\x82\xAC"));
    embi_dstr w2 = embi_dstr_from_utf8(s, U8("x\xE2\x82\xAC"));
    embi_dstr w3 = embi_dstr_from_utf8(s, U8("x\xE2\x82\xAD"));
    EMB_CHECK(embi_dstr_eq(w1, w2));
    EMB_CHECK(!embi_dstr_eq(w1, w3));
    EMB_CHECK(embi_dstr_hash(w1) == embi_dstr_hash(w2));
    EMB_CHECK(!embi_dstr_eq(a, w1));
    EMB_CHECK(embi_dstr_cmp(w1, w3) < 0);

    embi_dstr_free(a);
    embi_dstr_free(b);
    embi_dstr_free(c);
    embi_dstr_free(ab);
    embi_dstr_free(w1);
    embi_dstr_free(w2);
    embi_dstr_free(w3);
    embi_slab_release(s);
}

EMB_TEST(dstr_compare_with_utf8)
{
    embi_slab *s = embi_slab_new();
    embi_dstr d = embi_dstr_from_utf8(s, U8("H\xC3\xA9llo\xE2\x82\xAC"));
    EMB_CHECK(embi_dstr_eq_utf8(d, U8("H\xC3\xA9llo\xE2\x82\xAC")));
    EMB_CHECK(!embi_dstr_eq_utf8(d, U8("H\xC3\xA9llo")));
    EMB_CHECK(!embi_dstr_eq_utf8(d, U8("H\xC3\xA9llo\xE2\x82\xAD")));

    embi_dstr t = embi_dstr_from_utf8(s, U8("DIV"));
    EMB_CHECK(embi_dstr_ieq_utf8(t, U8("div")));
    EMB_CHECK(embi_dstr_ieq_utf8(t, U8("Div")));
    EMB_CHECK(!embi_dstr_ieq_utf8(t, U8("dim")));
    embi_dstr accent = embi_dstr_from_utf8(s, U8("\xC3\x89"));
    EMB_CHECK(!embi_dstr_ieq_utf8(accent, U8("\xC3\xA9")));

    embi_dstr emoji = embi_dstr_from_utf8(s, U8("\xF0\x9F\x98\x80"));
    EMB_CHECK(embi_dstr_eq_utf8(emoji, U8("\xF0\x9F\x98\x80")));

    embi_dstr_free(d);
    embi_dstr_free(t);
    embi_dstr_free(accent);
    embi_dstr_free(emoji);
    embi_slab_release(s);
}

EMB_TEST(dstr_to_utf8_round_trip_and_lone_surrogates)
{
    embi_slab *s = embi_slab_new();
    const char text[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    embi_str in = {text, (uint32_t)(sizeof text - 1)};
    embi_dstr d = embi_dstr_from_utf8(s, in);
    char buf[64];
    size_t n = embi_dstr_to_utf8(d, buf, sizeof buf);
    EMB_CHECK_EQ_INT(n, in.len);
    EMB_CHECK(SDL_memcmp(buf, text, n) == 0);
    /* A short buffer still reports the full length. */
    char tiny[2];
    EMB_CHECK_EQ_INT(embi_dstr_to_utf8(d, tiny, sizeof tiny), in.len);

    uint16_t lone[2] = {0xD800, 0x41};
    embi_dstr bad = embi_dstr_from_units(s, lone, 2);
    EMB_CHECK(bad.wide);
    n = embi_dstr_to_utf8(bad, buf, sizeof buf);
    EMB_CHECK_EQ_INT(n, 4);
    EMB_CHECK(SDL_memcmp(buf, "\xEF\xBF\xBD" "A", 4) == 0);

    embi_dstr_free(d);
    embi_dstr_free(bad);
    embi_slab_release(s);
}

EMB_TEST(dstr_copy_is_independent)
{
    embi_slab *s = embi_slab_new();
    embi_dstr original = embi_dstr_from_utf8(s, U8("copy me"));
    embi_dstr copy = embi_dstr_copy(s, original);
    embi_dstr_free(original);
    EMB_CHECK(embi_dstr_eq_utf8(copy, U8("copy me")));
    embi_dstr_free(copy);
    embi_slab_release(s);
}

EMB_TEST(dstr_atom_matches_intern)
{
    embi_slab *s = embi_slab_new();
    embi_dstr d = embi_dstr_from_utf8(s, U8("dstr-atom-probe"));
    embi_atom a = embi_dstr_atom(d);
    EMB_CHECK(a != 0);
    EMB_CHECK(a == embi_atom_intern("dstr-atom-probe", 15));
    embi_dstr_free(d);
    embi_slab_release(s);
}

EMB_TEST(dbuf_narrows_on_finish_and_resets)
{
    embi_slab *s = embi_slab_new();
    embi_dbuf b;
    embi_dbuf_init(&b, s);
    embi_dbuf_append_utf8(&b, U8("ab"));
    embi_dbuf_append_cp(&b, 0x1F600);
    EMB_CHECK_EQ_INT(embi_dbuf_len(&b), 4);
    embi_dstr d = embi_dbuf_finish(&b);
    EMB_CHECK(d.wide);
    EMB_CHECK_EQ_INT(d.len, 4);
    EMB_CHECK_EQ_INT(embi_dbuf_len(&b), 0);

    embi_dbuf_append_utf8(&b, U8("xyz"));
    embi_dstr narrow = embi_dbuf_finish(&b);
    EMB_CHECK(!narrow.wide);
    EMB_CHECK(embi_dstr_eq_utf8(narrow, U8("xyz")));

    embi_dstr empty = embi_dbuf_finish(&b);
    EMB_CHECK_EQ_INT(empty.len, 0);

    embi_dbuf_append(&b, d);
    embi_dbuf_append_unit(&b, 0x41);
    embi_dstr joined = embi_dbuf_finish(&b);
    EMB_CHECK(joined.wide);
    EMB_CHECK_EQ_INT(joined.len, 5);

    embi_dbuf_free(&b);
    embi_dstr_free(d);
    embi_dstr_free(narrow);
    embi_dstr_free(joined);
    embi_slab_release(s);
}

EMB_TEST_MAIN()
