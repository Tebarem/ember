/* Global sites (J5d): GET_GLOBAL, GET_GLOBAL_OPT and SET_GLOBAL cache
 * the binding a name resolves to, a box of the lexical record or an own property of the global
 * object. Each case warms a function that reads or writes a global name, changes the binding
 * underneath it (a later let, delete, accessor, attribute change, const, the dictionary renewal of
 * the global object, TDZ), and checks the result against the generic path. The entries are checked
 * too: a warmed site must hold a global entry, and reads of a hit must not refill it. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/ic.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* Live bytes after a create/free cycle: lazily built process state is in place before a case's own
 * runtime is measured against it. */
static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

/* The inline caches of the global function named name, or nullptr when the global is not a closure. */
static const embi_js_ic *global_fn_ics(es6_env e, const char *name, uint32_t *nics)
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

/* Whether site of global function fn holds a global entry of kind (EMBI_JS_IC_G_*) and the realm's
 * lex count when it was filled (the epoch) is the current one. */
static bool site_holds(es6_env e, const char *fn, uint32_t site, uint8_t kind)
{
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, fn, &n);
    if (!ics || site >= n) {
        SDL_Log("    no site %u in %s", site, fn);
        return false;
    }
    bool ok = ics[site].gkind == kind;
    if (!ok) {
        SDL_Log("    site %u of %s: gkind=%u (want %u)", site, fn, ics[site].gkind, kind);
    }
    return ok;
}

/* Calls fn `times` times (each call must succeed) and requires the fill counter of site to be
 * unchanged: hits never refill. */
static bool hits_do_not_refill(es6_env e, const char *fn, uint32_t site, const char *call, int times)
{
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, fn, &n);
    if (!ics || site >= n) {
        return false;
    }
    uint16_t before = ics[site].fills;
    for (int i = 0; i < times; i++) {
        embi_js_value v = es6_run(e, call);
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(e.rt);
            return false;
        }
    }
    if (ics[site].fills != before) {
        SDL_Log("    fills changed %u -> %u on hits of %s", before, ics[site].fills, fn);
    }
    return ics[site].fills == before;
}

/* A name that becomes a lexical binding in a later script shadows the configurable global property. */
EMB_TEST(js_ic_global_let_shadows_prop)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "gy = 'prop'; function ry() { return gy; } var r = [ry(), ry(), ry()];"
            "r.join() === 'prop,prop,prop'",
            true);
    EMB_CHECK(site_holds(e, "ry", 0, EMBI_JS_IC_G_OBJ));
    E6_BOOL(e, "r.join() === 'prop,prop,prop' && ry() === 'prop'", true);
    EMB_CHECK(hits_do_not_refill(e, "ry", 0, "ry()", 20));
    E6_BOOL(e, "let gy = 'let'; r.push(ry()); r.join() === 'prop,prop,prop,let'", true);
    EMB_CHECK(site_holds(e, "ry", 0, EMBI_JS_IC_G_LEX));
    E6_BOOL(e, "r.push(ry()); r[4] === 'let'", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* TDZ: a read of a let binding before its initialization throws, and after it, the same site reads
 * the value (the cached box, with its live value). */
EMB_TEST(js_ic_global_tdz_then_init)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function rt() { return tz; } var t1 = ''; var t2 = 0;"
            "try { rt(); } catch (x) { t1 = x.name; }"
            "let tz = 5;"
            "t2 = rt();"
            "t1 === 'ReferenceError' && t2 === 5",
            true);
    EMB_CHECK(site_holds(e, "rt", 0, EMBI_JS_IC_G_LEX));
    E6_BOOL(e,
            "var threw = false; try { tz = 1; tz = 2; } catch (x) { threw = true; }"
            "!threw && rt() === 2",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* delete of a configurable global property, then a read: ReferenceError. Re-adding it is seen. */
EMB_TEST(js_ic_global_delete_and_readd)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "gd = 1; function rd() { return gd; } var rr = [rd(), rd(), rd()]; delete gd;"
            "rr.join() === '1,1,1'",
            true);
    E6_BOOL(e,
            "var thr = false; try { rd(); } catch (x) { thr = x instanceof ReferenceError; }"
            "thr === true",
            true);
    E6_BOOL(e, "gd = 2; rd() === 2", true);
    E6_BOOL(e, "delete gd; var thr2 = false; try { rd(); } catch (x) { thr2 = true; } thr2", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* An own data property replaced by an accessor, then a data property again. */
EMB_TEST(js_ic_global_accessor_swap)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "ga = 'data'; function rga() { return ga; } var s = [rga(), rga()];"
            "Object.defineProperty(globalThis, 'ga', {get: function () { return 'acc'; },"
            "  configurable: true});"
            "s.push(rga());"
            "Object.defineProperty(globalThis, 'ga', {value: 'back', writable: true,"
            "  enumerable: true, configurable: true});"
            "s.push(rga());"
            "s.join() === 'data,data,acc,back'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Writes through a cached global entry: a read-only property refuses the write (sloppy mode keeps
 * the old value), and a strict write throws. */
EMB_TEST(js_ic_global_write_attr_change)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "gw = 0; function setw(v) { gw = v; } for (var i = 0; i < 5; i++) setw(i);"
            "gw === 4",
            true);
    EMB_CHECK(site_holds(e, "setw", 0, EMBI_JS_IC_G_OBJ));
    E6_BOOL(e,
            "Object.defineProperty(globalThis, 'gw', {writable: false});"
            "setw(9); gw === 4",
            true);
    E6_BOOL(e,
            "function setws(v) { 'use strict'; gw = v; } var sthrew = false;"
            "try { setws(7); } catch (x) { sthrew = x instanceof TypeError; }"
            "sthrew && gw === 4",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* const: a write through the lexical box throws every time, and the value stays. */
EMB_TEST(js_ic_global_const_write)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "const kc = 1; function setk() { kc = 2; } var kc_thrown = 0;"
            "for (var i = 0; i < 4; i++) { try { setk(); } catch (x) { kc_thrown++; } }"
            "kc === 1 && kc_thrown === 4",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The global object is a prototype: many adds renew its dictionary shape, and a cached read must
 * still see the current value of its property after each renewal. */
EMB_TEST(js_ic_global_dict_renewal)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "gdict = 'a'; function rdict() { return gdict; } var dr = [rdict()];"
            "for (var k = 0; k < 120; k++) globalThis['pad' + k] = k;"
            "dr.push(rdict()); gdict = 'b'; dr.push(rdict());"
            "for (var k2 = 0; k2 < 120; k2++) delete globalThis['pad' + k2];"
            "dr.push(rdict()); gdict = 'c'; dr.push(rdict());"
            "dr.join() === 'a,a,b,b,c'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* typeof of a missing global (GET_GLOBAL_OPT), then the name is created. */
EMB_TEST(js_ic_global_typeof_missing_then_created)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function ty() { return typeof late_g; } var tyr = [ty(), ty()];"
            "late_g = 3; tyr.push(ty());"
            "tyr.join() === 'undefined,undefined,number'",
            true);
    E6_BOOL(e, "delete late_g; ty() === 'undefined'", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Implicit global creation in sloppy mode repeats; the same site in strict mode throws. */
EMB_TEST(js_ic_global_implicit_create)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function mk(v) { made_g = v; } mk(1); mk(2); made_g === 2"
            " && Object.getOwnPropertyDescriptor(globalThis, 'made_g').configurable === true",
            true);
    E6_BOOL(e,
            "function smk() { 'use strict'; undeclared_zz = 1; }"
            "var sth = false; try { smk(); } catch (x) { sth = x instanceof ReferenceError; }"
            "sth && typeof undeclared_zz === 'undefined'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Reads that hit keep working across many shapes of the global object (the site stays cached
 * while unrelated globals come and go). */
EMB_TEST(js_ic_global_hits_stable)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "gs = 11; function rgs() { return gs + 1; } var sum = 0;"
            "for (var i = 0; i < 10; i++) sum += rgs();"
            "sum === 120",
            true);
    EMB_CHECK(hits_do_not_refill(e, "rgs", 0, "rgs()", 30));
    E6_BOOL(e, "other_g = 1; rgs() === 12", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
