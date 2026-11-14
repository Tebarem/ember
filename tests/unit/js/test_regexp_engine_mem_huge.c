#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A very large but ordinary pattern compiles in bounded memory. */

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

EMB_TEST(hundred_thousand_property_escapes_compile)
{
    /* new RegExp('\\p{L}'.repeat(100000), 'u') */
    const uint32_t reps = 100000;
    uint32_t len;
    uint16_t *pat = repeat_pat("\\p{L}", reps, &len);
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, len, EMBI_RE_FLAG_U, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    size_t growth = embi_mem_peak() - before;
    SDL_Log("    peak growth %llu bytes, %llu ms", (unsigned long long)growth,
            (unsigned long long)dt);
    EMB_REQUIRE(p != nullptr);
    /* Nodes, code and the shared set only: well under 300 bytes per escape (it was gigabytes). */
    EMB_CHECK(growth < (size_t)reps * 300);
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 30000u : 3000u));
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST_MAIN()
