#include "emb_test.h"

#include "base/base.h"

EMB_TEST(atom_same_bytes_same_id)
{
    embi_atom a = embi_atom_intern("div", 3);
    embi_atom b = embi_atom_intern("div", 3);
    embi_atom c = embi_atom_intern("span", 4);
    EMB_CHECK(a != 0);
    EMB_CHECK(a == b);
    EMB_CHECK(a != c);
    embi_str s = embi_atom_str(a);
    EMB_CHECK(embi_str_eq(s, EMBI_STR("div")));
    EMB_CHECK(embi_str_eq(embi_atom_str(c), EMBI_STR("span")));
    embi_atom_shutdown();
}

EMB_TEST(atom_prefix_and_empty_are_distinct)
{
    embi_atom ab = embi_atom_intern("ab", 2);
    embi_atom a = embi_atom_intern("a", 1);
    embi_atom empty = embi_atom_intern("", 0);
    embi_atom empty2 = embi_atom_intern("xyz", 0);
    EMB_CHECK(ab != a);
    EMB_CHECK(a != empty);
    EMB_CHECK(empty == empty2);
    EMB_CHECK_EQ_INT(embi_atom_str(empty).len, 0);
    /* A view into the source buffer must not be what the atom returns. */
    char buf[8] = "abcdef";
    embi_atom sub = embi_atom_intern(buf, 3);
    buf[0] = 'Z';
    EMB_CHECK(embi_str_eq(embi_atom_str(sub), EMBI_STR("abc")));
    embi_atom_shutdown();
}

EMB_TEST(atom_id_zero_is_invalid_str)
{
    EMB_CHECK_EQ_INT(embi_atom_str(0).len, 0);
    EMB_CHECK_EQ_INT(embi_atom_str(123456).len, 0);
    embi_atom_shutdown();
}

EMB_TEST(atom_many_strings_stay_distinct_and_stable)
{
    enum { N = 20000 };
    static embi_atom ids[N];
    char text[32];
    for (int i = 0; i < N; i++) {
        int n = SDL_snprintf(text, sizeof text, "name-%d", i);
        ids[i] = embi_atom_intern(text, (uint32_t)n);
    }
    int bad = 0;
    for (int i = 0; i < N; i++) {
        int n = SDL_snprintf(text, sizeof text, "name-%d", i);
        if (embi_atom_intern(text, (uint32_t)n) != ids[i]) {
            bad++;
        }
        if (!embi_str_eq(embi_atom_str(ids[i]), (embi_str){text, (uint32_t)n})) {
            bad++;
        }
    }
    EMB_CHECK_EQ_INT(bad, 0);
    /* Distinctness: the first and last ids differ, and ids are never zero. */
    EMB_CHECK(ids[0] != ids[N - 1]);
    EMB_CHECK(ids[N / 2] != 0);
    embi_atom_shutdown();
}

EMB_TEST(atom_shutdown_returns_memory_and_rebuilds)
{
    size_t base = embi_mem_live();
    embi_atom first = embi_atom_intern("again", 5);
    EMB_CHECK(first != 0);
    EMB_CHECK(embi_mem_live() > base);
    embi_atom_shutdown();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
    /* A new table works after shutdown. */
    embi_atom second = embi_atom_intern("again", 5);
    EMB_CHECK(second != 0);
    EMB_CHECK(embi_str_eq(embi_atom_str(second), EMBI_STR("again")));
    embi_atom_shutdown();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST_MAIN()
