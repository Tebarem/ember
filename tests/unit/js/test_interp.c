#include "emb_test.h"

#include "js/js.h"

#include <stdio.h>
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

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
}

static double num_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

/* True when the value's ToString equals want. */
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

/* True when the pending exception is an object whose name property equals want. */
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

#define CHECK_NUM(e_, src_, want_)                                                             \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ != EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(embi_js_is_number(v_) && num_of(v_) == (double)(want_));                     \
    } while (0)

#define CHECK_TEXT(e_, src_, want_)                                                            \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ != EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(text_is((e_).rt, v_, (want_)));                                              \
    } while (0)

#define CHECK_BOOL(e_, src_, want_)                                                            \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ != EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(embi_js_is_bool(v_) && embi_js_as_bool(v_) == (bool)(want_));                \
    } while (0)

#define CHECK_THROWS(e_, src_, name_)                                                          \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ == EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(exc_name_is((e_).rt, (name_)));                                              \
        embi_js_exception_clear((e_).rt);                                                      \
    } while (0)

EMB_TEST(interp_arithmetic_and_precedence)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "1 + 2 * 3", 7);
    CHECK_NUM(e, "(1 + 2) * 3", 9);
    CHECK_NUM(e, "7 % 3", 1);
    CHECK_NUM(e, "10 - 4 - 3", 3);
    CHECK_NUM(e, "1 << 4 | 1", 17);
    CHECK_NUM(e, "-8 >> 1", -4);
    CHECK_NUM(e, "2147483647 + 1", 2147483648.0);
    env_free(e);
}

EMB_TEST(interp_strings_and_coercion)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e, "'a' + 1 + 'b'", "a1b");
    CHECK_TEXT(e, "1 + 2 + 'x'", "3x");
    CHECK_NUM(e, "'6' * '7'", 42);
    CHECK_BOOL(e, "'1' == 1", true);
    CHECK_BOOL(e, "null === undefined", false);
    CHECK_TEXT(e, "typeof 'x'", "string");
    CHECK_TEXT(e, "typeof nothere", "undefined");
    env_free(e);
}

EMB_TEST(interp_logical_and_increment)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e, "0 || 'x'", "x");
    CHECK_NUM(e, "1 && 0", 0);
    CHECK_NUM(e, "var i = 5; var j = i++ + ++i; j", 12);
    CHECK_NUM(e, "var n = 0; n += 3; n *= 4; n", 12);
    env_free(e);
}

EMB_TEST(interp_closures_share_mutable_state)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e,
              "function mk() { var n = 0; return function() { n = n + 1; return n; }; }"
              "var c = mk(); c(); c(); c()",
              3);
    CHECK_NUM(e,
              "function pair() { var v = 1; return [function() { return v; },"
              " function(x) { v = x; }]; }"
              "var p = pair(); p[1](9); p[0]()",
              9);
    env_free(e);
}

EMB_TEST(interp_recursion_and_calls)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } fib(15)", 610);
    CHECK_NUM(e, "function f() { return arguments.length; } f(1, 2, 3)", 3);
    CHECK_NUM(e, "function g(a, b) { return a; } g(4)", 4);
    env_free(e);
}

EMB_TEST(interp_loops_and_completion_values)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var s = 0, i = 0; while (true) { i++; if (i > 10) break; if (i % 2) continue;"
                 " s += i; } s",
              30);
    CHECK_NUM(e, "var t = 0; for (var k = 0; k < 5; k++) t += k; t", 10);
    CHECK_NUM(e, "var d = 0; do { d++; } while (d < 4); d", 4);
    CHECK_NUM(e, "1; 2; 3", 3);
    CHECK_NUM(e, "1; try { 2 } finally { 3 }", 2);
    CHECK_NUM(e, "try { throw 0 } catch (e) { 4 } finally { 3 }", 4);
    CHECK_TEXT(e, "var k2 = ''; for (var p in {a: 1, b: 2}) k2 += p; k2", "ab");
    env_free(e);
}

EMB_TEST(interp_labels_and_switch)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e,
               "var r = ''; switch (2) { case 1: r = 'a'; break; case 2: r = 'b';"
               " case 3: r += 'c'; break; default: r = 'd'; } r",
               "bc");
    CHECK_NUM(e,
              "var hits = 0; outer: for (var i = 0; i < 4; i++) {"
              " for (var j = 0; j < 4; j++) { if (j == 2) continue outer; hits++; } } hits",
              8);
    env_free(e);
}

EMB_TEST(interp_try_catch_finally_order)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e,
               "var log = ''; try { log += 't'; throw 1; } catch (x) { log += 'c' + x; }"
               " finally { log += 'f'; } log",
               "tc1f");
    CHECK_TEXT(e, "var r = ''; function f() { try { return 1; } finally { r = 'x'; } } f(); r", "x");
    CHECK_TEXT(e, "var m = ''; try { null.x; } catch (err) { m = err.name; } m", "TypeError");
    env_free(e);
}

EMB_TEST(interp_objects_accessors_and_new)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var o = { v: 4, get: function() { return this.v; } }; o.get()", 4);
    CHECK_NUM(e, "var h = { get g() { return 5; } }; h.g", 5);
    CHECK_NUM(e, "function P(x) { this.x = x; } var p = new P(7); p.x", 7);
    CHECK_BOOL(e, "var q = { a: 1 }; delete q.a; 'a' in q", false);
    env_free(e);
}

EMB_TEST(interp_uncaught_errors_and_names)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_THROWS(e, "nothere + 1", "ReferenceError");
    CHECK_THROWS(e, "function r() { return r(); } r()", "RangeError");
    CHECK_THROWS(e, "throw new TypeError('x')", "TypeError");
    env_free(e);
}

EMB_TEST(interp_strict_function_declarations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Declarations at the top level and in blocks are legal in strict code. */
    CHECK_NUM(e, "'use strict'; function f() { return 1; } f()", 1);
    CHECK_NUM(e, "'use strict'; var r = 0; { function h() { return 5; } r = h(); } r", 5);
    CHECK_NUM(e, "function s() { 'use strict'; function g() { return 4; } return g(); } s()", 4);
    /* A declaration as the direct body of a statement is not. */
    CHECK_THROWS(e, "'use strict'; if (true) function q() {}", "SyntaxError");
    CHECK_THROWS(e, "'use strict'; while (false) function q() {}", "SyntaxError");
    /* Sloppy code allows it, as Annex B does. */
    CHECK_NUM(e, "if (true) function q2() { return 3; } q2()", 3);
    env_free(e);
}

EMB_TEST(interp_try_in_loops_keeps_operand_stack)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Several try statements inside nested for-in loops, then a call with many arguments: the
     * operand stack of each handler entry must be accounted for, or the arguments are clobbered. */
    CHECK_NUM(e,
              "function g(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15,"
              " p16, p17, p18, p19) { return p0 * 1000 + p18 * 10 + p19 - p17 * 0; }"
              "function f() { for (var a in {x: 1}) for (var b in {x: 1}) for (var c in {x: 1})"
              " for (var d in {x: 1}) { try {} catch (e) {} try {} catch (e) {}"
              " try {} catch (e) {} return g(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,"
              " 15, 16, 17, 18, 19); } } f()",
              199);
    CHECK_NUM(e,
              "var n = 0; for (var k in {a: 1}) { switch (k) { case 'a': try { throw 1; }"
              " catch (x) { n += x; } } } n",
              1);
    env_free(e);
}

EMB_TEST(interp_catch_scoped_function_declarations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "try { throw 1; } catch (e) { function g() { return e; } } g()", 1);
    CHECK_NUM(e,
              "function o() { try { throw 2; } catch (e) { function g() { return e + 1; } }"
              " return g(); } o()",
              3);
    /* The declaration is undefined until its block runs. */
    CHECK_TEXT(e, "typeof kk; try { throw 0; } catch (x) { function kk() {} } typeof kk", "function");
    env_free(e);
}

EMB_TEST(interp_labels_chain_to_loops)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var n = 0; a: b: while (n < 3) { n++; continue a; } n", 3);
    CHECK_NUM(e,
              "var c = 0; out: mid: for (var i = 0; i < 3; i++) {"
              " for (var j = 0; j < 3; j++) { if (j == 1) continue out; c++; } } c",
              3);
    CHECK_NUM(e, "var r = 0; x: y: { r = 1; break x; r = 2; } r", 1);
    CHECK_NUM(e, "var t = 0; p: q: for (;;) { t++; break p; } t", 1);
    env_free(e);
}

EMB_TEST(interp_comma_value_is_not_a_reference)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_THROWS(e, "var a = 1, b = 2; (a, b) = 3", "SyntaxError");
    CHECK_TEXT(e, "var b = {f: function() { return this === b ? 'b' : 'other'; }}; (0, b.f)()",
               "other");
    CHECK_TEXT(e, "(b.f)()", "b");
    CHECK_NUM(e, "var v = 0; (v, 5) + 1", 6);
    env_free(e);
}

EMB_TEST(interp_directive_prologue_ends_at_first_statement)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* 'x' + 1 is not a directive, so the 'use strict' after it does nothing. The inner function
     * stays sloppy and sees the global object as this. */
    CHECK_BOOL(e,
               "function f() { 'x' + 1; 'use strict'; return (function() {"
               " return this === undefined; })(); } f()",
               false);
    CHECK_BOOL(e,
               "function g() { 'use strict'; return (function() { return this === undefined; })(); }"
               " g()",
               true);
    env_free(e);
}

EMB_TEST(interp_strict_octal_literals_are_rejected)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_THROWS(e, "'use strict'; 010", "SyntaxError");
    CHECK_THROWS(e, "'use strict'; var z = 09;", "SyntaxError");
    CHECK_NUM(e, "010", 8);
    env_free(e);
}

EMB_TEST(interp_not_a_function_names_the_callee)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e, "var m = ''; try { var q = {}; q.foo(); } catch (err) { m = err.message; } m",
               "q.foo is not a function");
    CHECK_TEXT(e, "var m2 = ''; try { var z = 3; z(); } catch (err) { m2 = err.message; } m2",
               "z is not a function");
    env_free(e);
}

EMB_TEST(interp_named_function_expression_name_is_immutable)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Sloppy: the store is ignored and the name keeps the function. */
    CHECK_TEXT(e, "var fn = function f() { f = 1; return typeof f; }; fn()", "function");
    CHECK_TEXT(e, "var fn3 = function h() { function inner() { h = 5; } inner(); return typeof h; };"
                  " fn3()",
               "function");
    /* Strict: the store throws. */
    CHECK_THROWS(e, "'use strict'; var fn2 = function g() { g = 1; return 0; }; fn2()", "TypeError");
    env_free(e);
}

EMB_TEST(interp_error_locations_follow_the_throw)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A caught throw does not leave a stale location behind: the uncaught one is on line 2. */
    EMB_CHECK(run(e, "try { throw 3; } catch (x) {}\nthrow 3;") == EMBI_JS_EXCEPTION);
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    EMB_REQUIRE(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 2);
    embi_js_exception_clear(e.rt);

    /* Rethrowing a caught error reports the rethrow. */
    EMB_CHECK(run(e, "var k; try { throw new Error('a'); } catch (x) { k = x; }\nthrow k;") ==
              EMBI_JS_EXCEPTION);
    EMB_REQUIRE(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 2);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

EMB_TEST(interp_uncaught_error_has_location)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_value v = run(e, "var a = 1;\nnull.x;");
    EMB_CHECK(v == EMBI_JS_EXCEPTION);
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    EMB_REQUIRE(embi_js_error_location(e.rt, &file, &line, &col));
    EMB_CHECK_EQ_INT(line, 2);
    EMB_REQUIRE(file != nullptr);
    EMB_CHECK_EQ_STR(file, "test.js");
    embi_js_exception_clear(e.rt);
    env_free(e);
}

static bool error_line_is(env e, const char *src, uint32_t want)
{
    if (run(e, src) != EMBI_JS_EXCEPTION) {
        return false;
    }
    const char *file = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    bool ok = embi_js_error_location(e.rt, &file, &line, &col) && line == want;
    embi_js_exception_clear(e.rt);
    return ok;
}

EMB_TEST(interp_error_location_survives_finally)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(error_line_is(e, "try {\n null.x;\n} finally {\n var z = 1;\n}", 2));
    EMB_CHECK(error_line_is(e, "try {\n try {\n null.x;\n } finally {\n var a = 1;\n }\n}"
                               " finally {\n var b = 2;\n}",
                            3));
    EMB_CHECK(error_line_is(e, "function f() {\n try {\n throw 1;\n } finally {\n var q = 1;\n }\n}\n"
                               "f();",
                            3));
    env_free(e);
}

EMB_TEST(interp_for_in_member_targets)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_TEXT(e, "var o = {}; for (o.x in {a:1, b:2}); o.x", "b");
    CHECK_TEXT(e, "var o2 = {}, k = 'kk'; for (o2[k] in {a:1}); o2.kk", "a");
    /* ES5 12.6.4: the target (here its key) is evaluated for every key: p0, p1, p2. */
    CHECK_TEXT(e, "var o3 = {}, n = 0; for (o3['p' + n++] in {a:1, b:2, c:3}); n + o3.p0", "3a");
    CHECK_NUM(e, "var a = [], c = 0; for (a[0] in {x:1, y:2}) { c++; if (a[0] == 'y') break; } c", 2);
    CHECK_NUM(e, "function f() { var o = {}, n = 0; for (o.z in {a:1, b:2}) { n += o.z.length; }"
                 " return n; } f()",
              2);
    CHECK_THROWS(e, "for (1 in {a:1});", "SyntaxError");
    env_free(e);
}

EMB_TEST(interp_read_modify_write_converts_key_once)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_NUM(e, "var s = 0, t = {}, o = {toString: function() { s++; return 'k'; }}; t[o] = 1; s = 0;"
                 " t[o]++; s",
              1);
    CHECK_NUM(e, "var s2 = 0, t2 = {k: 1}, o2 = {toString: function() { s2++; return 'k'; }};"
                 " t2[o2] += 5; s2",
              1);
    CHECK_NUM(e, "var s3 = 0, t3 = {k: 1}, o3 = {toString: function() { s3++; return 'k'; }};"
                 " --t3[o3]; t3[o3]--; s3",
              2);
    CHECK_NUM(e, "var s4 = 0, t4 = {k: 1}, o4 = {toString: function() { s4++; return 'k'; }};"
                 " t4[o4]++; t4.k",
              2);
    env_free(e);
}

EMB_TEST(interp_early_errors_accessors_and_for_in_head)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_THROWS(e, "({ get a(x) { return 1; } })", "SyntaxError");
    CHECK_THROWS(e, "({ set a() {} })", "SyntaxError");
    CHECK_THROWS(e, "({ set a(x, y) {} })", "SyntaxError");
    CHECK_NUM(e, "({ get a() { return 4; }, set a(v) {} }).a", 4);
    CHECK_THROWS(e, "for (var i, j in {});", "SyntaxError");
    CHECK_NUM(e, "var n = 0; for (var i in {a:1, b:2}) n++; n", 2);
    env_free(e);
}

EMB_TEST(interp_directive_followed_by_continuing_token)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* "use strict" + 1 is an expression, not a directive: sloppy code may assign to undeclared. */
    CHECK_NUM(e, "function f() { \"use strict\"\n+ 1; undeclared_g = 3; return undeclared_g; } f()", 3);
    CHECK_THROWS(e, "function g() { \"use strict\"\n undeclared_h = 3; } g()", "ReferenceError");
    CHECK_THROWS(e, "\"use strict\"\n; undeclared_i = 3;", "ReferenceError");
    env_free(e);
}

static void embi_sb_puts(embi_sb *sb, const char *t)
{
    embi_sb_append(sb, (embi_str){t, (uint32_t)strlen(t)});
}

static embi_js_value eval_sb(env e, const embi_sb *sb, const char *name)
{
    return embi_js_eval(e.r, sb->data, sb->len, name, 0);
}

EMB_TEST(interp_parser_limits)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A long else-if chain parses (the native stack check, not a small count, bounds nesting). */
    embi_sb sb = {nullptr, 0, 0};
    embi_sb_puts(&sb, "var v = 1199; var r = 0; if (v == -1) r = -1;");
    for (int i = 0; i < 1500; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, " else if (v == %d) r = %d;", i, i + 1);
        embi_sb_puts(&sb, buf);
    }
    embi_sb_puts(&sb, " r");
    embi_js_value v = eval_sb(e, &sb, "chain.js");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 1200);
    embi_sb_free(&sb);

    /* Many bindings in one scope. */
    embi_sb big = {nullptr, 0, 0};
    for (int i = 0; i < 20000; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "var v%d = %d;", i, i);
        embi_sb_puts(&big, buf);
    }
    embi_sb_puts(&big, " v0 + v19999");
    v = eval_sb(e, &big, "big.js");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 19999);
    embi_sb_free(&big);

    /* A token longer than the initial lexer scratch buffer. */
    embi_sb lng = {nullptr, 0, 0};
    embi_sb_puts(&lng, "var s = '");
    for (int i = 0; i < 5000; i++) {
        embi_sb_puts(&lng, "abcdefghij");
    }
    embi_sb_puts(&lng, "'; s.length");
    v = eval_sb(e, &lng, "long.js");
    EMB_CHECK(v != EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_number(v) && num_of(v) == 50000);
    embi_sb_free(&lng);

    CHECK_THROWS(e, "var f = 1; f(); f(); f();", "TypeError");
    env_free(e);
}

EMB_TEST_MAIN()
