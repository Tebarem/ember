/*
 * Runs tests/data/css-parsing-tests/An+B.json through embi_sel_parse_anb. Each entry is an input
 * string and either null (not an An+B) or [A, B]. Every case must match exactly, with no
 * expected-failure list.
 */
#include "emb_test.h"

#include "css/selector.h"
#include "json_mini.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <string.h>

#define ANB_PATH EMBER_SOURCE_DIR "/tests/data/css-parsing-tests/An+B.json"

EMB_TEST(anb_fixture_suite)
{
    size_t n = 0;
    char *text = SDL_LoadFile(ANB_PATH, &n);
    EMB_REQUIRE(text != nullptr);

    jm_val root;
    bool parsed = jm_parse(text, n, &root);
    SDL_free(text);
    EMB_REQUIRE(parsed);
    EMB_REQUIRE(root.type == JM_ARRAY);
    EMB_REQUIRE(root.count % 2 == 0);

    uint32_t cases = 0;
    uint32_t failures = 0;
    for (size_t k = 0; k + 1 < root.count; k += 2, cases++) {
        const jm_val *input = &root.items[k];
        const jm_val *expected = &root.items[k + 1];
        EMB_REQUIRE(input->type == JM_STRING);

        embi_arena arena;
        embi_arena_init(&arena, 1024);
        embi_css_slice cvs = embi_css_parse_cvs(&arena, (embi_str){input->str, (uint32_t)input->len});
        int64_t a = 0;
        int64_t b = 0;
        bool ok = embi_sel_parse_anb(cvs, &a, &b);

        bool want_ok = expected->type == JM_ARRAY;
        bool case_ok = ok == want_ok;
        if (want_ok && ok) {
            EMB_REQUIRE(expected->count == 2);
            case_ok = a == (int64_t)expected->items[0].num && b == (int64_t)expected->items[1].num;
        }
        if (!case_ok) {
            failures++;
            SDL_Log("    An+B case %u: \"%.*s\" got %s (%lld, %lld)", (unsigned)cases,
                    (int)input->len, input->str, ok ? "ok" : "invalid", (long long)a, (long long)b);
        }
        embi_arena_free(&arena);
    }
    jm_free(&root);

    EMB_CHECK(cases > 100);
    EMB_CHECK_EQ_INT(failures, 0);
}

EMB_TEST_MAIN()
