#include "emb_test.h"

#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static embi_js_value run(env e, const char *src, unsigned flags)
{
    return embi_js_eval(e.r, src, strlen(src), "eval.js", flags);
}

static double num_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

static bool exc_name_is(embi_js_runtime *rt, const char *want)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
    if (!embi_js_is_string(n)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(n), &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

/* Native functions used by the tests. */

static embi_js_value twice_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (argc < 1) {
        return embi_js_number(0);
    }
    double d = 0;
    if (!embi_js_to_number(r->rt, argv[0], &d)) {
        return EMBI_JS_EXCEPTION;
    }
    return embi_js_number(d * 2);
}

static embi_js_value boom_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_throw_type_error(r->rt, "boom");
}

/* Calls its first argument from native code: a re-entry into the interpreter. */
static embi_js_value call_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (argc < 1) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_js_call(r->rt, argv[0], embi_js_from_object(r->global), 0, nullptr);
}

EMB_TEST(eval_native_functions_and_reentry)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "twice", twice_fn, 1, 0);
    embi_js_define_method(e.r, e.r->global, "boom", boom_fn, 0, 0);
    embi_js_define_method(e.r, e.r->global, "call", call_fn, 1, 0);

    embi_js_value v = run(e, "twice(21)", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 42);

    v = run(e, "var x = 3; call(function() { return x + 1; })", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 4);

    v = run(e, "var m = ''; try { boom(); } catch (err) { m = err.name; } m", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_string(v));
    env_free(e);
}

EMB_TEST(eval_globals_persist_between_scripts)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run(e, "var z = 9;", 0) != EMBI_JS_EXCEPTION);
    embi_js_value v = run(e, "z * 2", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 18);
    env_free(e);
}

EMB_TEST(eval_syntax_error_is_a_syntax_error_exception)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run(e, "var = ;", 0) == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
    embi_js_exception_clear(e.rt);
    env_free(e);
}

EMB_TEST(eval_unary_before_exponent_is_a_syntax_error)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run(e, "-2 ** 2", 0) == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
    embi_js_exception_clear(e.rt);
    embi_js_value v = run(e, "(-2) ** 2", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION && embi_js_is_number(v) && num_of(v) == 4);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

EMB_TEST(eval_strict_flag_rejects_undeclared_assignment)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run(e, "undeclaredThing = 1;", EMBI_JS_EVAL_STRICT) == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "ReferenceError"));
    embi_js_exception_clear(e.rt);
    /* Sloppy mode creates the global. */
    EMB_CHECK(run(e, "undeclaredThing = 2;", 0) != EMBI_JS_EXCEPTION);
    env_free(e);
}

EMB_TEST(eval_depth_limit_throws_range_error)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_set_limits(e.rt, 50, 768u * 1024u);
    const char *src = "function d(n) { return n == 0 ? 0 : 1 + d(n - 1); }";
    EMB_CHECK(run(e, src, 0) != EMBI_JS_EXCEPTION);
    embi_js_value v = run(e, "d(40)", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 40);
    EMB_CHECK(run(e, "d(200)", 0) == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "RangeError"));
    embi_js_exception_clear(e.rt);
    env_free(e);
}

typedef struct interrupt_state {
    int calls;
    int stop_after;
} interrupt_state;

static bool interrupt_after(embi_js_runtime *rt, void *user)
{
    (void)rt;
    interrupt_state *st = user;
    st->calls++;
    return st->calls > st->stop_after;
}

EMB_TEST(eval_interrupt_cannot_be_caught)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    interrupt_state st = {0, 5};
    embi_js_set_interrupt(e.rt, interrupt_after, &st);
    embi_js_value v = run(e, "var caught = 0; try { while (true) {} } catch (x) { caught = 1; }", 0);
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    EMB_CHECK(st.calls > 5);
    embi_js_exception_clear(e.rt);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    v = run(e, "1 + 1", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    env_free(e);
}

/* Stops exactly once: true only at call number stop_after, so the hook can be used again later. */
static bool interrupt_once(embi_js_runtime *rt, void *user)
{
    (void)rt;
    interrupt_state *st = user;
    st->calls++;
    return st->calls == st->stop_after;
}

EMB_TEST(eval_interrupt_do_while_is_checked)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    interrupt_state st = {0, 1000};
    embi_js_set_interrupt(e.rt, interrupt_after, &st);
    EMB_CHECK(run(e, "do {} while (true)", 0) == EMBI_JS_EXCEPTION);
    EMB_CHECK(st.calls > 1000);
    embi_js_exception_clear(e.rt);

    /* The condition is true for a long time, but the back-edge still stops the loop. */
    st.calls = 0;
    EMB_CHECK(run(e, "var i = 0; do { i++; } while (i < 1e9); i", 0) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_value v = run(e, "i", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) > 0 && num_of(v) < 1e9);
    env_free(e);
}

EMB_TEST(eval_interrupt_skips_catch_and_finally)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    interrupt_state st = {0, 5};
    embi_js_set_interrupt(e.rt, interrupt_after, &st);
    EMB_CHECK(run(e, "var fin = 0; try { while (true) {} } catch (x) { fin = 2; }"
                     " finally { fin = 1; }",
                  0) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_value v = run(e, "fin", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 0);
    env_free(e);
}

EMB_TEST(eval_interrupt_through_embedder_call_leaves_handlers_working)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run(e, "function spin() { while (true) {} }"
                     " function guard() { try { throw 5; } catch (x) { return x + 1; } }",
                  0) != EMBI_JS_EXCEPTION);
    embi_js_value spin = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "spin"));
    embi_js_value guard = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "guard"));
    EMB_REQUIRE(embi_js_is_object(spin) && embi_js_is_object(guard));

    interrupt_state st = {0, 3};
    embi_js_set_interrupt(e.rt, interrupt_once, &st);
    /* The interrupt unwinds the closure_call; the stop must not outlive that call. */
    EMB_CHECK(embi_js_call(e.rt, spin, EMBI_JS_UNDEFINED, 0, nullptr) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);

    embi_js_value v = embi_js_call(e.rt, guard, EMBI_JS_UNDEFINED, 0, nullptr);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 6);
    env_free(e);
}

EMB_TEST(eval_interrupt_across_native_frame_is_not_caught)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "call", call_fn, 1, 0);
    EMB_CHECK(run(e, "function spin() { while (true) {} } var caught = 0;", 0) !=
              EMBI_JS_EXCEPTION);
    interrupt_state st = {0, 3};
    embi_js_set_interrupt(e.rt, interrupt_once, &st);
    EMB_CHECK(run(e, "try { call(spin); } catch (x) { caught = 1; }", 0) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_value v = run(e, "caught", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 0);
    env_free(e);
}

EMB_TEST(eval_interrupted_vm_runs_handlers_again)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    interrupt_state st = {0, 4};
    embi_js_set_interrupt(e.rt, interrupt_once, &st);
    EMB_CHECK(run(e, "while (true) {}", 0) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);
    embi_js_value v = run(e, "var r = 0; try { throw 1; } catch (x) { r = 7; } r", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 7);
    env_free(e);
}

EMB_TEST(eval_native_stack_limit_throws_range_error)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "call", call_fn, 1, 0);
    embi_js_set_limits(e.rt, 100000, 64u * 1024u);
    EMB_CHECK(run(e, "function r() { return call(r); } r()", 0) == EMBI_JS_EXCEPTION);
    EMB_CHECK(exc_name_is(e.rt, "RangeError"));
    embi_js_exception_clear(e.rt);
    embi_js_set_limits(e.rt, 10000, 768u * 1024u);
    env_free(e);
}

/* An error raised by re-entered JS reports the position of the native call in the frame below. */
EMB_TEST(eval_stack_position_of_reentered_caller)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "call", call_fn, 1, 0);
    embi_js_value v = run(e,
                          "function outer() {\n"
                          "  return call(function inner() {\n"
                          "    null.x;\n"
                          "  });\n"
                          "}\n"
                          "outer();",
                          0);
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    embi_js_value ex = embi_js_exception_get(e.rt);
    EMB_REQUIRE(embi_js_is_object(ex));
    embi_js_value st = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "stack"));
    EMB_REQUIRE(embi_js_is_string(st));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(st), &sb);
    EMB_CHECK(strstr(embi_sb_cstr(&sb), "outer (eval.js:2:") != nullptr);
    EMB_CHECK(strstr(embi_sb_cstr(&sb), "inner (eval.js:3:") != nullptr);
    embi_sb_free(&sb);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

/* A native that evaluates source with embi_js_eval, as an indirect eval would. */
static embi_js_value ev_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                           const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    if (argc < 1 || !embi_js_is_string(argv[0])) {
        return EMBI_JS_UNDEFINED;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(argv[0]), &sb);
    embi_js_value v = embi_js_eval(r, embi_sb_cstr(&sb), sb.len, "inner.js", 0);
    embi_sb_free(&sb);
    return v;
}

EMB_TEST(eval_interrupt_through_nested_eval_is_not_caught)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "ev", ev_fn, 1, 0);
    EMB_CHECK(run(e, "var caught = 0, after = 0;", 0) != EMBI_JS_EXCEPTION);
    interrupt_state st = {0, 3};
    embi_js_set_interrupt(e.rt, interrupt_once, &st);
    embi_js_value v = run(e, "try { ev('for(;;);'); } catch (x) { caught = 1; } after = 1;", 0);
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(e.rt);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    v = run(e, "caught * 10 + after", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 0);
    /* Handlers work again once the outermost entry returned. */
    v = run(e, "var r = 0; try { throw 1; } catch (x) { r = 5; } r", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 5);
    env_free(e);
}

/* A getter that throws is called from an op of the frame below: that frame reports the op. */
EMB_TEST(eval_stack_position_of_accessor_reentry)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_value v = run(e,
                          "var o = { get g() { null.x; } };\n"
                          "function outer() {\n"
                          "  var a = 1;\n"
                          "  return o.g;\n"
                          "}\n"
                          "outer();",
                          0);
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    embi_js_value ex = embi_js_exception_get(e.rt);
    EMB_REQUIRE(embi_js_is_object(ex));
    embi_js_value st = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "stack"));
    EMB_REQUIRE(embi_js_is_string(st));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(st), &sb);
    EMB_CHECK(strstr(embi_sb_cstr(&sb), "outer (eval.js:4:") != nullptr);
    embi_sb_free(&sb);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

/* GC stress: a collection runs at nearly every allocation (see embi_js_alloc_cell), so every
 * value the script keeps alive must be found by the marker through frames, boxes and objects. */
EMB_TEST(eval_gc_stress_keeps_live_values)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_set_gc_params(e.rt, 0, 0.0);

    const char *closures =
        "var acc = 0;"
        "function mk(i) { return function() { return i; }; }"
        "for (var i = 0; i < 2000; i++) { acc += mk(i)(); }"
        "acc";
    embi_js_value v = run(e, closures, 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 1999000);

    v = run(e, "var o = {}; for (var j = 0; j < 500; j++) { o.k = j; } o.k", 0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 499);

    v = run(e, "var c = 0; for (var q = 0; q < 300; q++) { try { throw q; } catch (x) { c += 1; } } c",
            0);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 300);

    embi_js_mem_stats st;
    embi_js_get_mem_stats(e.rt, &st);
    EMB_CHECK(st.gc_count > 100);
    env_free(e);
}

EMB_TEST_MAIN()
