#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A pattern that would need an unreasonable amount of memory returns a syntax error instead of
 * aborting the process. */

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

EMB_TEST(oversized_pattern_is_a_syntax_error_not_an_abort)
{
    /* Three million atoms need more than the parse budget (64 MB). */
    const uint32_t count = 3000000;
    uint16_t *pat = malloc((size_t)count * sizeof *pat);
    for (uint32_t i = 0; i < count; i++) {
        pat[i] = 'a';
    }
    size_t before = embi_mem_live();
    embi_re_error err = {0, nullptr};
    embi_re_prog *p = embi_re_compile(pat, count, 0, &err);
    size_t growth = embi_mem_peak() - before;
    SDL_Log("    peak growth %llu bytes", (unsigned long long)growth);
    EMB_CHECK(p == nullptr);
    EMB_CHECK(err.msg != nullptr && strcmp(err.msg, "pattern too complex") == 0);
    EMB_CHECK(growth < ((size_t)160 << 20));
    EMB_CHECK(embi_mem_live() == before);
    free(pat);

    /* Large classes accumulate too: the same cap applies in v mode. */
    const uint32_t reps = 400000;
    uint32_t len;
    pat = repeat_pat("[a-c]", reps, &len);
    p = embi_re_compile(pat, len, EMBI_RE_FLAG_V, &err);
    EMB_CHECK(p == nullptr);
    EMB_CHECK(err.msg != nullptr && strcmp(err.msg, "pattern too complex") == 0);
    EMB_CHECK(embi_mem_live() == before);
    free(pat);
}

EMB_TEST_MAIN()
