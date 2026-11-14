#include "emb_test.h"

#include "js/js.h"
#include "js/ops.h"

#include <stdint.h>
#include <string.h>

/* Symbol, Symbol.prototype, the well-known symbols, Symbol.for/keyFor, and the built-in hook
 * points that consult them (ToPrimitive, instanceof, toStringTag, unscopables, concat, species,
 * and the String Symbol-method dispatch). Scripts use ES5 syntax only. */

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

/* Logs the code units of a string as ASCII (for diagnostics). */
static void sb_log_str(const embi_js_string *s)
{
    char buf[512];
    uint32_t n = embi_js_str_len(s) < sizeof buf - 1 ? embi_js_str_len(s) : (uint32_t)(sizeof buf - 1);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        buf[i] = c < 128 ? (char)c : '?';
    }
    buf[n] = 0;
    SDL_Log("      message: %s", buf);
}

/* Evaluates src. Returns true when it yields the boolean true; logs otherwise. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "symbol.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(e.rt);
        embi_js_value m = embi_js_is_object(ex) ? embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "message")) : EMBI_JS_UNDEFINED;
        embi_js_string *ms = embi_js_is_string(m) ? embi_js_as_string(m) : nullptr;
        SDL_Log("    js threw (%s): %s", ms ? "message follows" : "no message", src);
        if (ms) {
            sb_log_str(ms);
        }
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

/* Evaluates src, which must throw. Returns true when the thrown value is an object whose name
 * property equals want. */
static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "symbol.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected %s: %s", want, src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "name"));
        embi_js_string *ns = embi_js_is_string(n) ? embi_js_as_string(n) : nullptr;
        if (ns && embi_js_str_len(ns) == strlen(want)) {
            ok = true;
            for (size_t i = 0; i < strlen(want); i++) {
                if (embi_js_str_at(ns, (uint32_t)i) != (uint16_t)want[i]) {
                    ok = false;
                }
            }
        }
    }
    if (!ok) {
        SDL_Log("    wrong exception for: %s", src);
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

/* ------------------------------------------------------------------ Symbol() and its prototype */

EMB_TEST(symbol_call_creates_distinct_symbols)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "typeof Symbol() === 'symbol'"));
    EMB_CHECK(js_true(e, "Symbol() !== Symbol()"));
    EMB_CHECK(js_true(e, "var s1 = Symbol('a'); var s2 = Symbol('a'); s1 !== s2"));
    EMB_CHECK(js_true(e, "Symbol.length === 0 && Symbol.name === 'Symbol'"));
    EMB_CHECK(js_true(e, "typeof Symbol === 'function'"));
    env_free(e);
}

EMB_TEST(symbol_description_and_to_string)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Symbol('x').toString() === 'Symbol(x)'"));
    EMB_CHECK(js_true(e, "Symbol().toString() === 'Symbol()'"));
    EMB_CHECK(js_true(e, "Symbol().description === undefined"));
    EMB_CHECK(js_true(e, "Symbol(undefined).description === undefined"));
    EMB_CHECK(js_true(e, "Symbol('').description === ''"));
    EMB_CHECK(js_true(e, "Symbol(null).description === 'null'"));
    EMB_CHECK(js_true(e, "Symbol(1.5).description === '1.5'"));
    EMB_CHECK(js_true(e, "String(Symbol('q')) === 'Symbol(q)'"));
    EMB_CHECK(js_true(e, "Symbol.iterator.description === 'Symbol.iterator'"));
    EMB_CHECK(js_true(e, "Symbol.iterator.toString() === 'Symbol(Symbol.iterator)'"));
    env_free(e);
}

EMB_TEST(symbol_construct_throws)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_throws(e, "new Symbol()", "TypeError"));
    EMB_CHECK(js_throws(e, "new Symbol('x')", "TypeError"));
    EMB_CHECK(js_true(e, "typeof Symbol.prototype === 'object' && Symbol.prototype.constructor === Symbol"));
    EMB_CHECK(js_true(e, "Symbol.prototype.constructor === Symbol"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(Symbol.prototype) === Object.prototype"));
    env_free(e);
}

EMB_TEST(symbol_conversions_throw)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_throws(e, "Symbol() + ''", "TypeError"));
    EMB_CHECK(js_throws(e, "+Symbol()", "TypeError"));
    EMB_CHECK(js_throws(e, "Symbol() * 2", "TypeError"));
    EMB_CHECK(js_throws(e, "'' + Symbol()", "TypeError"));
    EMB_CHECK(js_true(e, "!!Symbol() === true"));
    EMB_CHECK(js_true(e, "Symbol() == Symbol.iterator || true"));
    env_free(e);
}

EMB_TEST(symbol_property_keys)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var o = {}; var k = Symbol('k'); o[k] = 7; o[k] === 7"));
    EMB_CHECK(js_true(e, "Object.keys(o).length === 0"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertySymbols(o)[0] === k"));
    EMB_CHECK(js_true(e, "o.hasOwnProperty(k) === true && k in o"));
    EMB_CHECK(js_true(e, "delete o[k] && !(k in o)"));
    env_free(e);
}

/* ------------------------------------------------------------------ Symbol.for and keyFor */

EMB_TEST(symbol_for_registry)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Symbol.for('a') === Symbol.for('a')"));
    EMB_CHECK(js_true(e, "Symbol.for('a') !== Symbol('a')"));
    EMB_CHECK(js_true(e, "Symbol.keyFor(Symbol.for('a')) === 'a'"));
    EMB_CHECK(js_true(e, "Symbol.keyFor(Symbol('a')) === undefined"));
    EMB_CHECK(js_true(e, "Symbol.keyFor(Symbol.iterator) === undefined"));
    EMB_CHECK(js_true(e, "Symbol.for(undefined).description === 'undefined'"));
    EMB_CHECK(js_true(e, "Symbol.for(1) === Symbol.for('1')"));
    EMB_CHECK(js_true(e, "Symbol.keyFor(Symbol.for(1)) === '1'"));
    EMB_CHECK(js_true(e, "Symbol.for('').description === '' && Symbol.keyFor(Symbol.for('')) === ''"));
    EMB_CHECK(js_true(e, "Symbol.for.length === 1 && Symbol.keyFor.length === 1"));
    EMB_CHECK(js_throws(e, "Symbol.keyFor('a')", "TypeError"));
    EMB_CHECK(js_throws(e, "Symbol.keyFor({})", "TypeError"));
    env_free(e);
}

EMB_TEST(symbol_for_survives_collection)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Garbage between the two lookups makes collections likely: the registry must keep its
     * symbols alive through the persistent handle. */
    EMB_CHECK(js_true(e, "var first = Symbol.for('persist'); var junk = [];"
                         "for (var i = 0; i < 20000; i++) { junk.push({i: i, s: 'x' + i}); }"
                         "junk = null;"
                         "first === Symbol.for('persist') && Symbol.keyFor(first) === 'persist'"));
    env_free(e);
}

/* ------------------------------------------------------------------ Symbol.prototype */

EMB_TEST(symbol_prototype_methods_and_wrappers)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "typeof Object(Symbol()) === 'object'"));
    EMB_CHECK(js_true(e, "var s = Symbol('w'); var w = Object(s); w !== s"));
    EMB_CHECK(js_true(e, "w.valueOf() === s && w.description === 'w' && w.toString() === 'Symbol(w)'"));
    EMB_CHECK(js_true(e, "Symbol.prototype.valueOf.call(w) === s"));
    EMB_CHECK(js_true(e, "Symbol.prototype.toString.call(s) === 'Symbol(w)'"));
    EMB_CHECK(js_true(e, "w == s && !(w === s)"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(w) === Symbol.prototype"));
    EMB_CHECK(js_throws(e, "Symbol.prototype.toString.call(1)", "TypeError"));
    EMB_CHECK(js_throws(e, "Symbol.prototype.valueOf.call({})", "TypeError"));
    EMB_CHECK(js_throws(e, "Object.getOwnPropertyDescriptor(Symbol.prototype, 'description').get.call('s')",
                        "TypeError"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(Symbol.prototype, 'description').get.name"
                         " === 'get description'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(Symbol.prototype, 'description').set === undefined"));
    env_free(e);
}

EMB_TEST(symbol_prototype_to_primitive_and_tag)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var tp = Symbol.prototype[Symbol.toPrimitive];"
                         "typeof tp === 'function' && tp.length === 1 && tp.name === '[Symbol.toPrimitive]'"));
    EMB_CHECK(js_true(e, "Symbol.prototype[Symbol.toPrimitive].call(s = Symbol('p'), 'number') === s"));
    EMB_CHECK(js_true(e, "Symbol.prototype[Symbol.toStringTag] === 'Symbol'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(Symbol()) === '[object Symbol]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(Object(Symbol())) === '[object Symbol]'"));
    EMB_CHECK(js_true(e, "var d = Object.getOwnPropertyDescriptor(Symbol.prototype, Symbol.toStringTag);"
                         "d.writable === false && d.enumerable === false && d.configurable === true"));
    EMB_CHECK(js_true(e, "var d2 = Object.getOwnPropertyDescriptor(Symbol.prototype, Symbol.toPrimitive);"
                         "d2.writable === false && d2.enumerable === false && d2.configurable === true"));
    env_free(e);
}

EMB_TEST(symbol_well_known_properties)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    static const char *const names[] = {
        "asyncIterator", "hasInstance", "isConcatSpreadable", "iterator", "match", "matchAll",
        "replace", "search", "species", "split", "toPrimitive", "toStringTag", "unscopables",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char src[256];
        SDL_snprintf(src, sizeof src,
                     "typeof Symbol.%s === 'symbol' && Symbol.%s.description === 'Symbol.%s'"
                     " && Symbol.keyFor(Symbol.%s) === undefined",
                     names[i], names[i], names[i], names[i]);
        EMB_CHECK(js_true(e, src));
        SDL_snprintf(src, sizeof src,
                     "var pd = Object.getOwnPropertyDescriptor(Symbol, '%s');"
                     "pd.writable === false && pd.enumerable === false && pd.configurable === false",
                     names[i]);
        EMB_CHECK(js_true(e, src));
    }
    EMB_CHECK(js_true(e, "Symbol.iterator !== Symbol.asyncIterator && Symbol.match !== Symbol.matchAll"));
    EMB_CHECK(js_true(e, "var pp = Object.getOwnPropertyDescriptor(Symbol, 'prototype');"
                         "pp.writable === false && pp.enumerable === false && pp.configurable === false"));
    EMB_CHECK(js_true(e, "var gd = Object.getOwnPropertyDescriptor(this, 'Symbol');"
                         "gd.writable === true && gd.enumerable === false && gd.configurable === true"));
    env_free(e);
}

/* ------------------------------------------------------------------ hook points */

EMB_TEST(symbol_to_primitive_hook)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var seen = []; var o = {};"
                         "o[Symbol.toPrimitive] = function (h) { seen.push(h); return 21; };"
                         "o * 2 === 42 && seen[0] === 'number'"));
    EMB_CHECK(js_true(e, "var s2 = []; var p = {};"
                         "p[Symbol.toPrimitive] = function (h) { s2.push(h); return 'pv'; };"
                         "(p + '') === 'pv' && s2[0] === 'default' && String(p) === 'pv' && s2[1] === 'string'"));
    EMB_CHECK(js_true(e, "var q = {}; q[Symbol.toPrimitive] = function () { return {}; };"
                         "var threw = false; try { q + 1; } catch (err) { threw = err instanceof TypeError; }"
                         "threw"));
    EMB_CHECK(js_true(e, "var n = {}; n[Symbol.toPrimitive] = null;"
                         "var r = n * 1; r !== r"));
    EMB_CHECK(js_true(e, "var u = {valueOf: function () { return 5; }};"
                         "u[Symbol.toPrimitive] = undefined; u * 2 === 10"));
    env_free(e);
}

EMB_TEST(symbol_has_instance_hook)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var C = {}; C[Symbol.hasInstance] = function (v) { return v === 1; };"
                         "(1 instanceof C) === true && (2 instanceof C) === false"));
    EMB_CHECK(js_true(e, "typeof Function.prototype[Symbol.hasInstance] === 'function'"));
    EMB_CHECK(js_true(e, "Function.prototype[Symbol.hasInstance].call(Array, []) === true"));
    EMB_CHECK(js_true(e, "var Fn = function () {}; var inst = new Fn();"
                         "Fn[Symbol.hasInstance](inst) === true && Fn[Symbol.hasInstance]({}) === false"));
    EMB_CHECK(js_throws(e, "({}) instanceof 1", "TypeError"));
    env_free(e);
}

EMB_TEST(symbol_to_string_tag_hook)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var t = {}; t[Symbol.toStringTag] = 'Tagged';"
                         "Object.prototype.toString.call(t) === '[object Tagged]'"));
    EMB_CHECK(js_true(e, "var t2 = {}; t2[Symbol.toStringTag] = 3;"
                         "Object.prototype.toString.call(t2) === '[object Object]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(Math) === '[object Math]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(JSON) === '[object JSON]'"));
    env_free(e);
}

EMB_TEST(symbol_is_concat_spreadable_hook)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var like = {length: 2, 0: 'x', 1: 'y'}; like[Symbol.isConcatSpreadable] = true;"
                         "[].concat(like).length === 2 && [].concat(like)[1] === 'y'"));
    EMB_CHECK(js_true(e, "var arr = [1, 2]; arr[Symbol.isConcatSpreadable] = false;"
                         "var c = [].concat(arr); c.length === 1 && c[0] === arr"));
    EMB_CHECK(js_true(e, "var plain = {length: 1, 0: 'z'}; [].concat(plain).length === 1"));
    env_free(e);
}

EMB_TEST(symbol_species_hook)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(Array, Symbol.species).get !== undefined"));
    EMB_CHECK(js_true(e, "Array[Symbol.species] === Array"));
    EMB_CHECK(js_true(e, "var Sub = function (n) { this.made = n; }; Sub[Symbol.species] = Sub;"
                         "var a = [1, 2]; a.constructor = Sub;"
                         "var m = a.map(function (x) { return x; }); m.made === 2"));
    EMB_CHECK(js_true(e, "var Plain = function () {}; var b = [3]; b.constructor = Plain;"
                         "Plain[Symbol.species] = undefined;"
                         "Array.isArray(b.map(function (x) { return x; })) === true"));
    env_free(e);
}

EMB_TEST(symbol_unscopables_array_defaults)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var u = Array.prototype[Symbol.unscopables];"
                         "u.flat === true && u.includes === true && u.keys === true && u.map === undefined"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(Array.prototype[Symbol.unscopables]) === null"));
    env_free(e);
}

/* The String methods consult the Symbol method of an Object argument only (ES2025 22.1.3). */
EMB_TEST(symbol_string_dispatch_objects_only)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var rep = {}; rep[Symbol.replace] = function (s, r) { return 'R' + s + r; };"
                         "'abc'.replace(rep, 'x') === 'Rabcx'"));
    EMB_CHECK(js_true(e, "var spl = {}; spl[Symbol.split] = function (s, l) { return 'S' + s + l; };"
                         "'abc'.split(spl, 2) === 'Sabc2'"));
    EMB_CHECK(js_true(e, "var mat = {}; mat[Symbol.match] = function (s) { return 'M' + s; };"
                         "'abc'.match(mat) === 'Mabc'"));
    EMB_CHECK(js_true(e, "var sea = {}; sea[Symbol.search] = function (s) { return 'Q' + s; };"
                         "'abc'.search(sea) === 'Qabc'"));
    EMB_CHECK(js_true(e, "'a1b'.split(1).length === 2 && 'a1b'.replace(1, 'x') === 'axb'"));
    /* Primitive arguments never reach their prototype's Symbol methods. */
    EMB_CHECK(js_true(e, "var hits = 0;"
                         "Object.defineProperty(String.prototype, Symbol.replace, {configurable: true,"
                         " get: function () { hits++; return undefined; }});"
                         "Object.defineProperty(String.prototype, Symbol.split, {configurable: true,"
                         " get: function () { hits++; return undefined; }});"
                         "Object.defineProperty(String.prototype, Symbol.search, {configurable: true,"
                         " get: function () { hits++; return undefined; }});"
                         "'a,b'.replace(',', ';') === 'a;b' && 'a,b'.split(',').length === 2"
                         " && hits === 0"));
    EMB_CHECK(js_true(e, "delete String.prototype[Symbol.replace]; delete String.prototype[Symbol.split];"
                         "delete String.prototype[Symbol.search]; true"));
    env_free(e);
}

/* ------------------------------------------------------------------ C-level hook: with */

EMB_TEST(symbol_with_has_binding_unscopables)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_runtime *rt = e.rt;
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "a"), embi_js_number(1.0), EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "b"), embi_js_number(2.0), EMBI_JS_ATTR_DEFAULT);
    embi_js_object *uns = embi_js_object_new(rt, EMBI_JS_NULL);
    embi_js_define_raw(rt, uns, embi_js_key_c(rt, "a"), EMBI_JS_TRUE, EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, uns, embi_js_key_c(rt, "b"), EMBI_JS_FALSE, EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_wk_sym(rt, EMBI_JS_WK_UNSCOPABLES),
                       embi_js_from_object(uns), EMBI_JS_ATTR_DEFAULT);

    EMB_CHECK_EQ_INT(embi_js_with_has_binding(rt, o, embi_js_key_c(rt, "a")), EMBI_JS_NO);
    EMB_CHECK_EQ_INT(embi_js_with_has_binding(rt, o, embi_js_key_c(rt, "b")), EMBI_JS_YES);
    EMB_CHECK_EQ_INT(embi_js_with_has_binding(rt, o, embi_js_key_c(rt, "zz")), EMBI_JS_NO);

    /* A non-object @@unscopables is ignored. */
    embi_js_object *plain = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, plain, embi_js_key_c(rt, "a"), embi_js_number(1.0), EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, plain, embi_js_wk_sym(rt, EMBI_JS_WK_UNSCOPABLES), EMBI_JS_TRUE,
                       EMBI_JS_ATTR_DEFAULT);
    EMB_CHECK_EQ_INT(embi_js_with_has_binding(rt, plain, embi_js_key_c(rt, "a")), EMBI_JS_YES);
    env_free(e);
}

/* Symbol.for symbols are flagged as registered and fresh symbols are not. */
EMB_TEST(symbol_registered_flag)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_value reg = embi_js_eval(e.r, "Symbol.for('flagged')", strlen("Symbol.for('flagged')"), "symbol.js", 0);
    EMB_REQUIRE(embi_js_is_symbol(reg));
    EMB_CHECK(embi_js_symbol_is_registered(reg));
    embi_js_value fresh = embi_js_eval(e.r, "Symbol('fresh')", strlen("Symbol('fresh')"), "symbol.js", 0);
    EMB_REQUIRE(embi_js_is_symbol(fresh));
    EMB_CHECK(!embi_js_symbol_is_registered(fresh));
    EMB_CHECK(!embi_js_symbol_is_registered(embi_js_number(1.0)));
    env_free(e);
}

EMB_TEST_MAIN()
