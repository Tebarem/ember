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

/* The name property of the pending exception. Clears the exception. */
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

/* ------------------------------------------------------------------ constructor and revocable */

EMB_TEST(proxy_ctor_shape_and_errors)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("typeof Proxy", "function");
    T_TEXT("Proxy.length + ':' + Proxy.name", "2:Proxy");
    T_TEXT("'prototype' in Proxy", "false");
    T_THROWS("Proxy({}, {})", "TypeError");
    T_THROWS("new Proxy(1, {})", "TypeError");
    T_THROWS("new Proxy({}, null)", "TypeError");
    T_TEXT("typeof new Proxy({}, {})", "object");
    T_TEXT("typeof new Proxy(function () {}, {})", "function");
    T_TEXT("typeof Proxy.revocable", "function");
    T_TEXT("var rv = Proxy.revocable({}, {}); Object.keys(rv).join() + ':' + rv.revoke.length + ':' + rv.revoke.name",
           "proxy,revoke:0:");
    env_free(e);
}

EMB_TEST(proxy_revoked_throws_and_keeps_typeof)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_THROWS("var r1 = Proxy.revocable({}, {}); r1.revoke(); r1.proxy.x", "TypeError");
    T_TEXT("var r2 = Proxy.revocable({}, {}); r2.revoke(); typeof r2.proxy", "object");
    T_TEXT("var r3 = Proxy.revocable(function () {}, {}); r3.revoke(); typeof r3.proxy", "function");
    T_TEXT("var r4 = Proxy.revocable({}, {}); r4.revoke(); r4.revoke(); 'again-ok'", "again-ok");
    T_THROWS("var r5 = Proxy.revocable([], {}); r5.revoke(); Array.isArray(r5.proxy)", "TypeError");
    T_THROWS("var r6 = Proxy.revocable({}, {}); r6.revoke(); Object.keys(r6.proxy)", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ traps */

EMB_TEST(proxy_get_set_has_delete_traps)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var pg = new Proxy({a: 1}, { get(t, k, r) { return k === 'a' ? 42 : Reflect.get(t, k, r); } });"
           " pg.a + ':' + pg.b",
           "42:undefined");
    T_TEXT("var ph = new Proxy({}, { has(t, k) { return k === 'x'; } }); ('x' in ph) + ':' + ('y' in ph)",
           "true:false");
    T_TEXT("var log = []; var ps = new Proxy({}, { set(t, k, v, r) { log.push(k + '=' + v); t[k] = v; return true; } });"
           " ps.z = 5; log.join() + ':' + ps.z",
           "z=5:5");
    T_TEXT("var pd = new Proxy({a: 1}, { deleteProperty(t, k) { return false; } });"
           " String(delete pd.a) + ':' + ('a' in pd)",
           "false:true");
    T_TEXT("var pdel = []; var pd2 = new Proxy({a: 1}, { deleteProperty(t, k) { pdel.push(k); return delete t[k]; } });"
           " delete pd2.a; pdel.join() + ':' + ('a' in pd2)",
           "a:false");
    env_free(e);
}

EMB_TEST(proxy_own_keys_and_descriptors)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var pk = new Proxy({a: 1, b: 2}, { ownKeys(t) { return ['b', 'a']; } }); Object.keys(pk).join()",
           "b,a");
    T_THROWS("Reflect.ownKeys(new Proxy({}, { ownKeys() { return ['a', 'a']; } }))", "TypeError");
    T_THROWS("Reflect.ownKeys(new Proxy({}, { ownKeys() { return [1]; } }))", "TypeError");
    T_THROWS("var tk = {}; Object.defineProperty(tk, 'x', { value: 1 });"
             " Reflect.ownKeys(new Proxy(tk, { ownKeys() { return []; } }))",
             "TypeError");
    T_THROWS("Reflect.ownKeys(new Proxy(Object.preventExtensions({a: 1}), { ownKeys() { return ['a', 'b']; } }))",
             "TypeError");
    T_THROWS("var gt = {}; Object.defineProperty(gt, 'x', { value: 1, configurable: false });"
             " Object.getOwnPropertyDescriptor(new Proxy(gt, { getOwnPropertyDescriptor() { return undefined; } }), 'x')",
             "TypeError");
    T_TEXT("JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy({}, {"
           " getOwnPropertyDescriptor() { return { value: 7, configurable: true }; } }), 'q'))",
           "{\"value\":7,\"writable\":false,\"enumerable\":false,\"configurable\":true}");
    T_THROWS("Object.defineProperty(new Proxy({}, { defineProperty() { return true; } }), 'x',"
             " { value: 1, configurable: false })",
             "TypeError");
    T_TEXT("Object.defineProperty(new Proxy({}, { defineProperty(t, k, d) { t[k] = d.value; return true; } }),"
           " 'w', { value: 9 }) !== undefined",
           "true");
    env_free(e);
}

EMB_TEST(proxy_prototype_and_extensibility_traps)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var pp = new Proxy({}, { getPrototypeOf() { return Array.prototype; } });"
           " Object.getPrototypeOf(pp) === Array.prototype",
           "true");
    T_TEXT("(new Proxy({}, { getPrototypeOf() { return Array.prototype; } })) instanceof Array", "true");
    T_THROWS("Object.getPrototypeOf(new Proxy(Object.preventExtensions({}),"
             " { getPrototypeOf() { return Array.prototype; } }))",
             "TypeError");
    T_TEXT("Reflect.setPrototypeOf(new Proxy({}, { setPrototypeOf() { return false; } }), null)", "false");
    T_THROWS("Object.setPrototypeOf(new Proxy({}, { setPrototypeOf() { return false; } }), null)", "TypeError");
    T_THROWS("Object.isExtensible(new Proxy({}, { isExtensible() { return false; } }))", "TypeError");
    T_TEXT("Object.isExtensible(new Proxy({}, { isExtensible(t) { return Reflect.isExtensible(t); } }))", "true");
    T_THROWS("Object.preventExtensions(new Proxy({}, { preventExtensions() { return true; } }))", "TypeError");
    T_TEXT("Reflect.preventExtensions(new Proxy({}, { preventExtensions() { return false; } }))", "false");
    env_free(e);
}

EMB_TEST(proxy_call_and_construct_traps)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var pa = new Proxy(function (a, b) { return a + b; },"
           " { apply(t, th, args) { return t.apply(th, args) * 2; } }); pa(2, 5)",
           "14");
    T_TEXT("var pc = new Proxy(function F() { this.x = 1; }, { construct(t, args, nt) { return { y: args[0] }; } });"
           " new pc(9).y",
           "9");
    T_THROWS("new (new Proxy(() => 1, {}))()", "TypeError");
    T_THROWS("var pn = new Proxy(function () {}, { construct() { return 1; } }); new pn()", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ IsArray, JSON, builtins */

EMB_TEST(proxy_is_array_json_and_builtins_see_through)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Array.isArray(new Proxy([], {}))", "true");
    T_TEXT("Array.isArray(new Proxy({}, {}))", "false");
    T_TEXT("Object.prototype.toString.call(new Proxy([], {}))", "[object Array]");
    T_TEXT("JSON.stringify(new Proxy({a: 1, b: [2]}, {}))", "{\"a\":1,\"b\":[2]}");
    T_TEXT("JSON.stringify(new Proxy([1, 2], {}))", "[1,2]");
    T_TEXT("JSON.stringify(new Proxy({a: 1}, { get(t, k) { return k === 'a' ? 5 : t[k]; } }))", "{\"a\":5}");
    T_TEXT("Array.prototype.join.call(new Proxy([1, 2, 3], {}))", "1,2,3");
    /* Array fast paths must not skip the get trap on a proxy. */
    T_TEXT("var pj = new Proxy([1, 2, 3], { get(t, k, r) { return k === '0' ? 'x' : Reflect.get(t, k, r); } });"
           " Array.prototype.join.call(pj)",
           "x,2,3");
    T_TEXT("[].concat(new Proxy([7, 8], {})).join()", "7,8");
    T_TEXT("[1, [2, [3]]].flat(Infinity).length", "3");
    env_free(e);
}

/* ------------------------------------------------------------------ prototype chains */

EMB_TEST(proxy_as_prototype_forwards_get_set_has)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var proto = new Proxy({}, { get(t, k, r) { return k === 'hello' ? 'from proxy' : undefined; } });"
           " var o = Object.create(proto); o.hello",
           "from proxy");
    T_TEXT("var log2 = []; var proto2 = new Proxy({}, { set(t, k, v, r) { log2.push(k); return true; } });"
           " var o2 = Object.create(proto2); o2.q = 1; log2.join()",
           "q");
    T_TEXT("var pp2 = new Proxy({}, { has(t, k) { return k === 'zz'; } }); var oo = Object.create(pp2);"
           " ('zz' in oo) + ':' + ('yy' in oo)",
           "true:false");
    /* Object.prototype.isPrototypeOf walks the argument's chain with [[GetPrototypeOf]]: the proxy's
     * trap runs once, and Array.prototype's chain then reaches Object.prototype. */
    T_TEXT("var seen = 0; var pq = new Proxy({}, { getPrototypeOf() { seen++; return Array.prototype; } });"
           " Object.prototype.isPrototypeOf.call(Object.prototype, pq) + ':' + seen",
           "true:1");
    env_free(e);
}

/* ------------------------------------------------------------------ invariants */

EMB_TEST(proxy_get_set_invariants)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_THROWS("var t1 = {}; Object.defineProperty(t1, 'k', { value: 1 });"
             " new Proxy(t1, { get() { return 2; } }).k",
             "TypeError");
    T_TEXT("var t1b = {}; Object.defineProperty(t1b, 'k', { value: 1 });"
           " new Proxy(t1b, { get() { return 1; } }).k",
           "1");
    T_THROWS("var t2 = {}; Object.defineProperty(t2, 'k', { value: 1 });"
             " new Proxy(t2, { set() { return true; } }).k = 5",
             "TypeError");
    T_TEXT("var p3 = new Proxy({}, { set() { return false; } }); p3.x = 1; String(p3.x)", "undefined");
    T_THROWS("var t3 = {}; Object.defineProperty(t3, 'g', { get: undefined });"
             " new Proxy(t3, { get() { return 1; } }).g",
             "TypeError");
    T_THROWS("var t3s = {}; Object.defineProperty(t3s, 'g', { set: undefined });"
             " new Proxy(t3s, { set() { return true; } }).g = 1",
             "TypeError");
    env_free(e);
}

EMB_TEST(proxy_has_delete_invariants)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_THROWS("var t4 = {}; Object.defineProperty(t4, 'h', { value: 1 });"
             " 'h' in new Proxy(t4, { has() { return false; } })",
             "TypeError");
    T_THROWS("var t5 = Object.preventExtensions({ c: 1 }); 'c' in new Proxy(t5, { has() { return false; } })",
             "TypeError");
    T_TEXT("var t5b = Object.preventExtensions({ c: 1 }); 'zz' in new Proxy(t5b, { has() { return false; } })",
           "false");
    T_THROWS("var t6 = {}; Object.defineProperty(t6, 'd', { value: 1 });"
             " delete new Proxy(t6, { deleteProperty() { return true; } }).d",
             "TypeError");
    T_THROWS("var t7 = Object.preventExtensions({ d: 1 });"
             " delete new Proxy(t7, { deleteProperty() { return true; } }).d",
             "TypeError");
    env_free(e);
}

EMB_TEST(proxy_own_keys_symbols_and_pins)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var sy = Symbol('s'); Reflect.ownKeys(new Proxy({}, { ownKeys() { return [sy]; } }))[0] === sy",
           "true");
    T_TEXT("Object.getOwnPropertySymbols(new Proxy({}, { ownKeys() { return [Symbol.iterator, 'x']; } })).length",
           "1");
    T_THROWS("var sd = Symbol('d'); Reflect.ownKeys(new Proxy({}, { ownKeys() { return [sd, sd]; } }))",
             "TypeError");
    T_THROWS("Reflect.ownKeys(new Proxy({}, { ownKeys() { return 1; } }))", "TypeError");
    /* A symbol returned only by a trap must survive the collections that run after the call. */
    T_TEXT("var ks = Reflect.ownKeys(new Proxy({}, { ownKeys() { return [Symbol('v')]; } }));"
           " for (var i = 0; i < 200000; i++) { var junk = { a: i }; } ks[0].toString()",
           "Symbol(v)");
    /* A trap that calls ownKeys on its own proxy: the nested result must not disturb the outer one. */
    T_TEXT("var inner = false; var pn2 = new Proxy({}, { ownKeys(t) {"
           " if (!inner) { inner = true; Reflect.ownKeys(pn2); } return [Symbol('n')]; } });"
           " var nk = Reflect.ownKeys(pn2); nk[0].toString()",
           "Symbol(n)");
    env_free(e);
}

/* ------------------------------------------------------------------ revoked, bind, iteration */

EMB_TEST(proxy_revoked_call_construct_bind)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_THROWS("var rc = Proxy.revocable(function () {}, {}); rc.revoke(); new rc.proxy()", "TypeError");
    T_THROWS("var ra = Proxy.revocable(function () {}, {}); ra.revoke(); ra.proxy()", "TypeError");
    T_THROWS("var rb = Proxy.revocable(function () {}, {}); rb.revoke(); rb.proxy.bind(null)", "TypeError");
    env_free(e);
}

EMB_TEST(proxy_bind_uses_target_prototype)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var pf = new Proxy(function (x) { return x + 1; }, {}); var bd = Function.prototype.bind.call(pf, null, 1);"
           " (bd instanceof Function) + ':' + typeof bd.call + ':' + bd()",
           "true:function:2");
    T_TEXT("var pg = new Proxy(function () {}, { getPrototypeOf() { return Array.prototype; } });"
           " var bg = pg.bind(null); (bg instanceof Array) + ':' + (bg instanceof Function)",
           "true:false");
    env_free(e);
}

EMB_TEST(proxy_iteration_fast_paths_see_proxy)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Object.entries(new Proxy([7, 8], {})).join('|')", "0,7|1,8");
    T_TEXT("Object.values(new Proxy({ a: 1, b: 2 }, { ownKeys(t) { return ['b', 'a']; } })).join()", "2,1");
    T_TEXT("Object.entries(new Proxy({ a: 1 }, { get() { return 9; } })).join()", "a,9");
    /* for-in walks the chain with [[GetPrototypeOf]], so a proxy in the chain supplies its prototype. */
    T_TEXT("var fi = []; for (var k in new Proxy(Object.create({ a: 1 }), {})) fi.push(k); fi.join()", "a");
    T_TEXT("var fp = []; for (var k in new Proxy({}, { getPrototypeOf() { return { inh: 1 }; } })) fp.push(k);"
           " fp.join()",
           "inh");
    T_TEXT("var fq = []; for (var k in Object.create(new Proxy({ z: 1 }, {}))) fq.push(k); fq.join()", "z");
    env_free(e);
}

/* ------------------------------------------------------------------ review round 3 */

EMB_TEST(proxy_function_to_string_is_unobservable)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var gc = 0; var pt = new Proxy(function () {}, { getOwnPropertyDescriptor(t, k) { gc++; return Reflect.getOwnPropertyDescriptor(t, k); } });"
           " Function.prototype.toString.call(pt); gc",
           "0");
    T_TEXT("var rv = Proxy.revocable(function () {}, {}); rv.revoke();"
           " var ts = Function.prototype.toString.call(rv.proxy); (ts.indexOf('[native code]') > 0) + ':' + (ts.indexOf('function') === 0)",
           "true:true");
    env_free(e);
}

EMB_TEST(proxy_trap_and_invariant_gaps)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_THROWS("new Proxy({}, { get: 1 }).x", "TypeError");
    T_THROWS("new Proxy({}, { has: {} }); 'x' in new Proxy({}, { has: {} })", "TypeError");
    /* A proxy as the receiver of Reflect.set / of an inherited setter reaches its defineProperty trap. */
    T_TEXT("var dl = []; var rp = new Proxy({}, { defineProperty(t, k, d) { dl.push(k); return Reflect.defineProperty(t, k, d); } });"
           " Reflect.set({}, 'q', 1, rp); dl.join()",
           "q");
    T_TEXT("var dl2 = []; var rp2 = new Proxy({}, { defineProperty(t, k, d) { dl2.push(k); return Reflect.defineProperty(t, k, d); } });"
           " var sp = Object.create({ set z(v) { Reflect.set(this, 'w', v, this); } }); Reflect.set(sp, 'z', 5, rp2); dl2.join()",
           "w");
    /* A proxy whose target is a proxy. */
    T_TEXT("var pp = new Proxy(new Proxy({ a: 1 }, { get(t, k) { return t[k] + 1; } }), {}); pp.a", "2");
    T_TEXT("var pq = new Proxy(new Proxy({ a: 1 }, {}), { get(t, k) { return t[k] + 10; } }); pq.a", "11");
    /* getOwnPropertyDescriptor invariants. */
    T_THROWS("Object.getOwnPropertyDescriptor(new Proxy({}, { getOwnPropertyDescriptor() { return { value: 1, configurable: false }; } }), 'x')",
             "TypeError");
    T_THROWS("var ne = Object.preventExtensions({}); Object.getOwnPropertyDescriptor(new Proxy(ne, { getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } }), 'x')",
             "TypeError");
    T_THROWS("var wr = {}; Object.defineProperty(wr, 'x', { value: 1, writable: true, configurable: false });"
             " Object.getOwnPropertyDescriptor(new Proxy(wr, { getOwnPropertyDescriptor() { return { value: 1, writable: false, configurable: false }; } }), 'x')",
             "TypeError");
    T_TEXT("var ok = {}; Object.defineProperty(ok, 'x', { value: 1, configurable: true });"
           " JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy(ok, { getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } }), 'x'))",
           "{\"value\":1,\"writable\":false,\"enumerable\":false,\"configurable\":true}");
    env_free(e);
}

EMB_TEST(proxy_own_keys_large_and_many_symbols)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A huge claimed length with a bad first element fails at the element, not on the length. */
    T_THROWS("Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 1e7 }; } }))", "TypeError");
    T_THROWS("Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 1e12 }; } }))", "TypeError");
    T_TEXT("var many = []; for (var i = 0; i < 3000; i++) many.push(Symbol('s' + i));"
           " var pm = new Proxy({}, { ownKeys() { return many; } }); var ks = Reflect.ownKeys(pm);"
           " ks.length + ':' + (ks[2999] === many[2999]) + ':' + (Reflect.ownKeys(pm)[5] === many[5])",
           "3000:true:true");
    env_free(e);
}

/* ------------------------------------------------------------------ stress */

EMB_TEST(proxy_gc_pressure_keeps_values)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var sum = 0; for (var i = 0; i < 100000; i++) {"
           " var p = new Proxy({ v: i }, { get(t, k) { return t[k] + 1; } }); sum += p.v; } sum",
           "5000050000");
    env_free(e);
}

EMB_TEST_MAIN()
