#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time memory: temporaries must not pile up, so peak memory while compiling N copies of an
 * escape is a small multiple of the pattern, not of the Unicode tables times N. embi_mem_peak only
 * ever grows, so each executable holds one measurement. */

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

EMB_TEST(negated_property_v_ignore_case_compiles_in_bounded_memory)
{
    const uint32_t reps = 1500;
    uint32_t len;
    uint16_t *pat = repeat_pat("\\P{L}", reps, &len);
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, len, flags_of("vi"), nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    size_t growth = embi_mem_peak() - before;
    SDL_Log("    peak growth %llu bytes, %llu ms", (unsigned long long)growth,
            (unsigned long long)dt);
    EMB_REQUIRE(p != nullptr);
    /* Before the fix this peaked near 456 MB. Allow 400 bytes per escape. */
    EMB_CHECK(growth < (size_t)reps * 400);
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 20000u : 2000u));
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST_MAIN()
