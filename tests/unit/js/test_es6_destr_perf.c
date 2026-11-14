/* Parse-time bound for the destructuring cover grammar: a deeply nested literal must cost about the
 * same per token as a flat one (the pattern lookahead is memoized, so it is linear in the source
 * size, not size times nesting depth), and destructuring runs at a sane speed. */
#include "es6_util.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>

/* "[[[...[1,2,...,n]...]]]" with depth levels, or n objects "{a:[...]}" per level when obj is set. */
static char *make_src(uint32_t depth, uint32_t n, bool obj)
{
    size_t cap = (size_t)n * 12u + (size_t)depth * 8u + 64u;
    char *buf = malloc(cap);
    size_t len = 0;
    len += (size_t)snprintf(buf + len, cap - len, "var v = ");
    for (uint32_t d = 0; d < depth; d++) {
        len += (size_t)snprintf(buf + len, cap - len, obj ? "{a:" : "[");
    }
    len += (size_t)snprintf(buf + len, cap - len, "[");
    for (uint32_t i = 0; i < n; i++) {
        len += (size_t)snprintf(buf + len, cap - len, i ? ",%u" : "%u", i);
    }
    len += (size_t)snprintf(buf + len, cap - len, "]");
    for (uint32_t d = 0; d < depth; d++) {
        len += (size_t)snprintf(buf + len, cap - len, obj ? "}" : "]");
    }
    snprintf(buf + len, cap - len, ";");
    return buf;
}

static uint64_t best_of(es6_env e, const char *src, int reps)
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < reps; i++) {
        uint64_t t0 = SDL_GetTicksNS();
        embi_js_value v = embi_js_eval(e.r, src, strlen(src), "perf.js", 0);
        uint64_t dt = SDL_GetTicksNS() - t0;
        if (v == EMBI_JS_EXCEPTION) {
            return UINT64_MAX;
        }
        if (dt < best) {
            best = dt;
        }
    }
    return best;
}

EMB_TEST(destr_parse_nested_linear)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t n = EMB_TEST_ASAN ? 20000u : 100000u;
    /* The same leaf count at 1 level, and at 50 levels of nesting (the extra levels add brackets
     * only, so the token counts are within a few percent). */
    char *flat = make_src(0, n, false);
    char *deep = make_src(50, n, false);
    char *deepo = make_src(50, n, true);
    uint64_t tf = best_of(e, flat, 3);
    uint64_t td = best_of(e, deep, 3);
    uint64_t to = best_of(e, deepo, 3);
    SDL_Log("    parse+run: flat %.2f ms, 50-deep arrays %.2f ms, 50-deep objects %.2f ms",
            (double)tf / 1e6, (double)td / 1e6, (double)to / 1e6);
    EMB_REQUIRE(tf != UINT64_MAX && td != UINT64_MAX && to != UINT64_MAX);
    /* A rescan per level would make the deep source about 50x slower. */
    EMB_CHECK(td < tf * 4u + 2000000u);
    EMB_CHECK(to < tf * 4u + 2000000u);
    free(flat);
    free(deep);
    free(deepo);
    es6_env_free(e);
}

/* ns per destructured element, reported only. */
EMB_TEST(destr_ns_per_op)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t iters = EMB_TEST_ASAN ? 20000u : 200000u;
    char src[512];
    snprintf(src, sizeof src,
             "var s = 0; var arr = [1, 2, 3]; var ob = {a: 1, b: 2, c: 3};"
             "for (var i = 0; i < %u; i++) { var [x, y, z] = arr; var {a, b, c} = ob; s += x + a; } s",
             iters);
    uint64_t t0 = SDL_GetTicksNS();
    embi_js_value v = es6_run(e, src);
    uint64_t dt = SDL_GetTicksNS() - t0;
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(es6_num(v) == 2.0 * iters);
    SDL_Log("    destructuring: %.1f ns per element (6 elements per iteration)",
            (double)dt / ((double)iters * 6.0));
    es6_env_free(e);
}

EMB_TEST_MAIN()
