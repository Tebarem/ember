/*
 * ES2015+ micro-benchmarks of the JS engine. Each case runs a short
 * script that loops N times, and the time of the whole evaluation is divided by the number of
 * operations the loop performs:
 *   - destructuring of an array and of an object (per destructuring),
 *   - spread into a call and into an array literal (per spread),
 *   - for-of over an array and over a generator (per element),
 *   - class instantiation (per new),
 *   - Map and Set set and get (per operation),
 *   - RegExp exec and replace with a global pattern (per exec, per match),
 *   - BigInt add and multiply-and-mask (per operation),
 *   - TypedArray element write and read, and a Float64Array sum (per access, per element),
 *   - the J5 fast paths: field get and set, method call, global read, element read and write,
 *     push and pop (per push-pop pair), string +=, and an int loop (per operation, from J5a).
 * The best of several runs is reported in ns per operation. J5 tunes these numbers; J2 only needs
 * them recorded. Run it from a Release build for numbers:
 *     cmake --build build/<dir> --target bench_js_es6
 *     build/<dir>/tools/bench/bench_js_es6
 * Exit status is 1 when a script fails to run.
 */

#include "base/base.h"
#include "js/js.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define BENCH_RUNS 5
#define SCRIPT_MAX 1024

/* One benchmark: a script template with one %d for the loop count, the operations per loop
 * iteration (so ns/op divides by N times this), and a label. */
typedef struct bench_case {
    const char *name;
    const char *script; /* the loop count is the single %d */
    int ops_per_iter;
    int iters;
} bench_case;

static const bench_case cases[] = {
    {"destructure array [a, b] = arr",
     "var arr = [1, 2, 3]; var s = 0; for (var i = 0; i < %d; i++) { var [a, b] = arr; s += a + b; } s",
     1, 200000},
    {"destructure object {x, y} = o",
     "var o = {x: 1, y: 2, z: 3}; var s = 0; for (var i = 0; i < %d; i++) { var {x, y} = o; s += x + y; } s",
     1, 200000},
    {"spread into call f(...args)",
     "function f(a, b, c) { return a + b + c } var args = [1, 2, 3]; var s = 0;"
     " for (var i = 0; i < %d; i++) { s += f(...args); } s",
     1, 200000},
    {"spread into array literal [...src]",
     "var src = [1, 2, 3]; var n = 0; for (var i = 0; i < %d; i++) { n += [...src].length; } n",
     1, 200000},
    {"for-of over an array (per element)",
     "var arr = []; for (var k = 0; k < 1000; k++) arr.push(k); var s = 0;"
     " for (var i = 0; i < %d; i++) { for (var v of arr) s += v; } s",
     1000, 300},
    {"generator iteration (per yield)",
     "function* g(n) { for (var i = 0; i < n; i++) yield i } var s = 0;"
     " for (var i = 0; i < %d; i++) { for (var v of g(1000)) s += v; } s",
     1000, 100},
    {"class instantiation new P(i)",
     "class P { constructor(x) { this.x = x } } var s = 0;"
     " for (var i = 0; i < %d; i++) { s += new P(i).x; } s",
     1, 200000},
    {"Map set and get",
     "var m = new Map(); for (var i = 0; i < %d; i++) m.set(i, i); var s = 0;"
     " for (var i = 0; i < %d; i++) s += m.get(i); s",
     2, 100000},
    {"Set add and has",
     "var t = new Set(); for (var i = 0; i < %d; i++) t.add(i); var s = 0;"
     " for (var i = 0; i < %d; i++) if (t.has(i)) s++; s",
     2, 100000},
    {"RegExp test /a(b+)c/ (per exec)",
     "var re = /a(b+)c/; var s = 0; for (var i = 0; i < %d; i++) { if (re.test('xxabbbcyy')) s++; } s",
     1, 100000},
    {"RegExp replace /\\d+/g (per match)",
     "var s = 0; var str = '12 34 56 78 90'; for (var i = 0; i < %d; i++) { s += str.replace(/\\d+/g, 'x').length; } s",
     5, 20000},
    {"BigInt add 64-bit (per op)",
     "var a = 12345678901234567890n; var s = 0n; for (var i = 0; i < %d; i++) { s = s + a; } s",
     1, 200000},
    {"BigInt multiply and mask (per op)",
     "var s = 1n; for (var i = 0; i < %d; i++) { s = (s * 3n) & 0xffffffffn; } s",
     2, 200000},
    {"Uint8Array write and read (per access)",
     "var a = new Uint8Array(1024); var s = 0; for (var i = 0; i < %d; i++) { a[i & 1023] = i; s += a[(i * 7) & 1023]; } s",
     2, 200000},
    {"Float64Array sum (per element)",
     "var a = new Float64Array(1000); for (var k = 0; k < 1000; k++) a[k] = k; var s = 0;"
     " for (var i = 0; i < %d; i++) { for (var j = 0; j < 1000; j++) s += a[j]; } s",
     1000, 300},
    /* J5a: one case per fast path that J5 tunes. The loops sit in
     * functions so that their locals are not global property accesses, except where the global
     * read itself is measured. */
    {"field get o.x (per access)",
     "(function () { var o = {x: 1, y: 2}; var s = 0; for (var i = 0; i < %d; i++) { s = (s + o.x) | 0; } return s; })()",
     1, 2000000},
    {"field set o.x = i (per store)",
     "(function () { var o = {x: 1, y: 2}; for (var i = 0; i < %d; i++) { o.x = i; } return o.x; })()",
     1, 2000000},
    {"method call o.m() (per call)",
     "(function () { var o = {v: 2, m: function () { return this.v; }}; var s = 0;"
     " for (var i = 0; i < %d; i++) { s = (s + o.m()) | 0; } return s; })()",
     1, 1000000},
    {"global read g (per read)",
     "var g = 3; (function () { var s = 0; for (var i = 0; i < %d; i++) { s = (s + g) | 0; } return s; })()",
     1, 2000000},
    {"element read a[i] (per access)",
     "(function () { var a = []; for (var k = 0; k < 1024; k++) a.push(k); var s = 0;"
     " for (var i = 0; i < %d; i++) { s = (s + a[i & 1023]) | 0; } return s; })()",
     1, 2000000},
    {"element write a[i] = i (per store)",
     "(function () { var a = []; for (var k = 0; k < 1024; k++) a.push(0);"
     " for (var i = 0; i < %d; i++) { a[i & 1023] = i; } return a[5]; })()",
     1, 2000000},
    {"push and pop (per pair)",
     "(function () { var a = []; var s = 0; for (var i = 0; i < %d; i++) { a.push(i); s = (s + a.pop()) | 0; }"
     " return s; })()",
     1, 1000000},
    {"string += one char (per append)",
     "(function () { var s = ''; for (var i = 0; i < %d; i++) { s += 'a'; } return s.length; })()",
     1, 20000},
    {"int loop (per iteration)",
     "(function () { var s = 0; for (var i = 0; i < %d; i++) { s = (s + i * 3) & 0xffff; } return s; })()",
     1, 5000000},
};

/* Evaluates the script with the loop count filled in. Returns the elapsed time in nanoseconds, or
 * a negative number when the script throws (the message is logged). */
static double run_once(embi_js_realm *r, const bench_case *c)
{
    char src[SCRIPT_MAX];
    /* The Map and Set cases name the count twice, so the count is passed for each %d. */
    SDL_snprintf(src, sizeof src, c->script, c->iters, c->iters);
    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_js_value v = embi_js_eval(r, src, strlen(src), "bench.js", 0);
    uint64_t t1 = SDL_GetPerformanceCounter();
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(r->rt);
        return -1.0;
    }
    double ns = (double)(t1 - t0) * 1e9 / (double)SDL_GetPerformanceFrequency();
    return ns;
}

int main(void)
{
    int failures = 0;
    SDL_Log("ES2015+ JS micro-benchmarks (ns per operation, best of %d runs)", BENCH_RUNS);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const bench_case *c = &cases[i];
        double best = -1.0;
        bool ok = true;
        for (int run = 0; run < BENCH_RUNS && ok; run++) {
            embi_js_runtime *rt = embi_js_runtime_new();
            embi_js_realm *r = rt ? embi_js_realm_new(rt) : nullptr;
            if (!r) {
                ok = false;
                break;
            }
            double ns = run_once(r, c);
            if (ns < 0.0) {
                ok = false;
            } else if (best < 0.0 || ns < best) {
                best = ns;
            }
            embi_js_realm_free(r);
            embi_js_runtime_free(rt);
        }
        if (!ok) {
            SDL_Log("FAIL %-40s script threw", c->name);
            failures++;
            continue;
        }
        double ops = (double)c->iters * (double)c->ops_per_iter;
        SDL_Log("%-40s %10.1f ns/op  (%.0f ops)", c->name, best / ops, ops);
    }
    return failures ? 1 : 0;
}
