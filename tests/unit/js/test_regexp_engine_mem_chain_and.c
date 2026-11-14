#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A v-mode intersection chain must not retain one result set per operator. embi_mem_peak only ever grows, so each executable holds one measurement. */

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

static uint32_t flags_of(const char *s)
{
    uint16_t u[8];
    uint32_t n = 0;
    for (; s[n]; n++) {
        u[n] = (uint8_t)s[n];
    }
    uint32_t f = 0;
    EMB_CHECK(embi_re_parse_flags(u, n, &f));
    return f;
}

static uint16_t *build_pat(const char *prefix, const char *unit, uint32_t reps, uint32_t *len)
{
    uint32_t pl, ul;
    uint16_t *a = repeat_pat(prefix, 1, &pl);
    uint16_t *b = repeat_pat(unit, reps, &ul);
    uint16_t *pat = malloc(((size_t)pl + ul + 1) * sizeof *pat);
    memcpy(pat, a, (size_t)pl * sizeof *pat);
    memcpy(pat + pl, b, (size_t)ul * sizeof *pat);
    pat[pl + ul] = ']';
    *len = pl + ul + 1;
    free(a);
    free(b);
    return pat;
}

EMB_TEST(long_intersection_chain_compiles_in_bounded_memory)
{
    const uint32_t reps = 20000;
    uint32_t len;
    uint16_t *pat = build_pat("[\\p{L}", "&&\\p{L}", reps, &len);
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_error err = {0, nullptr};
    embi_re_prog *p = embi_re_compile(pat, len, flags_of("v"), &err);
    uint64_t dt = SDL_GetTicks() - t0;
    size_t growth = embi_mem_peak() - before;
    SDL_Log("    peak growth %llu bytes, %llu ms", (unsigned long long)growth,
            (unsigned long long)dt);
    if (p) {
        /* Compiled: the chain holds two sets, not one per operator. */
        EMB_CHECK(growth < ((size_t)8 << 20));
        embi_re_free(p);
    } else {
        EMB_CHECK(err.msg != nullptr && strcmp(err.msg, "pattern too complex") == 0);
        EMB_CHECK(growth < ((size_t)100 << 20));
    }
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 40000u : 4000u));
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST_MAIN()
