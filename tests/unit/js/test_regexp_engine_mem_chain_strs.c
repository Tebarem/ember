#include "emb_test.h"

#include "base/base.h"
#include "js/regexp.h"

#include <SDL3/SDL_timer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A backslash-q class with hundreds of strings and then a long subtraction chain must not copy the string list
 * per operator. embi_mem_peak only ever grows, so each executable holds one measurement. */

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

/* Builds a class of 400 strings followed by reps copies of a one-string subtraction. */
static uint16_t *mkstrs(uint32_t reps, uint32_t *len)
{
    char *head = malloc(400 * 8 + 16);
    size_t n = 0;
    n += (size_t)sprintf(head + n, "\\q{");
    for (uint32_t i = 0; i < 400; i++) {
        n += (size_t)sprintf(head + n, "%sstr%04u", i ? "|" : "", i);
    }
    n += (size_t)sprintf(head + n, "}");
    uint32_t hl;
    uint16_t *h = repeat_pat(head, 1, &hl);
    free(head);
    uint32_t tl;
    uint16_t *t = repeat_pat("--\\q{abc}", reps, &tl);
    uint16_t *pat = malloc(((size_t)hl + tl + 2) * sizeof *pat);
    pat[0] = '[';
    memcpy(pat + 1, h, (size_t)hl * sizeof *pat);
    memcpy(pat + 1 + hl, t, (size_t)tl * sizeof *pat);
    pat[1 + hl + tl] = ']';
    *len = hl + tl + 2;
    free(h);
    free(t);
    return pat;
}

EMB_TEST(long_string_subtraction_chain_compiles_in_bounded_memory)
{
    const uint32_t reps = 20000;
    uint32_t len;
    uint16_t *pat = mkstrs(reps, &len);
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
