/* Primitive receivers (J5d): str.length is answered from the string, and
 * the method loads on string, number, boolean, symbol and bigint receivers go through the site cache
 * on the intrinsic prototype, with no wrapper allocated. Each case warms a site on a primitive, changes
 * the prototype chain underneath it, and checks the result against the generic lookup (GetV on the
 * wrapper), which the expected values spell out. Hits are checked through the site's fill counter: a
 * warm hit never refills, so the counter is unchanged across hits, and a changed chain refills it. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/ic.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>

/* Returned by fn_site_fills when the site does not exist. */
#define NO_SITE 0xffffffffu

/* The inline caches of the global function named name, or nullptr when it is not a closure. */
static const embi_js_ic *fn_ics(es6_env e, const char *name, uint32_t *nics)
{
    embi_js_value f = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    *nics = 0;
    if (!embi_js_is_object(f) || embi_js_as_object(f)->klass != &embi_js_class_closure) {
        return nullptr;
    }
    const embi_js_closure *cl = embi_js_as_object(f)->internal;
    const embi_js_code *c = (const embi_js_code *)embi_js_as_cell(cl->code);
    *nics = c->nics;
    return c->ics;
}

/* Whether the first site of fn has been filled (it is not uninitialized). */
static bool site0_filled(es6_env e, const char *fn)
{
    uint32_t n = 0;
    const embi_js_ic *ics = fn_ics(e, fn, &n);
    return ics && n > 0 && ics[0].state != 0;
}

/* The fill counter of site of the global function fn, or NO_SITE. */
static uint32_t fn_site_fills(es6_env e, const char *fn, uint32_t site)
{
    uint32_t n = 0;
    const embi_js_ic *ics = fn_ics(e, fn, &n);
    return ics && site < n ? ics[site].fills : NO_SITE;
}

/* Runs src `times` times and reports whether every run succeeded. */
static bool run_n(es6_env e, const char *src, int times)
{
    for (int i = 0; i < times; i++) {
        if (es6_run(e, src) == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(e.rt);
            return false;
        }
    }
    return true;
}

/* Warm hits of site 0 of fn (after one fill) leave its fill counter unchanged. */
static bool site0_hits_do_not_refill(es6_env e, const char *fn, const char *call)
{
    uint32_t f1 = fn_site_fills(e, fn, 0);
    if (f1 == NO_SITE || f1 == 0) {
        SDL_Log("    no fill on site 0 of %s (fills=%u)", fn, f1);
        return false;
    }
    if (!run_n(e, call, 50)) {
        return false;
    }
    uint32_t f2 = fn_site_fills(e, fn, 0);
    if (f2 != f1) {
        SDL_Log("    fills changed %u -> %u on hits of %s", f1, f2, fn);
    }
    return f2 == f1;
}

/* Method loads on a string receiver see a replaced method, and an added one. */
EMB_TEST(js_ic_prim_string_method_replace)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var s = 'abc'; function cm(x) { return x.toUpperCase(); } var pr = [];"
            "for (var i = 0; i < 3; i++) pr.push(cm(s));"
            "String.prototype.toUpperCase = function () { return 'X'; };"
            "pr.push(cm(s)); pr.join() === 'ABC,ABC,ABC,X'",
            true);
    EMB_CHECK(site0_filled(e, "cm"));
    E6_BOOL(e,
            "function lt(x) { return x.later(); }"
            "var lr = []; try { lt('a'); } catch (x) { lr.push(x.name); }"
            "String.prototype.later = function () { return 'L'; };"
            "lr.push(lt('a')); lr.join() === 'TypeError,L'",
            true);
    es6_env_free(e);
}

/* An own property added to the prototype (a field, not a method) is seen, and removing it is seen. */
EMB_TEST(js_ic_prim_proto_add_and_delete)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function gz(x) { return x.zz; } var zr = [gz('a'), gz('a')];"
            "String.prototype.zz = 'v'; zr.push(gz('a'));"
            "delete String.prototype.zz; zr.push(gz('a'));"
            "zr[0] === undefined && zr[1] === undefined && zr[2] === 'v' && zr[3] === undefined",
            true);
    es6_env_free(e);
}

/* A getter on a prototype receives the primitive itself. The getter is strict, so this is not a
 * boxed wrapper. */
EMB_TEST(js_ic_prim_getter_receiver)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "Object.defineProperty(String.prototype, 'tg', {configurable: true,"
            "  get: function () { 'use strict'; return typeof this; }});"
            "function gt(x) { return x.tg; } var gr = [gt('a'), gt('b')];"
            "Object.defineProperty(Number.prototype, 'tg', {configurable: true,"
            "  get: function () { 'use strict'; return typeof this; }});"
            "gr.push(gt(5)); gr.join() === 'string,string,number'",
            true);
    E6_BOOL(e, "gt('c') === 'string' && (true).tg === undefined", true);
    es6_env_free(e);
}

/* length on a string is the string length, and other objects keep their own length. */
EMB_TEST(js_ic_prim_length)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function ln(x) { return x.length; } var lr = [];"
            "lr.push(ln('ab')); lr.push(ln('abcd')); lr.push(ln([1, 2, 3]));"
            "lr.push(ln({length: 7})); lr.push(ln(function (a, b) {}));"
            "lr.push(ln('')); lr.join() === '2,4,3,7,2,0'",
            true);
    /* A string with a long run: the length is the code unit count. */
    E6_BOOL(e,
            "var big = ''; for (var i = 0; i < 1000; i++) big += 'xy';"
            "function lnb(x) { return x.length; } lnb(big) === 2000",
            true);
    es6_env_free(e);
}

/* Number, boolean, symbol and bigint receivers reach their prototype methods. */
EMB_TEST(js_ic_prim_other_kinds)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function tf(x) { return x.toFixed(2); } var kr = [];"
            "kr.push(tf(1.5)); kr.push(tf(2)); kr.join() === '1.50,2.00'",
            true);
    E6_BOOL(e,
            "function bs(x) { return x.toString(); } var br = [];"
            "br.push(bs(true)); br.push(bs(false)); br.join() === 'true,false'",
            true);
    E6_BOOL(e,
            "function sd(x) { return x.description; }"
            "sd(Symbol('q')) === 'q' && sd(Symbol()) === undefined",
            true);
    E6_BOOL(e,
            "function bg(x) { return x.toString(); } bg(7n) === '7' && bg(-12345678901234567890n)"
            " === '-12345678901234567890'",
            true);
    es6_env_free(e);
}

/* One site sees objects and primitives of several types: each gets its own method. */
EMB_TEST(js_ic_prim_mixed_site)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function mx(x) { return x.toString(); } var mr = [];"
            "var o = {toString: function () { return 'obj'; }};"
            "for (var i = 0; i < 3; i++) { mr.push(mx('a')); mr.push(mx(o)); mr.push(mx(1)); }"
            "mr.push(mx(false)); mr.push(mx(10n));"
            "mr.join() === 'a,obj,1,a,obj,1,a,obj,1,false,10'",
            true);
    es6_env_free(e);
}

/* The primitive's method lookup throws for null and undefined, as before. */
EMB_TEST(js_ic_prim_nullish_throws)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function nf(x) { return x.foo; } var nr = [];"
            "try { nf(null); } catch (x) { nr.push(x.name); }"
            "try { nf(undefined); } catch (x) { nr.push(x.name); }"
            "nr.join() === 'TypeError,TypeError'",
            true);
    es6_env_free(e);
}

/* A getter that replaces the prototype's own method during a loop is seen on the next call. */
EMB_TEST(js_ic_prim_proto_swap_mid_loop)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function cc(x) { return x.charCodeAt(0); } var cr = [];"
            "for (var i = 0; i < 3; i++) cr.push(cc('A'));"
            "String.prototype.charCodeAt = function () { return 99; };"
            "cr.push(cc('A')); cr.join() === '65,65,65,99'",
            true);
    es6_env_free(e);
}

/* A method found on Object.prototype (two hops from a string: String.prototype, then Object.prototype)
 * is a cached hit. Warm hits leave the fill counter alone, and a value write to the holder is seen. */
EMB_TEST(js_ic_prim_chain_hit_value_write)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function hop(x) { return x.hasOwnProperty; } var hr = [];"
            "for (var i = 0; i < 3; i++) hr.push(hop('a') === Object.prototype.hasOwnProperty);"
            "hr.join() === 'true,true,true'",
            true);
    EMB_CHECK(site0_filled(e, "hop"));
    EMB_CHECK(site0_hits_do_not_refill(e, "hop", "hop('a')"));
    E6_BOOL(e,
            "Object.prototype.hasOwnProperty = function () { return 'replaced'; };"
            "hop('a')('x') === 'replaced' && 'a'.hasOwnProperty('x') === 'replaced'",
            true);
    es6_env_free(e);
}

/* A shadowing add on String.prototype (the first hop) for a name that resolved on Object.prototype
 * changes the chain at depth 1: the next lookup misses and refills, and finds the shadow. */
EMB_TEST(js_ic_prim_chain_shadow_add)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function hop2(x) { return x.hasOwnProperty; } var h2r = [];"
            "for (var i = 0; i < 3; i++) h2r.push(hop2('a') === Object.prototype.hasOwnProperty);"
            "h2r.join() === 'true,true,true'",
            true);
    uint32_t before = fn_site_fills(e, "hop2", 0);
    E6_BOOL(e,
            "String.prototype.hasOwnProperty = function () { return 'shadow'; };"
            "hop2('a')('x') === 'shadow'",
            true);
    EMB_CHECK(before != NO_SITE && fn_site_fills(e, "hop2", 0) != before);
    es6_env_free(e);
}

/* Object.setPrototypeOf on String.prototype (the first hop) replaces the chain under a cached hit at
 * depth 2: the next lookup finds the new prototype's method. */
EMB_TEST(js_ic_prim_setproto_string)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function hop3(x) { return x.hasOwnProperty; } var h3r = [];"
            "for (var i = 0; i < 3; i++) h3r.push(hop3('a') === Object.prototype.hasOwnProperty);"
            "h3r.join() === 'true,true,true'",
            true);
    E6_BOOL(e,
            "Object.setPrototypeOf(String.prototype, {hasOwnProperty: function () {"
            "  return 'swapped'; }});"
            "hop3('a')() === 'swapped' && 'a'.hasOwnProperty() === 'swapped'",
            true);
    es6_env_free(e);
}

/* Symbol and bigint prototype methods are cached hits: warm hits leave the fill counter alone, and a
 * replaced method on the prototype is seen. */
EMB_TEST(js_ic_prim_symbol_bigint_hits)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function sy(x) { return x.toString; } var syr = [];"
            "for (var i = 0; i < 3; i++) syr.push(sy(Symbol('q')) === Symbol.prototype.toString);"
            "function bi(x) { return x.toString; } var bir = [];"
            "for (var j = 0; j < 3; j++) bir.push(bi(7n) === BigInt.prototype.toString);"
            "syr.join() === 'true,true,true' && bir.join() === 'true,true,true'",
            true);
    EMB_CHECK(site0_filled(e, "sy"));
    EMB_CHECK(site0_filled(e, "bi"));
    EMB_CHECK(site0_hits_do_not_refill(e, "sy", "sy(Symbol('w'))"));
    EMB_CHECK(site0_hits_do_not_refill(e, "bi", "bi(8n)"));
    E6_BOOL(e,
            "Symbol.prototype.toString = function () { return 'sym'; };"
            "BigInt.prototype.toString = function () { return 'big'; };"
            "sy(Symbol('z'))() === 'sym' && bi(9n)() === 'big'",
            true);
    es6_env_free(e);
}

EMB_TEST_MAIN()
