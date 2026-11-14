/* Error locations with the interpreter's pc sync (J5b §6): the handlers that never store the pc
 * run without it, so a throw from a handler that does (a generic property read, a call, a slow
 * numeric path) must still report the line of the op that threw, after fast ops on earlier
 * lines and across a call and its return. */
#include "es6_util.h"

#include <string.h>

static uint32_t thrown_line(es6_env e, const char *src);

/* True when the text of v contains needle. */
static bool text_has(embi_js_runtime *rt, embi_js_value v, const char *needle)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strstr(embi_sb_cstr(&sb), needle) != nullptr;
    embi_sb_free(&sb);
    return ok;
}

/* Native re-entry: forEach (a native) calls a callback, which throws. The outer frame's line is
 * the forEach call on line 7, so the caught stack must list both line 7 and the throw on line 9. */
EMB_TEST(js_numeric_fast_loc_native_reentry)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var n = 0;\n"                          /* 1 */
                      "n = n + 1;\n"                          /* 2 */
                      "var q = 5 * 7 - 1;\n"                  /* 3 */
                      "var arr = [1, 2];\n"                   /* 4 */
                      "var r = '';\n"                         /* 5 */
                      "try {\n"                               /* 6 */
                      "  arr.forEach(function (x) {\n"        /* 7 */
                      "    n = n * 2 - x + q;\n"              /* 8 */
                      "    if (x === 2) null.boom;\n"         /* 9 */
                      "  });\n"                               /* 10 */
                      "} catch (err) { r = err.stack; }\n"    /* 11 */
                      "r";
    embi_js_value v = es6_run(e, src);
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(text_has(e.rt, v, "test.js:9:"));
    EMB_CHECK(text_has(e.rt, v, "test.js:7:"));
    EMB_CHECK(!text_has(e.rt, v, "test.js:3:"));
    es6_env_free(e);
}

EMB_TEST(js_numeric_fast_loc_native_reentry_uncaught)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var n = 0;\n"                          /* 1 */
                      "n = n + 1;\n"                          /* 2 */
                      "var q = 5 * 7 - 1;\n"                  /* 3 */
                      "var arr = [1, 2];\n"                   /* 4 */
                      "arr.forEach(function (x) {\n"          /* 5 */
                      "  n = n * 2 - x + q;\n"                /* 6 */
                      "  if (x === 2) null.boom;\n"           /* 7 */
                      "});";                                  /* 8 */
    EMB_CHECK_EQ_INT(thrown_line(e, src), 7);
    es6_env_free(e);
}

/* Accessor re-entry: a getter throws after fast ops, read by GET_FIELD on line 7. */
EMB_TEST(js_numeric_fast_loc_accessor_reentry)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var o = { get g() {\n"                 /* 1 */
                      "  var z = 3 * 4;\n"                    /* 2 */
                      "  return null.k;\n"                    /* 3 */
                      "} };\n"                                /* 4 */
                      "var a = 1 + 2;\n"                      /* 5 */
                      "var r = '';\n"                         /* 6 */
                      "try { var v = o.g; } catch (err) { r = err.stack; }\n" /* 7 */
                      "r";
    embi_js_value v = es6_run(e, src);
    EMB_REQUIRE(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(text_has(e.rt, v, "test.js:3:"));
    EMB_CHECK(text_has(e.rt, v, "test.js:7:"));
    es6_env_free(e);
}

/* Interrupt callback (host code run at each call and backward jump). The callback captures the
 * stack of a fresh Error created by a nested eval, which is an entry frame pushed from the host.
 * stop_after > 0 makes the callback interrupt on that invocation. busy blocks the callback while
 * its own eval runs. */
typedef struct loc_cb {
    es6_env env;
    uint32_t calls;
    uint32_t stop_after;
    bool busy;
    char stack[1024];
} loc_cb;

static bool loc_interrupt(embi_js_runtime *rt, void *user)
{
    loc_cb *c = user;
    if (c->busy) {
        return false;
    }
    c->calls++;
    c->busy = true;
    /* stack is set only on a thrown error, so the nested code throws and catches one. */
    static const char cb_src[] = "(function () { try { throw new Error('cb'); } catch (e) {"
                                 " return e.stack; } })()";
    embi_js_value v = embi_js_eval(c->env.r, cb_src, sizeof cb_src - 1, "cb.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        c->stack[0] = '\0';
    } else {
        embi_sb sb = {nullptr, 0, 0};
        embi_js_string *s = embi_js_to_string(rt, v);
        if (s) {
            embi_js_str_append_utf8(s, &sb);
        }
        size_t n = sb.len < sizeof c->stack - 1 ? sb.len : sizeof c->stack - 1;
        memcpy(c->stack, sb.len ? embi_sb_cstr(&sb) : "", n);
        c->stack[n] = '\0';
        embi_sb_free(&sb);
    }
    c->busy = false;
    return c->stop_after != 0 && c->calls == c->stop_after;
}

/* The loop is in run() on line 3. The dispatch stores pc_cur before every op (computed goto in
 * NEXT(), switch fallback at dispatch_top), so an interrupt callback run from a back-edge sees the
 * back-edge's own pc. The callback's nested Error must show run at line 3. Line 6 (the call of
 * run) would appear only if a dispatch path skipped the store; the switch fallback once did. */
static const char loop_src[] = "function run() {\n"           /* 1 */
                               "  var i = 0;\n"                /* 2 */
                               "  while (i < 1000) { i = i + 1; }\n" /* 3 */
                               "  return i;\n"                 /* 4 */
                               "}\n"                           /* 5 */
                               "run();";                       /* 6 */

EMB_TEST(js_numeric_fast_loc_interrupt_stack)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    loc_cb c;
    memset(&c, 0, sizeof c);
    c.env = e;
    embi_js_set_interrupt(e.rt, loc_interrupt, &c);
    embi_js_value v = es6_run(e, loop_src);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(c.calls >= 1000);
    EMB_CHECK(strstr(c.stack, "run (test.js:3:") != nullptr);
    EMB_CHECK(strstr(c.stack, "run (test.js:6:") == nullptr);
    es6_env_free(e);
}

/* The callback interrupts on its 20th invocation, a back-edge in run() on line 3 (the call on line
 * 6 is the first). The interruption throws there, so the reported line is 3 and the captured stack
 * shows run at line 3. */
EMB_TEST(js_numeric_fast_loc_interrupt_throw)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    loc_cb c;
    memset(&c, 0, sizeof c);
    c.env = e;
    c.stop_after = 20;
    embi_js_set_interrupt(e.rt, loc_interrupt, &c);
    embi_js_value v = es6_run(e, loop_src);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    EMB_CHECK(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 3);
    embi_js_exception_clear(e.rt);
    EMB_CHECK(strstr(c.stack, "run (test.js:3:") != nullptr);
    es6_env_free(e);
}

static uint32_t thrown_line(es6_env e, const char *src)
{
    embi_js_value v = es6_run(e, src);
    if (v != EMBI_JS_EXCEPTION) {
        return 0;
    }
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    bool ok = embi_js_error_location(e.rt, &file, &line, &col);
    embi_js_exception_clear(e.rt);
    return ok ? line : 0;
}

EMB_TEST(js_numeric_fast_error_location_property)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var a = 1;\n"
                      "var b = a + 2;\n"
                      "function f() {\n"
                      "  var k = 3 * 4;\n"
                      "  return (null).x;\n"
                      "}\n"
                      "f();";
    EMB_CHECK_EQ_INT(thrown_line(e, src), 5);
    es6_env_free(e);
}

EMB_TEST(js_numeric_fast_error_location_throw)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var a = 1;\n"
                      "var b = a * 3 - 1;\n"
                      "var c = b | 0;\n"
                      "throw new Error('e' + c);";
    EMB_CHECK_EQ_INT(thrown_line(e, src), 4);
    es6_env_free(e);
}

/* A slow numeric path (a valueOf that throws) in a callee, after fast ops in the caller: the
 * throw is in the valueOf on line 6. */
EMB_TEST(js_numeric_fast_error_location_slow_call)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var n = 0;\n"
                      "function bad(o) {\n"
                      "  return 1 + o;\n"
                      "}\n"
                      "for (var i = 0; i < 3; i++) n = n + i * 2;\n"
                      "bad({ valueOf() { throw new RangeError('v') } });";
    EMB_CHECK_EQ_INT(thrown_line(e, src), 6);
    es6_env_free(e);
}

/* A throw caught inside the same frame keeps working after fast ops, and the next error in a
 * different function reports its own line. */
EMB_TEST(js_numeric_fast_error_location_caught)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var r = 0; try { r = 5 - 2; null.q; } catch (x) { r = r * 10 } r", 30);
    const char *src = "var s = 2;\n"
                      "try { s = s + 1; undefined.k } catch (y) { s = s * 2 }\n"
                      "var t = s - 1;\n"
                      "t.u.v;";
    EMB_CHECK_EQ_INT(thrown_line(e, src), 4);
    es6_env_free(e);
}

EMB_TEST_MAIN()
