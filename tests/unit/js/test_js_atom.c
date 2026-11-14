#include "emb_test.h"

#include "js/js.h"

EMB_TEST(atom_same_content_yields_same_pointer)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *a = embi_js_atom_intern_utf8(rt, "length", 6);
    embi_js_string *b = embi_js_atom_intern_utf8(rt, "length", 6);
    embi_js_string *c = embi_js_atom_intern_utf8(rt, "lengths", 7);
    EMB_CHECK(a == b);
    EMB_CHECK(a != c);
    EMB_CHECK(embi_js_str_is_atom(a));
    EMB_CHECK(!embi_js_str_is_atom(embi_js_str_new_utf8(rt, "length", 6)));
    embi_js_runtime_free(rt);
}

EMB_TEST(atom_interning_a_fresh_string_adopts_it)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *fresh = embi_js_str_new_utf8(rt, "name", 4);
    EMB_CHECK(!embi_js_str_is_atom(fresh));
    embi_js_string *atom = embi_js_atom_intern(rt, fresh);
    EMB_CHECK(atom == fresh);
    EMB_CHECK(embi_js_str_is_atom(fresh));
    /* A second copy with equal contents resolves to the first. */
    embi_js_string *again = embi_js_str_new_utf8(rt, "name", 4);
    EMB_CHECK(embi_js_atom_intern(rt, again) == fresh);
    EMB_CHECK(again != fresh);
    embi_js_runtime_free(rt);
}

EMB_TEST(atom_canonical_width_makes_narrow_wide_strings_equal)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *narrow = embi_js_atom_intern_utf8(rt, "AB", 2);
    uint16_t units[] = {'A', 'B'};
    embi_js_string *from_wide = embi_js_str_new_utf16(rt, units, 2);
    EMB_CHECK(!embi_js_str_is_wide(from_wide));
    EMB_CHECK(embi_js_atom_intern(rt, from_wide) == narrow);

    uint16_t astral[] = {0x3B1, 'x'};
    embi_js_string *w1 = embi_js_atom_intern(rt, embi_js_str_new_utf16(rt, astral, 2));
    embi_js_string *w2 = embi_js_atom_intern(rt, embi_js_str_new_utf16(rt, astral, 2));
    EMB_CHECK(w1 == w2);
    EMB_CHECK(embi_js_str_is_wide(w1));
    embi_js_runtime_free(rt);
}

EMB_TEST(atom_table_grows_and_keeps_identity)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    enum { N = 10000 };
    embi_js_string **first = embi_calloc(N, sizeof *first);
    for (uint32_t i = 0; i < N; i++) {
        char name[32];
        int n = SDL_snprintf(name, sizeof name, "k%u", (unsigned)i);
        first[i] = embi_js_atom_intern_utf8(rt, name, (size_t)n);
    }
    EMB_CHECK_EQ_INT(embi_js_atoms_count(rt), N);
    int same = 1;
    for (uint32_t i = 0; i < N; i++) {
        char name[32];
        int n = SDL_snprintf(name, sizeof name, "k%u", (unsigned)i);
        if (embi_js_atom_intern_utf8(rt, name, (size_t)n) != first[i]) {
            same = 0;
        }
    }
    EMB_CHECK(same);
    EMB_CHECK_EQ_INT(embi_js_atoms_count(rt), N);
    embi_free(first, N * sizeof *first);
    embi_js_runtime_free(rt);
}

EMB_TEST(key_numeric_strings_become_index_keys)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_key k5 = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "5", 1));
    EMB_CHECK(embi_js_key_is_index(k5));
    EMB_CHECK_EQ_INT(embi_js_key_index(k5), 5);
    EMB_CHECK(k5 == embi_js_key_from_index(5));

    embi_js_key kmax = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "4294967294", 10));
    EMB_CHECK(embi_js_key_is_index(kmax));
    EMB_CHECK_EQ_INT(embi_js_key_index(kmax), 4294967294u);

    /* These stay strings: leading zero, the 2^32-1 boundary, and a sign. */
    embi_js_key k05 = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "05", 2));
    EMB_CHECK(!embi_js_key_is_index(k05));
    EMB_CHECK(embi_js_is_string(k05));
    EMB_CHECK(embi_js_as_string(k05) == embi_js_atom_intern_utf8(rt, "05", 2));
    embi_js_key kbig = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "4294967295", 10));
    EMB_CHECK(!embi_js_key_is_index(kbig));
    embi_js_key kneg = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "-1", 2));
    EMB_CHECK(!embi_js_key_is_index(kneg));

    /* Equal string keys are the same key value. */
    embi_js_key a = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "foo", 3));
    embi_js_key b = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "foo", 3));
    EMB_CHECK(a == b);
    embi_js_runtime_free(rt);
}

EMB_TEST(key_index_zero_and_largest_round_trip)
{
    EMB_CHECK_EQ_INT(embi_js_key_index(embi_js_key_from_index(0)), 0);
    EMB_CHECK_EQ_INT(embi_js_key_index(embi_js_key_from_index(EMBI_JS_MAX_ARRAY_INDEX)),
                     4294967294LL);
    EMB_CHECK(embi_js_key_is_index(embi_js_key_from_index(0x80000000u)));
}

/* ------------------------------------------------------------------ atoms survive collection */

/* Overwrites dead stack below the caller, so stale copies of atom pointers from earlier calls
 * are not left for the conservative scan to find (see test_js_gc.c). */
static EMB_TEST_NOIPA void scrub_stack_impl(uintptr_t a, uintptr_t b,
                                                              uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub_stack() scrub_stack_impl(0, 0, 0, 0)

static bool atom_text_is(const embi_js_string *s, const char *ascii)
{
    size_t n = strlen(ascii);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint16_t)(unsigned char)ascii[i]) {
            return false;
        }
    }
    return true;
}

/* The addresses live in statics, which the conservative scan does not read, and the strings are
 * not returned to the caller. Only the atom table can keep these cells alive. */
static uintptr_t g_hello_addr;
static uintptr_t g_wide_addr;

static __attribute__((noinline)) void intern_survivors(embi_js_runtime *rt)
{
    static const uint16_t wide_units[] = {0x3B1, 'b', 'c'};
    g_hello_addr = (uintptr_t)embi_js_atom_intern_utf8(rt, "hello", 5);
    g_wide_addr = (uintptr_t)embi_js_atom_intern(rt, embi_js_str_new_utf16(rt, wide_units, 3));
}

/* 20000 cells of 128 bytes is about 2.5 MB, past the 1 MB default trigger, so the runtime collects
 * on its own while the atoms are only held by the atom table. */
static __attribute__((noinline)) void make_garbage(embi_js_runtime *rt, int n)
{
    uint8_t text[100];
    memset(text, 'g', sizeof text);
    for (int i = 0; i < n; i++) {
        (void)embi_js_str_new_latin1(rt, text, sizeof text);
    }
}

static __attribute__((noinline)) void atoms_survive_scenario(embi_js_runtime *rt)
{
    intern_survivors(rt);
    embi_js_mem_stats before;
    embi_js_get_mem_stats(rt, &before);
    make_garbage(rt, 20000);
    embi_js_mem_stats after;
    embi_js_get_mem_stats(rt, &after);
    EMB_CHECK(after.gc_count > before.gc_count);

    scrub_stack();
    embi_js_gc(rt);

    embi_js_cell *hello_cell = embi_js_heap_find_cell(&rt->heap, g_hello_addr);
    EMB_REQUIRE(hello_cell != nullptr);
    const embi_js_string *hello = (const embi_js_string *)hello_cell;
    EMB_CHECK(embi_js_str_is_atom(hello));
    EMB_CHECK(atom_text_is(hello, "hello"));
    /* Re-interning the same contents returns the surviving cell. */
    EMB_CHECK((uintptr_t)embi_js_atom_intern_utf8(rt, "hello", 5) == g_hello_addr);

    embi_js_cell *wide_cell = embi_js_heap_find_cell(&rt->heap, g_wide_addr);
    EMB_REQUIRE(wide_cell != nullptr);
    const embi_js_string *wide = (const embi_js_string *)wide_cell;
    EMB_CHECK(embi_js_str_is_atom(wide));
    EMB_CHECK(embi_js_str_is_wide(wide));
    EMB_CHECK_EQ_INT(embi_js_str_len(wide), 3);
    EMB_CHECK_EQ_INT(embi_js_str_at(wide, 0), 0x3B1);
    EMB_CHECK_EQ_INT(embi_js_str_at(wide, 1), 'b');
    EMB_CHECK_EQ_INT(embi_js_str_at(wide, 2), 'c');
    static const uint16_t same[] = {0x3B1, 'b', 'c'};
    EMB_CHECK((uintptr_t)embi_js_atom_intern(rt, embi_js_str_new_utf16(rt, same, 3)) ==
              g_wide_addr);
    EMB_CHECK_EQ_INT(embi_js_atoms_count(rt), 2);
}

EMB_TEST(atoms_survive_automatic_and_explicit_collections)
{
    scrub_stack();
    embi_js_runtime *rt = embi_js_runtime_new();
    atoms_survive_scenario(rt);
    embi_js_runtime_free(rt);
    scrub_stack();
}

EMB_TEST_MAIN()
