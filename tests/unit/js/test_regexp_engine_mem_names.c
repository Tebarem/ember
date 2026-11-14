#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate group names: backreferences to a name shared by many groups must not copy the group
 * list per reference. embi_mem_peak only ever grows, so this exe holds one measurement. */

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

EMB_TEST(backrefs_to_duplicate_name_share_one_list)
{
    const uint32_t reps = 6000;
    uint32_t len;
    uint16_t *pat = repeat_pat("\\k<a>(?<a>b)|", reps, &len);
    size_t before = embi_mem_live();
    uint64_t t0 = SDL_GetTicks();
    embi_re_prog *p = embi_re_compile(pat, len, 0, nullptr);
    uint64_t dt = SDL_GetTicks() - t0;
    size_t growth = embi_mem_peak() - before;
    SDL_Log("    peak growth %llu bytes, %llu ms", (unsigned long long)growth,
            (unsigned long long)dt);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_re_group_count(p) == reps);
    /* Before the fix this peaked near 832 MB: a private list of every same-named group per \k.
     * Nodes, one shared list, and the code array come to about 1 KB per repetition; allow 2 KB. */
    EMB_CHECK(growth < (size_t)reps * 2048);
    EMB_CHECK(dt <= (EMB_TEST_ASAN ? 20000u : 2000u));
    embi_re_free(p);
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST_MAIN()
