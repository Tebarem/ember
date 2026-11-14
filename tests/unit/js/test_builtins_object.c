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

/* The name property of the pending exception, or "" when there is none. Clears the exception. */
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

/* Evaluates src and checks that its ToString is want. */
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

/* Evaluates src and checks that it throws an error whose name is want. */
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

/* Symbols defined from C for the tests that need symbol keys: the global S is a plain symbol, and
 * TAG is Symbol.toStringTag (the well-known symbol). */
static void install_symbols(env e)
{
    embi_js_runtime *rt = e.rt;
    embi_js_symbol *s = embi_js_symbol_new(rt, embi_js_from_string(embi_js_atom_intern_utf8(rt, "s", 1)));
    embi_js_define_raw(rt, e.r->global, embi_js_key_c(rt, "S"), embi_js_from_symbol(s), 0);
    embi_js_define_raw(rt, e.r->global, embi_js_key_c(rt, "TAG"),
                       embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), 0);
}

/* ------------------------------------------------------------------ the constructor */

EMB_TEST(object_ctor_and_global_binding)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("typeof Object", "function");
    T_TEXT("Object.length + ':' + Object.name", "1:Object");
    T_TEXT("Object.prototype.constructor === Object", "true");
    T_TEXT("Object.prototype === Object.getPrototypeOf({})", "true");
    T_TEXT("typeof Object(1) + typeof Object('s') + typeof Object(true)", "objectobjectobject");
    T_TEXT("typeof Object(null) + typeof Object(undefined)", "objectobject");
    T_TEXT("Object(null) === Object(null)", "false");
    T_TEXT("new Object() instanceof Object", "true");
    T_TEXT("Object.prototype.hasOwnProperty('__proto__')", "true");
    /* Object.prototype is an immutable prototype exotic object. */
    T_THROWS("Object.setPrototypeOf(Object.prototype, {})", "TypeError");
    env_free(e);
}

EMB_TEST(object_statics_lengths_names_and_attributes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Each entry: name and length. Every static is a function whose name is its key. */
    T_TEXT("var spec = {assign:2, create:2, defineProperties:2, defineProperty:3, entries:1, "
           "freeze:1, getOwnPropertyDescriptor:2, getOwnPropertyDescriptors:1, "
           "getOwnPropertyNames:1, getOwnPropertySymbols:1, getPrototypeOf:1, hasOwn:2, is:2, "
           "isExtensible:1, isFrozen:1, isSealed:1, keys:1, preventExtensions:1, seal:1, "
           "setPrototypeOf:2, values:1};"
           "var bad = 0;"
           "for (var k in spec) {"
           "  var f = Object[k];"
           "  if (typeof f !== 'function' || f.length !== spec[k] || f.name !== k) bad++;"
           "}"
           "'' + bad",
           "0");
    /* Attributes of the statics: writable and configurable, not enumerable. */
    T_TEXT("var d = Object.getOwnPropertyDescriptor(Object, 'keys');"
           "'' + d.writable + d.enumerable + d.configurable",
           "truefalsetrue");
    /* The prototype link is fixed and not enumerable. */
    T_TEXT("var p = Object.getOwnPropertyDescriptor(Object, 'prototype');"
           "'' + p.writable + p.enumerable + p.configurable",
           "falsefalsefalse");
    /* Object.prototype methods: length, name and attributes. */
    T_TEXT("var pspec = {hasOwnProperty:1, isPrototypeOf:1, propertyIsEnumerable:1, "
           "toLocaleString:0, toString:0, valueOf:0};"
           "var pbad = 0;"
           "for (var k in pspec) {"
           "  var f = Object.prototype[k];"
           "  var d = Object.getOwnPropertyDescriptor(Object.prototype, k);"
           "  if (typeof f !== 'function' || f.length !== pspec[k] || f.name !== k ||"
           "      d.enumerable || !d.writable || !d.configurable) pbad++;"
           "}"
           "'' + pbad",
           "0");
    env_free(e);
}

EMB_TEST(object_ctor_conversions)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* ToObject on primitives returns wrappers, and a wrapper passes through unchanged. */
    T_TEXT("var w = Object('abc'); w.length + ':' + w[1]", "3:b");
    T_TEXT("var n = {}; Object(n) === n", "true");
    T_TEXT("var b = Object(true); typeof Object.prototype.valueOf.call(b)", "object");
    env_free(e);
}

/* ------------------------------------------------------------------ assign and create */

EMB_TEST(object_assign_copies_own_enumerable)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var t = Object.assign({a:1}, {b:2, c:3}, null, undefined, 'xy');"
           "t.a + '' + t.b + t.c + t[0] + t[1]",
           "123xy");
    /* Non-enumerable source properties are skipped. */
    T_TEXT("var src = {}; Object.defineProperty(src, 'hidden', {value: 9, enumerable: false});"
           "src.shown = 1; var r = Object.assign({}, src);"
           "'' + r.hasOwnProperty('hidden') + r.shown",
           "false1");
    /* Setters run on the target, in order. */
    T_TEXT("var log = ''; var tgt = {set x(v) { log += 'x' + v; }};"
           "Object.assign(tgt, {x: 1}, {x: 2}); log",
           "x1x2");
    /* A getter on the source is read once per key. */
    T_TEXT("var reads = 0; var s = {get g() { reads++; return 5; }};"
           "var r2 = Object.assign({}, s); '' + reads + r2.g", "15");
    T_THROWS("Object.assign()", "TypeError");
    T_THROWS("var fz = Object.freeze({a:1}); Object.assign(fz, {a:2})", "TypeError");
    env_free(e);
}

EMB_TEST(object_create_and_proto)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Object.getPrototypeOf(Object.create(null)) === null", "true");
    T_TEXT("var p = {m: 7}; var c = Object.create(p); c.m", "7");
    T_TEXT("var p2 = {}; Object.create(p2) instanceof Object", "true");
    T_TEXT("var o3 = Object.create({}, {x: {value: 4, enumerable: true}}); o3.x", "4");
    /* Properties argument: undefined is ignored, anything else must convert. */
    T_TEXT("Object.keys(Object.create({}, undefined)).length + ''", "0");
    T_THROWS("Object.create(1)", "TypeError");
    T_THROWS("Object.create(undefined)", "TypeError");
    T_THROWS("Object.create({}, null)", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ descriptors */

EMB_TEST(object_define_property_and_properties)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Defaults are false for every attribute that is not given. */
    T_TEXT("var o = {}; Object.defineProperty(o, 'a', {value: 1});"
           "var d = Object.getOwnPropertyDescriptor(o, 'a');"
           "'' + d.value + d.writable + d.enumerable + d.configurable",
           "1falsefalsefalse");
    /* A non-writable, non-configurable value cannot change. */
    T_THROWS("Object.defineProperty(o, 'a', {value: 2})", "TypeError");
    /* Redefining a configurable writable property keeps the attributes that are not given. */
    T_TEXT("var o3 = {}; Object.defineProperty(o3, 'b', {value: 1, writable: true,"
           "  enumerable: true, configurable: true});"
           "Object.defineProperty(o3, 'b', {value: 5}); o3.b + '' + Object.keys(o3).length",
           "51");
    /* Accessor descriptor. */
    T_TEXT("var g = {}; Object.defineProperty(g, 'x', {get: function () { return 8; },"
           "  enumerable: true, configurable: true}); g.x + ''",
           "8");
    /* defineProperty returns the object. */
    T_TEXT("var q = {}; Object.defineProperty(q, 'z', {value: 0}) === q", "true");
    /* Non-object target and malformed descriptor. */
    T_THROWS("Object.defineProperty(1, 'a', {})", "TypeError");
    T_THROWS("Object.defineProperty({}, 'a', 1)", "TypeError");
    T_THROWS("Object.defineProperty({}, 'a', {get: 1})", "TypeError");
    T_THROWS("Object.defineProperty({}, 'a', {get: function () {}, value: 1})", "TypeError");
    /* A non-configurable property rejects an incompatible change. */
    T_THROWS("var nc = {}; Object.defineProperty(nc, 'k', {value: 1});"
             "Object.defineProperty(nc, 'k', {value: 2})",
             "TypeError");
    /* Several properties at once. The descriptors are read before any is applied. */
    T_TEXT("var m = Object.defineProperties({}, {"
           "  p: {value: 'p', enumerable: true}, q: {value: 'q', enumerable: true}});"
           "m.p + m.q",
           "pq");
    /* Non-enumerable entries of the properties object are skipped. */
    T_TEXT("var props = {}; Object.defineProperty(props, 'hid', {value: {value: 1}});"
           "var r = Object.defineProperties({}, props); r.hasOwnProperty('hid')",
           "false");
    T_THROWS("Object.defineProperties({}, {a: 1})", "TypeError");
    T_THROWS("Object.defineProperties(1, {})", "TypeError");
    env_free(e);
}

EMB_TEST(object_get_own_property_descriptor_and_names)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var d = Object.getOwnPropertyDescriptor({k: 3}, 'k');"
           "'' + d.value + d.writable + d.enumerable + d.configurable + ('get' in d)",
           "3truetruetruefalse");
    T_TEXT("var a = Object.getOwnPropertyDescriptor({get k() { return 1; }}, 'k');"
           "'' + (typeof a.get) + (a.set === undefined) + a.enumerable + ('value' in a)",
           "functiontruetruefalse");
    T_TEXT("Object.getOwnPropertyDescriptor({}, 'nothere') === undefined", "true");
    /* Primitives are converted; string indices and length are own properties. */
    T_TEXT("var sd = Object.getOwnPropertyDescriptor('abc', 'length');"
           "'' + sd.value + sd.writable + sd.enumerable + sd.configurable",
           "3falsefalsefalse");
    T_TEXT("Object.getOwnPropertyDescriptor(1, 'x') === undefined", "true");
    T_THROWS("Object.getOwnPropertyDescriptor(null, 'x')", "TypeError");

    /* getOwnPropertyDescriptors returns one descriptor per own key, including non-enumerable. */
    T_TEXT("var all = Object.getOwnPropertyDescriptors({a: 1});"
           "all.a.value + '' + Object.keys(all).length",
           "11");

    /* getOwnPropertyNames lists indices, then strings in creation order, including hidden ones. */
    T_TEXT("var n = {b: 1, 2: 'x', a: 2}; Object.defineProperty(n, 'h', {value: 0});"
           "var names = Object.getOwnPropertyNames(n);"
           "names.length + ':' + names[0] + names[1] + names[2] + names[3]",
           "4:2bah");
    T_TEXT("Object.getOwnPropertyNames('ab').length + ''", "3");
    T_TEXT("Object.getOwnPropertyNames(Object.prototype).length > 5", "true");
    env_free(e);
}

/* ------------------------------------------------------------------ enumeration */

EMB_TEST(object_keys_values_entries)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var o = {z: 1, 10: 'ten', y: 2, 9: 'nine'};"
           "var k = Object.keys(o);"
           "k.length + ':' + k[0] + k[1] + k[2] + k[3]",
           "4:910zy");
    T_TEXT("var v = Object.values({a: 'x', b: 'y'}); v.length + v[0] + v[1]", "2xy");
    T_TEXT("var en = Object.entries({a: 'x'}); en.length + ':' + en[0][0] + en[0][1]", "1:ax");
    /* Non-enumerable properties are left out, and so are inherited ones. */
    T_TEXT("var base = {inh: 1}; var c = Object.create(base); c.own = 2;"
           "Object.keys(c).length + Object.keys(c)[0]",
           "1own");
    T_TEXT("Object.keys('ab').length + ''", "2");
    T_TEXT("Object.keys(5).length + ''", "0");
    T_THROWS("Object.keys(null)", "TypeError");
    T_THROWS("Object.values(undefined)", "TypeError");
    /* A getter that deletes a later key stops it from being listed by values. */
    T_TEXT("var dl = {get a() { delete dl.b; return 1; }, b: 2};"
           "var vals = Object.values(dl); vals.length + ''",
           "1");
    env_free(e);
}

/* ------------------------------------------------------------------ integrity */

EMB_TEST(object_has_own)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var ho = {a: 1}; '' + Object.hasOwn(ho, 'a') + Object.hasOwn(ho, 'toString')"
           "+ Object.hasOwn({1: 'x'}, 1) + Object.hasOwn('ab', 1)",
           "truefalsetruetrue");
    T_TEXT("'' + Object.hasOwn(Object.create({p: 1}), 'p')", "false");
    /* ToObject runs before ToPropertyKey, so a null target throws before the key is converted. */
    T_TEXT("var kl = ''; var key = {toString: function () { kl += 'k'; return 'x'; }};"
           "try { Object.hasOwn(null, key); } catch (err) { kl += err.name; } kl",
           "TypeError");
    T_TEXT("var kl2 = ''; var key2 = {toString: function () { kl2 += 'k'; return 'x'; }};"
           "Object.hasOwn({}, key2); kl2",
           "k");
    T_THROWS("Object.hasOwn(undefined, 'x')", "TypeError");
    T_THROWS("Object.hasOwn(null, 'x')", "TypeError");
    env_free(e);
}

EMB_TEST(object_integrity_levels)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var f = {a: 1}; Object.freeze(f) === f", "true");
    T_TEXT("var f2 = {a: 1}; Object.freeze(f2);"
           "'' + Object.isFrozen(f2) + Object.isSealed(f2) + Object.isExtensible(f2)",
           "truetruefalse");
    /* A frozen object ignores sloppy writes. */
    T_TEXT("var f3 = Object.freeze({a: 1}); f3.a = 9; f3.a + ''", "1");
    T_TEXT("var s = {a: 1}; Object.seal(s); s.a = 5; delete s.a; '' + s.a + Object.isSealed(s)"
           "+ Object.isFrozen(s)",
           "5truefalse");
    T_TEXT("var pe = {}; Object.preventExtensions(pe); pe.n = 1;"
           "'' + pe.n + Object.isExtensible(pe) + Object.isSealed(pe)",
           "undefinedfalsetrue");
    /* Primitives: freeze returns them, and they count as frozen and not extensible. */
    T_TEXT("Object.freeze(3) + ''", "3");
    T_TEXT("'' + Object.isFrozen(1) + Object.isSealed('x') + Object.isExtensible(1)",
           "truetruefalse");
    T_TEXT("Object.preventExtensions(7) + ''", "7");
    T_TEXT("Object.isFrozen({}) + ''", "false");
    /* An empty non-extensible object is frozen and sealed. */
    T_TEXT("var em = Object.preventExtensions({}); '' + Object.isFrozen(em)", "true");
    env_free(e);
}

/* ------------------------------------------------------------------ prototype and identity */

EMB_TEST(object_get_prototype_of_is_set_prototype_of)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Object.getPrototypeOf({}) === Object.prototype", "true");
    T_TEXT("Object.getPrototypeOf(Object.prototype) === null", "true");
    T_THROWS("Object.getPrototypeOf(null)", "TypeError");
    T_THROWS("Object.getPrototypeOf(undefined)", "TypeError");
    T_TEXT("var sp = {}; var sub = Object.setPrototypeOf({}, sp); Object.getPrototypeOf(sub) === sp",
           "true");
    T_TEXT("Object.setPrototypeOf({a: 1}, null).a + ''", "1");
    T_TEXT("Object.setPrototypeOf(3, null) + ''", "3");
    T_THROWS("Object.setPrototypeOf({}, 1)", "TypeError");
    T_THROWS("Object.setPrototypeOf(null, {})", "TypeError");
    /* A prototype cycle is refused. */
    T_THROWS("var a1 = {}; var b1 = Object.create(a1); Object.setPrototypeOf(a1, b1)", "TypeError");
    /* A non-extensible object keeps its prototype. */
    T_THROWS("var ne = Object.preventExtensions({}); Object.setPrototypeOf(ne, {})", "TypeError");

    T_TEXT("Object.is(NaN, NaN) + ''", "true");
    T_TEXT("Object.is(0, -0) + ''", "false");
    T_TEXT("Object.is('a', 'a') + ''", "true");
    T_TEXT("Object.is({}, {}) + ''", "false");
    T_TEXT("Object.is() + ''", "true");
    env_free(e);
}

EMB_TEST(object_proto_methods)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var o = {p: 1}; o.hasOwnProperty('p') + '' + o.hasOwnProperty('toString')", "truefalse");
    /* The key is converted with ToPropertyKey, so 1 and '1' name the same property. */
    T_TEXT("var n = {1: 'one'}; n.hasOwnProperty(1) + '' + n.hasOwnProperty(2)", "truefalse");
    T_TEXT("Object.prototype.hasOwnProperty.call('ab', 0) + ''", "true");
    T_THROWS("Object.prototype.hasOwnProperty.call(undefined, 'x')", "TypeError");

    T_TEXT("var base = {}; var der = Object.create(base);"
           "base.isPrototypeOf(der) + '' + der.isPrototypeOf(base) + base.isPrototypeOf(1)",
           "truefalsefalse");
    T_TEXT("Object.prototype.isPrototypeOf.call(Object.prototype, {}) + ''", "true");

    T_TEXT("var pe = {vis: 1}; Object.defineProperty(pe, 'hid', {value: 2});"
           "'' + pe.propertyIsEnumerable('vis') + pe.propertyIsEnumerable('hid')"
           "+ pe.propertyIsEnumerable('nope')",
           "truefalsefalse");
    T_THROWS("Object.prototype.propertyIsEnumerable.call(null, 'x')", "TypeError");

    T_TEXT("var lp = {toString: function () { return 'custom'; }}; lp.toLocaleString()", "custom");
    T_THROWS("Object.prototype.toLocaleString.call(undefined)", "TypeError");

    T_TEXT("var vo = {}; vo.valueOf() === vo", "true");
    T_TEXT("typeof Object.prototype.valueOf.call(4)", "object");
    T_THROWS("Object.prototype.valueOf.call(null)", "TypeError");
    env_free(e);
}

EMB_TEST(object_proto_to_string_tags)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    install_symbols(e);
    T_TEXT("Object.prototype.toString.call(undefined)", "[object Undefined]");
    T_TEXT("Object.prototype.toString.call(null)", "[object Null]");
    T_TEXT("Object.prototype.toString.call({})", "[object Object]");
    T_TEXT("Object.prototype.toString.call([])", "[object Array]");
    T_TEXT("Object.prototype.toString.call(function () {})", "[object Function]");
    T_TEXT("Object.prototype.toString.call(new Error('x'))", "[object Error]");
    T_TEXT("Object.prototype.toString.call(Object(true))", "[object Boolean]");
    T_TEXT("Object.prototype.toString.call(Object(1))", "[object Number]");
    T_TEXT("Object.prototype.toString.call(Object('s'))", "[object String]");
    T_TEXT("(function () { return Object.prototype.toString.call(arguments); })()",
           "[object Arguments]");
    T_TEXT("var tagged = {}; tagged[TAG] = 'Custom'; Object.prototype.toString.call(tagged)",
           "[object Custom]");
    /* A non-string tag is ignored. */
    T_TEXT("var nt = {}; nt[TAG] = 5; Object.prototype.toString.call(nt)", "[object Object]");
    env_free(e);
}

EMB_TEST(object_proto_accessor)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var q = {}; q.__proto__ === Object.prototype", "true");
    T_TEXT("var pr = {m: 3}; var c = {}; c.__proto__ = pr; c.m + ''", "3");
    /* A non-object assignment is ignored. */
    T_TEXT("var c2 = {}; c2.__proto__ = 5; Object.getPrototypeOf(c2) === Object.prototype", "true");
    T_TEXT("Object.getOwnPropertyDescriptor(Object.prototype, '__proto__').enumerable + ''",
           "false");
    T_THROWS("Object.prototype.__proto__ = {}", "TypeError");
    env_free(e);
}

EMB_TEST(object_legacy_accessor_methods)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var o = {}; o.__defineGetter__('g', function () { return 5; }); o.g + ''", "5");
    T_TEXT("var so = {}; so.__defineSetter__('s', function (v) { this.t = v * 2; });"
           "so.s = 4; so.t + ''",
           "8");
    T_TEXT("var gd = {}; gd.__defineGetter__('x', function () {});"
           "var xd = Object.getOwnPropertyDescriptor(gd, 'x');"
           "'' + xd.enumerable + xd.configurable + (xd.set === undefined)",
           "truetruetrue");
    T_TEXT("var lk = {get p() { return 1; }}; typeof lk.__lookupGetter__('p')", "function");
    T_TEXT("var child = Object.create({get q() { return 2; }}); typeof child.__lookupGetter__('q')",
           "function");
    T_TEXT("var plain = {v: 1}; (plain.__lookupGetter__('v') === undefined) + ''", "true");
    T_TEXT("var setonly = {set w(v) {}}; (setonly.__lookupGetter__('w') === undefined) + ''",
           "true");
    T_TEXT("var sl = {set w2(v) {}}; typeof sl.__lookupSetter__('w2')", "function");
    T_TEXT("({}).__lookupSetter__('nothere') === undefined ? 'y' : 'n'", "y");
    T_THROWS("({}).__defineGetter__('h', 1)", "TypeError");
    T_THROWS("({}).__defineSetter__('h', null)", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ symbol keys (from C) */

EMB_TEST(object_symbol_keys)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    install_symbols(e);
    T_TEXT("var so = {}; so[S] = 4; Object.getOwnPropertySymbols(so).length + ''", "1");
    T_TEXT("var so2 = {}; so2[S] = 4; Object.getOwnPropertyNames(so2).length + ''", "0");
    T_TEXT("var sc = {a: 1}; sc[S] = 2; Object.keys(sc).length + ''", "1");
    T_TEXT("var src = {}; src[S] = 7; Object.assign({}, src)[S] + ''", "7");
    T_TEXT("var sv = {}; sv[S] = 7; Object.values(sv).length + ''", "0");
    T_TEXT("var sd = {}; sd[S] = 1; Object.getOwnPropertyDescriptor(sd, S).value + ''", "1");
    env_free(e);
}

/* ------------------------------------------------------------------ GC safety */

/* A getter that allocates a lot and forces collections while Object.entries and Object.assign
 * are running. The results must be intact afterwards. */
static embi_js_value churn(embi_js_realm *r, embi_js_value this_v, int argc,
                           const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    for (int i = 0; i < 4; i++) {
        embi_js_gc(rt);
        (void)embi_js_object_new_plain(rt);
    }
    return embi_js_number(42.0);
}

EMB_TEST(object_statics_survive_collection_in_getters)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_define_method(e.r, e.r->global, "churn", churn, 0, 0);
    install_symbols(e);
    T_TEXT("var g = {}; for (var i = 0; i < 40; i++) { g['k' + i] = i; }"
           "Object.defineProperty(g, 'z', {get: churn, enumerable: true});"
           "var vs = Object.values(g); vs.length + ':' + vs[40]",
           "41:42");
    T_TEXT("var sym = {get a() { churn(); return 'A'; }}; sym[S] = 's';"
           "var asg = Object.assign({}, sym); asg.a + asg[S]",
           "As");
    T_TEXT("var dp = {}; Object.defineProperties(dp, {"
           "  p: {get: function () { churn(); return 1; }, enumerable: true},"
           "  q: {value: 2, enumerable: true}});"
           "dp.p + dp.q",
           "3");
    env_free(e);
}

EMB_TEST_MAIN()
