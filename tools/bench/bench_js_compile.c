/*
 * Compile-only benchmark of the JS front end (lexer, parser, scope analysis, bytecode emitter):
 *     bench_js_compile FILE.js [RUNS]
 * Each run creates a fresh runtime and realm, compiles FILE as a strict classic script (the TodoMVC
 * bundle has no import or export) and reports the compiler time (embi_js_compile_ms). Run 1 is the
 * cold one (first touch of the code and of the arena memory); the median of the others is the warm
 * time. A profile of the warm loop shows where the front end spends its time (SZ2).
 */
#include "base/base.h"
#include "js/compiler.h"
#include "js/js.h"
#include "js/runtime.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <stdlib.h>

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        SDL_Log("usage: bench_js_compile FILE.js [RUNS]");
        return 2;
    }
    int runs = argc > 2 ? atoi(argv[2]) : 20;
    if (runs < 2 || runs > 100000) {
        runs = 20;
    }
    size_t len = 0;
    void *src = SDL_LoadFile(argv[1], &len);
    if (src == nullptr) {
        SDL_Log("cannot read %s", argv[1]);
        return 2;
    }
    double *ms = SDL_calloc((size_t)runs, sizeof *ms);
    for (int i = 0; i < runs; i++) {
        embi_js_runtime *rt = embi_js_runtime_new();
        embi_js_realm *r = rt ? embi_js_realm_new(rt) : nullptr;
        if (r == nullptr) {
            SDL_Log("runtime failed");
            return 1;
        }
        embi_js_value code = embi_js_compile(r, (const uint8_t *)src, (uint32_t)len, "bundle.js", true);
        if (embi_js_is_exception(code)) {
            SDL_Log("compile failed");
            return 1;
        }
        ms[i] = embi_js_compile_ms(rt);
        embi_js_realm_free(r);
        embi_js_runtime_free(rt);
    }
    double first = ms[0];
    qsort(ms + 1, (size_t)runs - 1, sizeof *ms, cmp_d);
    SDL_Log("compile %u bytes: first %.2f ms, then median %.2f ms (min %.2f, max %.2f, n=%d)", (unsigned)len, first,
            ms[1 + (runs - 1) / 2], ms[1], ms[runs - 1], runs - 1);
    SDL_free(ms);
    SDL_free(src);
    return 0;
}
