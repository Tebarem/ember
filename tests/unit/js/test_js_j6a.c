/* J6a: the test262 burn-down outside Intl. One test per root cause that the expectations file used
 * to list; each case failed before the fix and states the behavior the spec requires. */
#include "es6_util.h"

/* Escaped private names (`#\u0061` is `#a`) in declarations, accesses and `in` tests. */
EMB_TEST(j6a_private_name_escapes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class C { #\\u0061 = 1; g() { return this.#a } } new C().g()", 1);
    E6_NUM(e, "class D { #a = 2; g() { return this.#\\u{61} } } new D().g()", 2);
    E6_BOOL(e, "class F { #a = 1; static h(o) { return #\\u0061 in o } } F.h(new F()) && !F.h({})", true);
    es6_env_free(e);
}

/* Function.prototype.toString of a built-in conforms to NativeFunction: a name that is not a
 * PropertyName ("get $&") is left out. */
EMB_TEST(j6a_native_function_text)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "Object.getOwnPropertyDescriptor(RegExp, '$&').get.toString().indexOf('$&') < 0", true);
    E6_BOOL(e, "/^function (get )?[A-Za-z_$]*\\(\\) \\{ \\[native code\\] \\}$/.test(Math.max.toString())", true);
    es6_env_free(e);
}

/* The reference of an assignment or update is resolved before the right-hand side runs, so a
 * with object that loses the property meanwhile still receives the store. */
EMB_TEST(j6a_early_reference)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var o1 = { a: 1 }; with (o1) { a = (delete o1.a, 5) } o1.a === 5", true);
    E6_BOOL(e, "var o2 = { a: 1 }; with (o2) { a += (delete o2.a, 5) } o2.a === 6", true);
    E6_BOOL(e, "var g1 = 0; var o3 = { b: 1 }; with (o3) { b = (o3.b = 9, 7) } o3.b === 7 && g1 === 0", true);
    E6_TEXT(e,
            "var log = []; var px = new Proxy({ a: 1 }, {"
            " has(t, k) { log.push('has:' + String(k)); return k in t },"
            " get(t, k) { log.push('get:' + String(k)); return t[k] },"
            " set(t, k, v) { log.push('set:' + String(k)); t[k] = v; return true } });"
            "with (px) { a = (log.push('rhs'), 2) } log.join()",
            "has:a,get:Symbol(Symbol.unscopables),has:log,rhs,has:a,set:a");
    es6_env_free(e);
}

/* Statement-position declarations and labels. */
EMB_TEST(j6a_statement_position)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "if (1) class C {}");
    E6_SYNTAX(e, "while (0) class C {}");
    E6_SYNTAX(e, "a: function* g() {}");
    E6_SYNTAX(e, "if (1) function* g() {}");
    E6_SYNTAX(e, "function* g() { yi\\u0065ld 1 }");
    es6_env_free(e);
}

/* caller / arguments / callee poison pills share the realm's %ThrowTypeError%. */
EMB_TEST(j6a_throw_type_error)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var tte = Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get;"
            "(function () { 'use strict'; return Object.getOwnPropertyDescriptor(arguments, 'callee').get })() === tte",
            true);
    E6_BOOL(e, "Object.getOwnPropertyDescriptor(Function.prototype, 'arguments').set === "
               "Object.getOwnPropertyDescriptor(Function.prototype, 'caller').get", true);
    E6_THROWS(e, "(function () { 'use strict'; return arguments.callee })()", "TypeError");
    es6_env_free(e);
}

/* Error.prototype.stack is an accessor; an own data property shadows it. */
EMB_TEST(j6a_error_stack)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var d = Object.getOwnPropertyDescriptor(Error.prototype, 'stack'); typeof d.get === 'function' && typeof d.set === 'function'", true);
    E6_BOOL(e, "var er = new Error('x'); er.stack = 'mine'; er.stack === 'mine'", true);
    /* Negative cases of the setter. */
    E6_THROWS(e, "new Error('x').stack = 5", "TypeError");
    E6_THROWS(e, "Error.prototype.stack = 'p'", "TypeError");
    E6_THROWS(e, "Object.getOwnPropertyDescriptor(Error.prototype, 'stack').set.call(1, 'a')", "TypeError");
    es6_env_free(e);
}

/* The legacy RegExp static properties. */
EMB_TEST(j6a_regexp_legacy_statics)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "/(a)(b)/.exec('xaby'); RegExp.$1 + RegExp.$2 + RegExp.lastMatch + RegExp.leftContext + RegExp.rightContext",
            "ababxy");
    E6_TEXT(e, "/(a)(b)/.exec('xaby'); RegExp['$&'] + RegExp.lastParen + RegExp.input", "abbxaby");
    E6_THROWS(e, "Object.getOwnPropertyDescriptor(RegExp, '$1').get.call({})", "TypeError");
    E6_THROWS(e, "class R extends RegExp {} new R('a').exec('a'); RegExp.$1", "TypeError");
    /* Assigning RegExp.input changes only [[RegExpInput]]; the match values keep their own string. */
    E6_TEXT(e, "/(b+)/.exec('abbc'); RegExp.input = 'zz'; "
               "RegExp.lastMatch + '|' + RegExp.$1 + '|' + RegExp.leftContext + '|' + RegExp.rightContext + '|' + RegExp.input",
            "bb|bb|a|c|zz");
    E6_TEXT(e, "/(b+)/.exec('abbc'); RegExp.$_ = 'q'; RegExp['$&'] + RegExp.lastParen", "bbbb");
    es6_env_free(e);
}

/* Regexp literals and Function source with lone surrogates (WTF-8 inside the engine). */
EMB_TEST(j6a_lone_surrogates)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "eval('/' + '\\uD800' + '/').test('\\uD800')", true);
    E6_BOOL(e, "new Function('return \"\\uD800\"')().charCodeAt(0) === 0xD800", true);
    E6_BOOL(e, "eval('\"\\uDC00\"').charCodeAt(0) === 0xDC00", true);
    es6_env_free(e);
}

/* Object, Array and completion-value corners. */
EMB_TEST(j6a_misc_semantics)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var a = [1]; a.join = 1; Array.prototype.toString.call(a)", "[object Array]");
    E6_THROWS(e, "Object.freeze(new Uint8Array(1))", "TypeError");
    E6_BOOL(e, "Object.isFrozen(Object.freeze(new Uint8Array(0)))", true);
    E6_THROWS(e, "Object.freeze(new Uint8Array(new ArrayBuffer(0, { maxByteLength: 8 })))", "TypeError");
    E6_THROWS(e, "Object.freeze(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }), 0, 2))", "TypeError");
    E6_THROWS(e, "var lt = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 })); Object.seal(lt); Object.freeze(lt)", "TypeError");
    E6_NUM(e, "eval('1; do { 2; try { 3; break; } finally { 4; } } while (false)')", 3);
    E6_BOOL(e, "var rr; Object.defineProperty(String.prototype, 'zz', { set: function (v) { rr = typeof this }, configurable: true }); 'a'.zz = 1; rr === 'object'", true);
    es6_env_free(e);
}

/* A direct eval call with a spread argument is still a direct eval. */
EMB_TEST(j6a_eval_spread)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "eval(...['1 + 2'])", 3);
    E6_NUM(e, "(function () { var q = 4; return eval(...['q + 1']) })()", 5);
    /* A spread call of something that is not %eval% is an ordinary call. */
    E6_NUM(e, "(function () { var q = 4; var eval = function (s) { return s.length }; return eval(...['abc']) })()", 3);
    E6_NUM(e, "(function () { var q = 4; return (0, eval)(...['typeof q === \"undefined\" ? 1 : 2']) })()", 1);
    es6_env_free(e);
}

/* Properties of strings (v flag): emoji sequences. */
EMB_TEST(j6a_property_of_strings)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "/^\\p{RGI_Emoji}$/v.test('\\u{1F600}')", true);
    E6_BOOL(e, "/^\\p{Emoji_Keycap_Sequence}$/v.test('1\\uFE0F\\u20E3')", true);
    E6_BOOL(e, "!/^\\p{Emoji_Keycap_Sequence}$/v.test('1\\u20E3')", true);
    E6_BOOL(e, "/^\\p{RGI_Emoji_Flag_Sequence}$/v.test('\\u{1F1FA}\\u{1F1F8}')", true);
    E6_BOOL(e, "/^\\p{RGI_Emoji_ZWJ_Sequence}$/v.test('\\u{1F468}\\u200D\\u{1F469}\\u200D\\u{1F467}')", true);
    E6_BOOL(e, "/^\\p{RGI_Emoji_Modifier_Sequence}$/v.test('\\u{1F44B}\\u{1F3FD}')", true);
    E6_BOOL(e, "/^[\\p{Basic_Emoji}--\\q{\\u{1F600}}]$/v.test('\\u{1F600}') === false", true);
    E6_BOOL(e, "/^\\p{RGI_Emoji}+$/v.test('\\u{1F600}1\\uFE0F\\u20E3')", true);
    E6_SYNTAX(e, "new RegExp('\\\\P{RGI_Emoji}', 'v')");
    E6_SYNTAX(e, "new RegExp('[^\\\\p{RGI_Emoji}]', 'v')");
    E6_SYNTAX(e, "new RegExp('\\\\p{RGI_Emoji}', 'u')");
    es6_env_free(e);
}

/* GetSuperBase comes before ToPropertyKey, also for a compound assignment: a key whose
 * conversion changes the home object's prototype does not change the base that was fixed. */
EMB_TEST(j6a_super_base_order)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e,
           "var A1 = { x: 10 }, A2 = { x: 20 };"
           "var o1 = { __proto__: A1, m() { return super[k1] } };"
           "var k1 = { toString() { Object.setPrototypeOf(o1, A2); return 'x' } };"
           "o1.m()",
           10);
    E6_NUM(e,
           "var B1 = { x: 10 }, B2 = { x: 20 };"
           "var o2 = { __proto__: B1, m() { return super[k2] += 1 } };"
           "var k2 = { toString() { Object.setPrototypeOf(o2, B2); return 'x' } };"
           "o2.m()",
           11);
    es6_env_free(e);
}

/* A destructuring assignment resolves its target reference before it takes the value. */
EMB_TEST(j6a_destructuring_early_reference)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var od = { x: 0 }; var itr = {};"
            "itr[Symbol.iterator] = function () { return { next() { delete od.x; return { value: 5, done: false } },"
            " return() { return {} } } };"
            "with (od) { [x] = itr } od.x === 5",
            true);
    es6_env_free(e);
}

/* GetBindingValue and SetMutableBinding of an object environment record test [[HasProperty]] a
 * second time. */
EMB_TEST(j6a_object_environment_has)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e,
            "var lg = []; var pg = new Proxy({ a: 1 }, {"
            " has(t, k) { lg.push('has:' + String(k)); return k in t },"
            " get(t, k) { lg.push('get:' + String(k)); return t[k] } });"
            "with (pg) { a } lg.join()",
            "has:a,get:Symbol(Symbol.unscopables),has:a,get:a");
    es6_env_free(e);
}

/* GetFunctionRealm of a revoked proxy new.target throws a TypeError. */
EMB_TEST(j6a_construct_revoked_proxy)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e,
              "var rv = Proxy.revocable(function () {}, { get(t, k) {"
              " if (k === 'prototype') { rv.revoke(); return undefined } return t[k] } });"
              "Reflect.construct(function () {}, [], rv.proxy)",
              "TypeError");
    es6_env_free(e);
}

/* The source text of a static method starts at its name, not at `static`. */
EMB_TEST(j6a_static_method_text)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "class SM { static m() {} } SM.m.toString()", "m() {}");
    E6_TEXT(e, "class SG { static get g() { return 1 } } Object.getOwnPropertyDescriptor(SG, 'g').get.toString()",
            "get g() { return 1 }");
    es6_env_free(e);
}

/* Parsing corners: `for (async of => {};;)`, yield in arrow parameters, escaped labels. */
EMB_TEST(j6a_parse_corners)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "eval('for (async of => {}; false;);'); true", true);
    E6_SYNTAX(e, "for (async of []) ;");
    E6_SYNTAX(e, "function* g() { (x = yield) => 1 }");
    E6_SYNTAX(e, "function* g2() { (yield) => 1 }");
    E6_NUM(e, "yi\u0065ld: 5; 7", 7);
    E6_SYNTAX(e, "'use strict'; yi\u0065ld: 1");
    E6_SYNTAX(e, "function* g3() { yi\u0065ld: 1 }");
    es6_env_free(e);
}

/* A strict assignment to a name that is unresolvable when the reference is made throws a
 * ReferenceError, even when the right-hand side creates the global; a name that is declared
 * (also later in the function) is not affected. */
EMB_TEST(j6a_strict_unresolvable_assignment)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "'use strict'; undecl_a = (this.undecl_a = 5)", "ReferenceError");
    E6_THROWS(e, "(function () { 'use strict'; undecl_b = (globalThis.undecl_b = 5) })()", "ReferenceError");
    E6_NUM(e, "(function () { 'use strict'; late = 3; var late; return late })()", 3);
    E6_NUM(e, "'use strict'; (function () { later_g = 4 })(); var later_g; later_g", 4);
    /* J6b: a getter or valueOf that creates the global while the right-hand side evaluates does
     * not make the reference resolvable either (ECMA-262 13.15.2, PutValue step 5.a). */
    E6_THROWS(e, "var gap_o = { get p() { globalThis.gap_z = 1; return 2 } };"
                 "(function () { 'use strict'; gap_z = gap_o.p + 0; return gap_z })()",
              "ReferenceError");
    E6_BOOL(e, "globalThis.gap_z === 1", true);
    E6_NUM(e, "var pre_c = 1; (function () { 'use strict'; pre_c = (pre_c + 1) })(); pre_c", 2);
    es6_env_free(e);
}

/* An arrow function whose parameters have expressions keeps its body vars apart: a `var arguments`
 * in the body does not touch the `arguments` that an eval in a parameter declared. */
EMB_TEST(j6a_arrow_body_var_arguments)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e,
            "var f = (p = eval(\"var arguments = 'param'\"), q = () => arguments) => {"
            " var arguments = 'local'; return arguments + ':' + q() }; f()",
            "local:param");
    E6_TEXT(e, "var g = () => { var arguments; return typeof arguments }; g()", "undefined");
    es6_env_free(e);
}

EMB_TEST_MAIN()
