#include "emb_test.h"

#include "base/base.h"
#include "js/compiler.h"
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

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
}

/* The string value of a property of the pending exception, or false when there is none. */
static bool exc_prop_has(embi_js_runtime *rt, const char *prop, const char *want)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, prop));
    if (!embi_js_is_string(m)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(m), &sb);
    bool ok = strstr(embi_sb_cstr(&sb), want) != nullptr;
    embi_sb_free(&sb);
    return ok;
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

#define CHECK_THROWS(e_, src_, name_)                                                          \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ == EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(exc_name_is((e_).rt, (name_)));                                              \
        embi_js_exception_clear((e_).rt);                                                      \
    } while (0)

/* A SyntaxError whose message contains msg, so that the error is the one expected. */
#define CHECK_SYNTAX(e_, src_, msg_)                                                           \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ == EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(exc_name_is((e_).rt, "SyntaxError"));                                        \
        EMB_CHECK(exc_prop_has((e_).rt, "message", (msg_)));                                   \
        embi_js_exception_clear((e_).rt);                                                      \
    } while (0)

static void note_src(const char *src)
{
    SDL_Log("    in: %s", src);
}

/* The source parses. It may still throw at run time (its names are not declared here), but never
 * with a SyntaxError. */
#define CHECK_PARSES(e_, src_)                                                                 \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        bool ok_ = v_ != EMBI_JS_EXCEPTION || !exc_name_is((e_).rt, "SyntaxError");            \
        if (!ok_) {                                                                            \
            note_src(src_);                                                                    \
        }                                                                                      \
        EMB_CHECK(ok_);                                                                        \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

#define CHECK_NOT_YET(e_, src_, feature_)                                                      \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ == EMBI_JS_EXCEPTION);                                                    \
        EMB_CHECK(exc_name_is((e_).rt, "SyntaxError"));                                        \
        EMB_CHECK(exc_prop_has((e_).rt, "message", "not yet supported: " feature_));           \
        embi_js_exception_clear((e_).rt);                                                      \
    } while (0)

#define CHECK_OK(e_, src_)                                                                     \
    do {                                                                                       \
        embi_js_value v_ = run((e_), (src_));                                                  \
        EMB_CHECK(v_ != EMBI_JS_EXCEPTION);                                                    \
        if (v_ == EMBI_JS_EXCEPTION) {                                                         \
            embi_js_exception_clear((e_).rt);                                                  \
        }                                                                                      \
    } while (0)

#define OCTAL_LIT "Octal literals and decimals with leading zeros are not allowed in strict mode"
#define OCTAL_ESC "Octal escape sequences are not allowed in strict mode"
#define EVAL_ARGS "Unexpected eval or arguments in strict mode"
#define STRICT_WORD "Unexpected"

/* Early errors of strict code (ES5 11.13.1, 12.2.1, 12.7, 13.1, 11.1.5). Each message is checked
 * as a substring, so that a SyntaxError for another reason fails the case. */
EMB_TEST(es5_strict_early_errors)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_SYNTAX(e, "'use strict'; var eval = 1;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; var arguments = 1;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; arguments = 1;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; eval = 5;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; eval++;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; --arguments;", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; var x; delete x;", "Delete of an unqualified identifier");
    CHECK_SYNTAX(e, "'use strict'; var x; delete (x);", "Delete of an unqualified identifier");
    CHECK_SYNTAX(e, "'use strict'; function f(a, a) {}", "Duplicate parameter name");
    CHECK_SYNTAX(e, "function f(eval) { 'use strict'; }", EVAL_ARGS);
    CHECK_SYNTAX(e, "function f(a, a) { 'use strict'; }", "Duplicate parameter name");
    CHECK_SYNTAX(e, "'use strict'; function eval() {}", EVAL_ARGS);
    /* ES2015 allows duplicate property names in object literals, in strict code too. */
    CHECK_OK(e, "'use strict'; var o = {a: 1, a: 2};");
    CHECK_SYNTAX(e, "'use strict'; with ({}) {}", "Strict mode code may not include a with statement");
    CHECK_SYNTAX(e, "'use strict'; if (true) function q() {}", "Function declarations are not allowed");
    CHECK_SYNTAX(e, "'use strict'; 010", OCTAL_LIT);
    CHECK_SYNTAX(e, "'use strict'; '\\01'", OCTAL_ESC);
    CHECK_SYNTAX(e, "'use strict'; (function eval() {})", EVAL_ARGS);
    /* Octal literals and escapes in property names. */
    CHECK_SYNTAX(e, "'use strict'; var o = { 010: 1 }", OCTAL_LIT);
    CHECK_SYNTAX(e, "'use strict'; ({ \"\\01\": 1 })", OCTAL_ESC);
    CHECK_SYNTAX(e, "'use strict'; ({ get 010() {} })", OCTAL_LIT);
    CHECK_SYNTAX(e, "'use strict'; ({ '\\7': 1 })", OCTAL_ESC);
    /* A directive prologue with an octal escape ahead of 'use strict' (the retroactive case). */
    CHECK_SYNTAX(e, "function r() { '\\01'; 'use strict'; }", OCTAL_ESC);
    CHECK_SYNTAX(e, "function r2(a) { 'x\\01'; 'use strict'; return a }", OCTAL_ESC);
    /* Reserved words of strict code. */
    CHECK_SYNTAX(e, "'use strict'; var implements = 1;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var interface;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var package;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var private;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var protected;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var public;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var static;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var yield;", STRICT_WORD);
    CHECK_SYNTAX(e, "'use strict'; var let = 1;", STRICT_WORD);
    CHECK_SYNTAX(e, "function rw(implements) { 'use strict'; }", STRICT_WORD);
    /* catch parameters and setter parameters follow the same rule. */
    CHECK_SYNTAX(e, "'use strict'; try {} catch (eval) {}", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; ({ set x(eval) {} })", EVAL_ARGS);
    CHECK_SYNTAX(e, "'use strict'; ({ set x(arguments) {} })", EVAL_ARGS);
    /* Accessor and data properties with one name: ES2015 allows every combination (J2a). */
    CHECK_OK(e, "({ get x() {}, get x() {} })");
    CHECK_OK(e, "({ set x(v) {}, set x(w) {} })");
    CHECK_OK(e, "({ get x() {}, x: 1 })");
    CHECK_OK(e, "({ x: 1, get x() {} })");
    CHECK_OK(e, "({ set x(v) {}, x: 1 })");
    CHECK_OK(e, "'use strict'; ({ get x() {}, x: 1 })");
    /* The forms that were always legal. */
    CHECK_OK(e, "({ get x() {}, set x(v) {} })");
    CHECK_OK(e, "({ set x(v) {}, get x() {} })");
    CHECK_OK(e, "({ a: 1, a: 2 })");
    CHECK_OK(e, "var eval = 1; eval");
    CHECK_OK(e, "var o = {eval: 1, arguments: 2, get: 3, set: 4};");
    CHECK_OK(e, "function f(a, b) { return a + b }");
    CHECK_OK(e, "var impl = 1, let2 = 2;");
    /* Unlabelled continue and break outside any loop, and labels that do not exist. */
    CHECK_SYNTAX(e, "continue;", "Illegal continue statement");
    CHECK_SYNTAX(e, "break;", "Illegal break statement");
    CHECK_SYNTAX(e, "lbl: { continue lbl; }", "Illegal continue statement");
    CHECK_SYNTAX(e, "break nowhere;", "Undefined label");
    env_free(e);
}

/* Early errors in sloppy code, for the forms that differ by mode. */
EMB_TEST(es5_sloppy_early_errors)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_OK(e, "var x = 010; x");
    CHECK_OK(e, "var y = '\\01'; y");
    CHECK_OK(e, "var o = {010: 1, '\\01': 2}");
    CHECK_OK(e, "function sl(eval, arguments) { return eval + arguments } sl(1, 2)");
    CHECK_OK(e, "with ({}) {}");
    CHECK_OK(e, "var z2; delete z2");
    CHECK_OK(e, "if (true) function q3() {}");
    CHECK_SYNTAX(e, "while (false) function q4() {}", "Function declarations are not allowed");
    CHECK_SYNTAX(e, "for (;false;) function q5() {}", "Function declarations are not allowed");
    CHECK_SYNTAX(e, "do function q6() {} while (false)", "Function declarations are not allowed");
    CHECK_SYNTAX(e, "with ({}) function q7() {}", "Function declarations are not allowed");
    env_free(e);
}

/* Not-yet-supported ES2015+ syntax is a SyntaxError that names the feature. */
EMB_TEST(es5_not_yet_recognizer)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_OK(e, "let la1 = 1;");
    CHECK_OK(e, "const la2 = 1;");
    CHECK_OK(e, "function g(a = 1) {}");
    CHECK_OK(e, "function g2(...a) {}");
    CHECK_PARSES(e, "class C {}");
    CHECK_PARSES(e, "var c = class {};");
    CHECK_PARSES(e, "var f = () => 1;");
    CHECK_PARSES(e, "var f2 = (a) => 1;");
    CHECK_PARSES(e, "f(...[1]);");
    CHECK_PARSES(e, "f(1,);");
    CHECK_PARSES(e, "var a = [...b];");
    CHECK_PARSES(e, "var o = {...b};");
    CHECK_PARSES(e, "var o2 = {[k]: 1};");
    CHECK_PARSES(e, "var o3 = {a};");
    CHECK_PARSES(e, "var o4 = {m() {}};");
    CHECK_PARSES(e, "function* g() {}");
    CHECK_PARSES(e, "var o5 = {*m() {}};");
    CHECK_PARSES(e, "for (var x of [1]) {}");
    CHECK_PARSES(e, "var n = 1_000;");
    CHECK_PARSES(e, "var n2 = 0b101;");
    CHECK_PARSES(e, "var n3 = 0o7;");
    CHECK_PARSES(e, "var {a2} = {};");
    CHECK_PARSES(e, "var [a3] = [];");
    CHECK_PARSES(e, "var a4 = b ?? c;");
    CHECK_PARSES(e, "var a5 = b?.c;");
    CHECK_PARSES(e, "var a6 = `x`;");
    CHECK_PARSES(e, "var a7 = /x/g;");
    CHECK_PARSES(e, "var a8 = 10n;");
    CHECK_PARSES(e, "var a9 = 1; a9 &&= 2;");
    CHECK_PARSES(e, "var a10 = 1; a10 ||= 2;");
    CHECK_PARSES(e, "var a11; a11 ?\?= 2;");
    CHECK_PARSES(e, "function nt() { return new.target }");
    CHECK_PARSES(e, "async function af() {}");
    {
        embi_js_value sv_ = run(e, "super.x;");
        EMB_CHECK(sv_ == EMBI_JS_EXCEPTION);
        EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
        embi_js_exception_clear(e.rt);
    }
    CHECK_PARSES(e, "import('x');");
    CHECK_SYNTAX(e, "import.meta;", "Cannot use 'import.meta' outside a module");
    {
        embi_js_value iv_ = run(e, "import x from 'x';");
        EMB_CHECK(iv_ == EMBI_JS_EXCEPTION);
        EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
        EMB_CHECK(exc_prop_has(e.rt, "message", "Cannot use import statement outside a module"));
        embi_js_exception_clear(e.rt);
    }
    {
        embi_js_value ev_ = run(e, "export default 1;");
        EMB_CHECK(ev_ == EMBI_JS_EXCEPTION);
        EMB_CHECK(exc_name_is(e.rt, "SyntaxError"));
        EMB_CHECK(exc_prop_has(e.rt, "message", "Unexpected token 'export'"));
        embi_js_exception_clear(e.rt);
    }
    CHECK_PARSES(e, "class D { #p = 1 }");
    CHECK_PARSES(e, "var o6 = {get [k]() {}};");
    env_free(e);
}

/* Failures in the lexer and parser report a SyntaxError and never a crash. */
EMB_TEST(es5_syntax_error_forms)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    CHECK_THROWS(e, "var 1a;", "SyntaxError");
    CHECK_THROWS(e, "'unterminated", "SyntaxError");
    CHECK_THROWS(e, "/* unterminated", "SyntaxError");
    CHECK_THROWS(e, "a = {", "SyntaxError");
    CHECK_THROWS(e, "function (){}", "SyntaxError");
    CHECK_THROWS(e, "if (", "SyntaxError");
    CHECK_THROWS(e, "x = 1 +", "SyntaxError");
    CHECK_THROWS(e, "1 = 2", "SyntaxError");
    CHECK_THROWS(e, "a++ = 1", "SyntaxError");
    env_free(e);
}

EMB_TEST_MAIN()
