#include "emb_test.h"

#include "js/js.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* GC stress for the interpreter and the compiler: a collection runs at nearly every allocation,
 * so each value that a frame, a box, a closure, a constant or an iterator holds must be found by
 * the marker. Each script has a known result, so a lost value shows up as a wrong answer or a
 * crash. */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new_stressed(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    if (e.rt) {
        embi_js_set_gc_params(e.rt, 0, 0.0);
    }
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "gc.js", 0);
}

static double num_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

static bool text_is(embi_js_runtime *rt, embi_js_value v, const char *want)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

typedef struct stress_case {
    const char *src;
    double want;
} stress_case;

static const stress_case number_cases[] = {
    /* A catch parameter captured by closures made in each iteration. */
    {"var fs = []; for (var i = 0; i < 50; i++) { try { throw i; } catch (e) {"
     " fs[i] = function() { return e + 1; }; } } fs[10]() + fs[49]()",
     61},
    /* Iterator keys and boxed loop state across try and closures. */
    {"var s = 0; for (var k in {a: 1, b: 2, c: 3}) { try { s += 1; throw k; }"
     " catch (q) { s += (function() { return q === k ? 1 : 0; })(); } } s",
     6},
    /* A switch discriminant and a finally route kept over allocations. */
    {"var n = 0; for (var k in {q: 1, r: 2}) { switch (k) { case 'q': try { n += 3; throw 1; }"
     " catch (x) { n += x; } finally { n += 10; } break; default: n += 100; } } n",
     (double)(3 + 1 + 10 + 100)},
    /* A function declared in a catch block captures the catch binding. */
    {"function mk() { try { throw 9; } catch (e) { function g() { return e * 2; } return g; } }"
     " var gg = mk(); gg() + gg()",
     36},
    /* The self name of a named function expression, recursing under collection. */
    {"var nf = function self(n) { return n == 0 ? 0 : n + self(n - 1); }; nf(50)", 1275},
    /* Many arguments pushed while handlers are entered inside nested loops. */
    {"function g(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16,"
     " p17, p18, p19) { return p0 + p1 * 2 + p18 * 10 + p19; } function f() { var t = 0;"
     " for (var a in {x: 1}) for (var b in {y: 1}) { try {} catch (e) {} try {} catch (e) {}"
     " t += g(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19); } return t; }"
     " f()",
     2 + 180 + 19},
    /* Property keys built from strings in a loop. */
    {"var o = {}; for (var j = 0; j < 300; j++) { o['k' + (j % 10)] = j; } o.k9", 299},
    /* for-in into member targets (base and key held in temps) and a once-converted key. */
    {"var t = {}, kk = {toString: function() { return 'q' + 'z'; }}, cnt = 0;"
     " for (t[kk] in {aa: 1, bb: 2}) { cnt += t.qz.length; } t[kk] = 1; t[kk]++; t.qz + cnt",
     2 + 4},
    /* Labelled loops with continue and closures. */
    {"var c = 0; out: mid: for (var i = 0; i < 4; i++) { for (var j = 0; j < 4; j++) {"
     " var keep = function() { return j; }; if (keep() == 1) continue out; c += 1; } } c",
     4},
};

EMB_TEST(gc_stress_interpreter_results_survive_collection)
{
    env e = env_new_stressed();
    EMB_REQUIRE(e.r != nullptr);
    for (uint32_t i = 0; i < sizeof number_cases / sizeof number_cases[0]; i++) {
        embi_js_value v = run(e, number_cases[i].src);
        EMB_CHECK(v != EMBI_JS_EXCEPTION);
        EMB_CHECK(embi_js_is_number(v) && num_of(v) == number_cases[i].want);
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(e.rt);
        }
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(e.rt, &st);
    EMB_CHECK(st.gc_count > 500); /* FP1a: compiling allocates fewer cells (one code block, no string for a known atom), so fewer stress collections */
    env_free(e);
}

EMB_TEST(gc_stress_strings_and_exceptions)
{
    env e = env_new_stressed();
    EMB_REQUIRE(e.r != nullptr);
    /* A thrown string and its message come back intact after many collections. */
    embi_js_value v = run(e, "var m = ''; try { throw 'boom' + 'ed'; } catch (x) { m = x; } m");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(text_is(e.rt, v, "boomed"));

    /* Each caught Error keeps its message while later iterations allocate. */
    v = run(e,
            "var msgs = ''; for (var q = 0; q < 40; q++) { try { null.x; } catch (err) {"
            " msgs = err.name; } } msgs");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(text_is(e.rt, v, "TypeError"));
    env_free(e);
}

EMB_TEST(gc_stress_compiler_constants_survive_parsing)
{
    env e = env_new_stressed();
    EMB_REQUIRE(e.r != nullptr);
    /* A long script whose constants (strings, names, nested functions) are created while the
     * compiler allocates, so the unit must root them. */
    embi_sb sb = {nullptr, 0, 0};
    embi_sb_append(&sb, (embi_str){"var acc = 0;", 12});
    for (int i = 0; i < 120; i++) {
        char line[160];
        int n = SDL_snprintf(line, sizeof line,
                             " acc += (function(a) { return a + 'x%d' === 'x%dx' ? 0 : %d; })(%d);",
                             i, i, i, i);
        embi_sb_append(&sb, (embi_str){line, (uint32_t)n});
    }
    embi_sb_append(&sb, (embi_str){" acc", 4});
    embi_js_value v = embi_js_eval(e.r, embi_sb_cstr(&sb), sb.len, "long.js", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    /* Sum of 0..119 added on each call with a nonmatching string comparison gives the total. */
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == (119.0 * 120.0) / 2.0);
    embi_sb_free(&sb);
    env_free(e);
}

EMB_TEST_MAIN()
