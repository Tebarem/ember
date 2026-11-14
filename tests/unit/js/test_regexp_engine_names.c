#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate group names: registering the n-th group of one name must not walk the earlier ones, and
 * the duplicate-name conflict rule (groups conflict unless they sit in different alternatives of a
 * shared disjunction) holds with the one-comparison registration. */

static uint16_t *repeat_pat(const char *unit, uint32_t reps, uint32_t *len)
{
    size_t ul = strlen(unit);
    uint16_t *pat = malloc(ul * reps * sizeof *pat);
    for (uint32_t i = 0; i < reps; i++) {
        for (size_t k = 0; k < ul; k++) {
            pat[(size_t)i * ul + k] = (uint16_t)(uint8_t)unit[k];
        }
    }
    *len = (uint32_t)(ul * reps);
    return pat;
}

static embi_re_prog *compile_ascii(const char *s, uint32_t flags, embi_re_error *err)
{
    uint32_t n = (uint32_t)strlen(s);
    uint16_t *u = malloc((n ? n : 1) * sizeof *u);
    for (uint32_t i = 0; i < n; i++) {
        u[i] = (uint8_t)s[i];
    }
    embi_re_prog *p = embi_re_compile(u, n, flags, err);
    free(u);
    return p;
}

EMB_TEST(registering_many_groups_of_one_name_is_linear)
{
    /* (?<a>x)| repeated 20000 times: every group sits in its own alternative. Comparing each new
     * group with all earlier ones took quadratic time. */
    const uint32_t reps = 20000;
    uint32_t len;
    uint16_t *pat = repeat_pat("(?<a>x)|", reps, &len);
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, len, 0, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 20000u : 1000u));
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST(duplicate_names_still_conflict_exactly_when_they_must)
{
    static const struct {
        const char *pat;
        bool ok;
    } cases[] = {
        {"(?<a>x)|(?<a>y)", true},
        {"(?<a>x)|(?<a>y)|(?<a>z)", true},
        {"(?:(?<a>x)|(?<a>y))(?<a>z)", false},
        {"(?<a>x)(?:(?<a>y)|z)", false},
        {"(?:(?<a>x)|y)|(?<a>z)", true},
        {"(?:(?<a>x)|(?<a>y))|(?<a>z)", true},
        {"(?:(?<a>x)|(?<a>y))|(?<a>z)(?<a>w)", false},
        {"(?<a>x)|(?<b>y)(?<a>z)", true},
        {"(?:(?<a>x)|(?<b>y))(?<a>z)", false},
        {"(?<a>(?<a>x))", false},
        {"(?:(?<a>x)|(?<b>y))|(?:(?<b>z)|(?<a>w))", true},
        {"(?<a>x)|(?:(?<a>y)|(?<a>z))|(?<a>w)", true},
        {"(?<a>x)|(?:(?<a>y)|(?<a>z)(?<a>v))", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_re_error err;
        embi_re_prog *p = compile_ascii(cases[i].pat, 0, &err);
        if ((p != nullptr) != cases[i].ok) {
            SDL_Log("    unexpected result for %s", cases[i].pat);
        }
        EMB_CHECK((p != nullptr) == cases[i].ok);
        embi_re_free(p);
    }
    /* The shared list keeps group order: \k<a> tries the group that participated. */
    embi_re_prog *p = compile_ascii("(?:(?<a>x)|(?<a>y))\\k<a>", 0, nullptr);
    EMB_REQUIRE(p != nullptr);
    uint16_t in[] = {'y', 'y', 'x', 'x'};
    uint32_t caps[16];
    embi_re_input inp = {in, 4, true};
    EMB_CHECK(embi_re_search(p, inp, 0, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(caps[0] == 0 && caps[1] == 2);
    EMB_CHECK(embi_re_search(p, inp, 2, caps, nullptr) == EMBI_RE_MATCH);
    EMB_CHECK(caps[0] == 2 && caps[1] == 4);
    embi_re_free(p);
}

EMB_TEST_MAIN()
