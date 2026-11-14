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

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
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

static bool exc_name_is(env e, const char *want)
{
    embi_js_runtime *rt = e.rt;
    embi_js_value ex = embi_js_exception_get(rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
        ok = embi_js_is_string(n) && text_is(rt, n, want);
    }
    embi_js_exception_clear(rt);
    return ok;
}

static bool check_text(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw (wanted \"%s\"): %s", want, src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = text_is(e.rt, v, want);
    if (!ok) {
        SDL_Log("    js: %s", src);
    }
    return ok;
}

static bool check_throws(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    js did not throw (wanted %s): %s", want, src);
        return false;
    }
    bool ok = exc_name_is(e, want);
    if (!ok) {
        SDL_Log("    js: %s", src);
    }
    return ok;
}

#define T_TEXT(src_, want_) EMB_CHECK(check_text(e, (src_), (want_)))
#define T_THROWS(src_, want_) EMB_CHECK(check_throws(e, (src_), (want_)))

/* ------------------------------------------------------------------ Function and its prototype */

EMB_TEST(function_ctor_and_prototype_properties)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("typeof Function + Function.length + Function.name", "function1Function");
    T_TEXT("typeof Function.prototype", "function");
    T_TEXT("Function.prototype.length + ':' + Function.prototype.name", "0:");
    T_TEXT("Function.prototype.constructor === Function", "true");
    T_TEXT("Object.getPrototypeOf(Function.prototype) === Object.prototype", "true");
    T_TEXT("Function.prototype()", "undefined");
    T_TEXT("var ff = function () {}; Object.getPrototypeOf(ff) === Function.prototype", "true");
    /* prototype on the constructor is fixed, and the prototype's constructor is hidden. */
    T_TEXT("var pd = Object.getOwnPropertyDescriptor(Function, 'prototype');"
           "'' + pd.writable + pd.enumerable + pd.configurable",
           "falsefalsefalse");
    T_TEXT("var cd = Object.getOwnPropertyDescriptor(Function.prototype, 'constructor');"
           "'' + cd.writable + cd.enumerable + cd.configurable",
           "truefalsetrue");
    /* The methods: length, name and attributes. */
    T_TEXT("var fs = {apply: 2, bind: 1, call: 1, toString: 0};"
           "var bad = 0;"
           "for (var k in fs) {"
           "  var f = Function.prototype[k];"
           "  var d = Object.getOwnPropertyDescriptor(Function.prototype, k);"
           "  if (typeof f !== 'function' || f.length !== fs[k] || f.name !== k ||"
           "      d.enumerable || !d.writable || !d.configurable) bad++;"
           "}"
           "'' + bad",
           "0");
    env_free(e);
}

/* ------------------------------------------------------------------ call, apply, bind */

EMB_TEST(function_call_passes_this_and_arguments)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("function f(a, b) { return this.x + a + b; } f.call({x: 1}, 2, 3) + ''", "6");
    T_TEXT("function g() { return arguments.length; } g.call(null) + ''", "0");
    T_TEXT("function h() { return this === undefined; } h.call(undefined) + ''", "false");
    T_TEXT("function s() { 'use strict'; return this; } s.call(7) + ''", "7");
    T_TEXT("function s2() { 'use strict'; return this === undefined; } s2.call() + ''", "true");
    T_THROWS("Function.prototype.call.call({}, null)", "TypeError");
    T_THROWS("Function.prototype.call.call(1)", "TypeError");
    env_free(e);
}

EMB_TEST(function_apply_argument_forms)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("function f(a, b) { return this.x + a + b; } f.apply({x: 1}, [2, 3]) + ''", "6");
    T_TEXT("function g() { return arguments.length; } g.apply(null) + ''", "0");
    T_TEXT("function g2() { return arguments.length; } g2.apply(null, null) + ''", "0");
    T_TEXT("function g3() { return arguments.length; } g3.apply(null, undefined) + ''", "0");
    /* Any object with a length is an array-like. */
    T_TEXT("function p(a, b) { return a + b; } p.apply(null, {length: 2, 0: 'x', 1: 'y'})",
           "xy");
    T_THROWS("function q() {} q.apply(null, 1)", "TypeError");
    T_THROWS("function q2() {} q2.apply(null, 'abc')", "TypeError");
    T_THROWS("Function.prototype.apply.call({}, null, [])", "TypeError");
    env_free(e);
}

/* A native callee that forces a collection, then reads its arguments. The arguments come from an
 * array-like whose getters return fresh objects, so the only references to them are in the list
 * that apply builds. The result is true when every argument survived with its value. */
static embi_js_value gc_probe_fn(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_gc(rt);
    int bad = argc != 2000;
    for (int i = 0; i < argc && !bad; i++) {
        if (!embi_js_is_object(argv[i])) {
            bad = 1;
            break;
        }
        embi_js_value v = embi_js_get(rt, embi_js_as_object(argv[i]), embi_js_key_c(rt, "i"));
        double d = -1.0;
        if (v == EMBI_JS_EXCEPTION || !embi_js_to_number(rt, v, &d) || d != (double)i) {
            bad = 1;
        }
    }
    return embi_js_from_bool(!bad);
}

EMB_TEST(function_apply_keeps_arguments_alive_across_gc)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "probe", gc_probe_fn, 0, 0);
    T_TEXT("var src = {length: 2000};"
           "function mk(i) {"
           "  Object.defineProperty(src, i, {get: function () { return {i: i, pad: [i]}; },"
           "                                 enumerable: true});"
           "}"
           "for (var j = 0; j < 2000; j++) mk(j);"
           "probe.apply(null, src) + ''",
           "true");
    /* Repeat, so that the arguments are rebuilt and collected several times. */
    T_TEXT("var acc = ''; for (var k = 0; k < 4; k++) {"
           "  acc += probe.apply(null, src) + ',';"
           "}"
           "acc",
           "true,true,true,true,");
    env_free(e);
}

EMB_TEST(function_bind_length_name_and_arguments)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("function f(a, b, c) { return this.x + ':' + a + b + c; }"
           "var bf = f.bind({x: 'T'}, 1);"
           "bf(2, 3)",
           "T:123");
    T_TEXT("function f(a, b, c) {} var bf = f.bind(null, 1); bf.length + ''", "2");
    T_TEXT("function f(a, b, c) {} f.bind(null, 1, 2, 3, 4).length + ''", "0");
    T_TEXT("function f(a, b, c) {} f.bind(null).name", "bound f");
    T_TEXT("function f() {} f.bind(null).bind(null).name", "bound bound f");
    /* The variable name is the function's name (the J1e name hint), so the bound name uses it. */
    T_TEXT("var anon = function () {}; anon.bind(null).name", "bound anon");
    T_TEXT("Function.prototype.bind.call(function () {}, null).name", "bound ");
    /* A target without a length or name gives 0 and the empty name. */
    T_TEXT("var nl = function () {}; Object.defineProperty(nl, 'length', {value: 'x'});"
           "nl.bind(null).length + ''",
           "0");
    T_TEXT("var nn = function () {}; Object.defineProperty(nn, 'name', {value: 5});"
           "nn.bind(null).name",
           "bound ");
    /* A bound function can be called with new, and new.target is the bound target. */
    T_TEXT("function C(a, b) { this.s = a + b; } var BC = C.bind(null, 'p'); new BC('q').s",
           "pq");
    T_TEXT("function C2() {} var BC2 = C2.bind(null); (new BC2()) instanceof C2", "true");
    T_THROWS("Function.prototype.bind.call({}, null)", "TypeError");
    T_THROWS("Function.prototype.bind.call(1)", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ restricted properties */

EMB_TEST(function_restricted_caller_and_arguments)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Reading caller or arguments throws on strict, bound and built-in functions; a sloppy plain
     * function reads null when nothing sloppy called it (the web-reality extension, J6a). */
    T_TEXT("String((function () {}).caller) + String((function () {}).arguments)", "nullnull");
    T_THROWS("(function () { 'use strict'; }).caller", "TypeError");
    T_THROWS("Function.prototype.bind.call(function () {}, null).caller", "TypeError");
    T_THROWS("(function () {}).caller = 1", "TypeError");
    /* The accessors are shared, have fixed length and name, and are not enumerable. */
    T_TEXT("var cd = Object.getOwnPropertyDescriptor(Function.prototype, 'caller');"
           "var ad = Object.getOwnPropertyDescriptor(Function.prototype, 'arguments');"
           "'' + (cd.get === ad.get) + (cd.set === cd.get) + cd.enumerable + cd.configurable",
           "truetruefalsetrue");
    T_TEXT("Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get.length + ''", "0");
    T_TEXT("Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get.name + '|'", "|");
    env_free(e);
}

/* ------------------------------------------------------------------ toString */

/* Compiled functions return the exact source text of their span (ES2015, J2a); native functions
 * keep the NativeFunction form. */
EMB_TEST(function_to_string_native_form)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("function foo(a) { return a; } Function.prototype.toString.call(foo)",
           "function foo(a) { return a; }");
    T_TEXT("Function.prototype.toString.call(Object.keys)", "function keys() { [native code] }");
    T_TEXT("var anon = function () {}; anon.toString()", "function () {}");
    T_TEXT("Function.prototype.toString.call(Function.prototype)",
           "function () { [native code] }");
    T_THROWS("Function.prototype.toString.call({})", "TypeError");
    T_THROWS("Function.prototype.toString.call('f')", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ Function constructor */

EMB_TEST(function_ctor_builds_functions_in_global_scope)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Function('a', 'b', 'return a + b')(1, 2) + ''", "3");
    T_TEXT("Function('a,b', 'return b')(1, 2) + ''", "2");
    T_TEXT("Function('a', 'b', 'c', 'return c')(1, 2, 3) + ''", "3");
    T_TEXT("new Function('return 4')() + ''", "4");
    T_TEXT("Function()() === undefined", "true");
    T_TEXT("Function('return this')() === globalThis", "true");
    T_TEXT("Function(\"'use strict'; return this\")() === undefined", "true");
    T_TEXT("Function('return 1').name", "anonymous");
    T_TEXT("Function('a', 'b', '').length + ''", "2");
    /* The new function binds nothing: its name is not a variable. */
    T_TEXT("Function('return 1'); typeof anonymous", "undefined");
    T_TEXT("var gv = 'g'; Function('return gv')()", "g");
    T_TEXT("Function('return typeof Function')() + ''", "function");
    /* A body whose closing brace comes early is a syntax error, not an injection. */
    T_THROWS("Function('}), (function(){', '')", "SyntaxError");
    T_THROWS("Function('a){}; x = 1; (function(b', '')", "SyntaxError");
    T_THROWS("Function('a', 'return a +')", "SyntaxError");
    T_THROWS("Function('a /*', 'return 1')", "SyntaxError");
    T_THROWS("Function('a', '} x = 2; function f(){')", "SyntaxError");
    env_free(e);
}

EMB_TEST(function_ctor_injection_runs_no_code)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("globalThis.hit = 0; typeof hit", "number");
    /* Body forms that close the wrapper early. */
    T_THROWS("Function('}}); globalThis.hit=1; (function(){ function x(){')", "SyntaxError");
    T_THROWS("Function('}); globalThis.hit=1; (function(){')", "SyntaxError");
    T_THROWS("Function('a', '}); globalThis.hit=1; (function(){')", "SyntaxError");
    T_THROWS("Function('}, globalThis.hit=1, function(){')", "SyntaxError");
    T_THROWS("Function('}() || {')", "SyntaxError");
    T_THROWS("Function('}(globalThis.hit=1), function(){')", "SyntaxError");
    /* Parameter forms. */
    T_THROWS("Function('a){}}); globalThis.hit=1; (function(x(', '')", "SyntaxError");
    T_THROWS("Function('a){}), globalThis.hit=1, (function(b', '')", "SyntaxError");
    T_THROWS("Function('a){}); globalThis.hit=1; (function(b', '')", "SyntaxError");
    T_THROWS("Function('a){ return 1 } /*', '*/')", "SyntaxError");
    T_TEXT("globalThis.hit + ''", "0");
    env_free(e);
}

EMB_TEST(function_ctor_converts_arguments_in_order)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var log = ''; var pa = {toString: function () { log += 'p'; return 'a'; }};"
           "var pb = {toString: function () { log += 'b'; return 'return a'; }};"
           "Function(pa, pb); log",
           "pb");
    env_free(e);
}

/* ------------------------------------------------------------------ @@hasInstance (from C) */

EMB_TEST(function_has_instance_symbol_property)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_runtime *rt = e.rt;
    embi_js_value sym = embi_js_wk_sym(rt, EMBI_JS_WK_HAS_INSTANCE);
    embi_js_pdesc d;
    int st = embi_js_obj_get_own_property(rt, embi_js_as_object(e.r->rt->protos[EMBI_JS_PROTO_FUNCTION]),
                                          sym, &d);
    EMB_CHECK(st == EMBI_JS_YES);
    EMB_CHECK((d.attrs & (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE |
                          EMBI_JS_ATTR_CONFIGURABLE)) == 0);
    EMB_CHECK(embi_js_is_callable(d.value));
    /* The default behaviour: a constructor's instances, and nothing else. */
    T_TEXT("function K() {} ((new K()) instanceof K) + ''", "true");
    T_TEXT("(1 instanceof Function) + ''", "false");
    T_TEXT("(function () {} instanceof Function) + ''", "true");
    env_free(e);
}

EMB_TEST_MAIN()
