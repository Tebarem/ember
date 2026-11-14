/* Proxy trap caches (J5d): the get, set and has traps are found through a
 * per-proxy cache: one handler shape and an int32 slot per trap (builtins_proxy.c). Each case warms a
 * proxy, checks through embi_js_proxy_trap_entry that the entry is filled (or, for a handler the cache
 * must not key on, that it stays empty), then changes the handler (a value write, an accessor, a
 * deletion, an inherited trap, a non-callable value, a dictionary handler, a revoked proxy) and checks
 * that the trap run is the one the generic lookup finds. A value write keeps the handler shape, so the
 * entry is the same after the write: the hit reads the new value from the slot. Each case frees its
 * runtime and checks that embi_mem_live returns to its baseline. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/ic.h"
#include "js/heap.h"
#include "js/interp.h"

/* The trap ids of builtins_proxy.c (enum TRAP_GET, TRAP_SET, TRAP_HAS). */
#define TRAP_GET_ID 0u
#define TRAP_SET_ID 1u
#define TRAP_HAS_ID 2u

/* Live bytes after a create/free cycle: lazily built process state is in place before a case's own
 * runtime is measured against it. */
static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

/* Whether the global proxy named px has a filled cache entry for trap id. On success, *sh is the
 * handler shape the entry was read under and *slot the cached trap slot. */
static bool trap_entry_filled(es6_env e, const char *px, uint32_t id, const embi_js_shape **sh,
                              int32_t *slot)
{
    *sh = nullptr;
    *slot = -1;
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, px));
    if (!embi_js_is_object(v)) {
        return false;
    }
    if (!embi_js_proxy_trap_entry(embi_js_as_object(v), id, sh, slot)) {
        return false;
    }
    return *sh != nullptr && *slot >= 0;
}

/* A value write to the handler's trap is seen by the next call. The entry is filled by the warm-up,
 * and the value write keeps the handler shape, so the entry is unchanged by the hit. */
EMB_TEST(js_ic_proxy_value_write_same_shape)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var h = {get: function (t, k) { return 'h1'; }};"
            "var px = new Proxy({}, h); function gp(o) { return o.foo; }"
            "var r = [gp(px), gp(px)]; r.join() === 'h1,h1'",
            true);
    const embi_js_shape *sh1 = nullptr;
    int32_t sl1 = -1;
    EMB_CHECK(trap_entry_filled(e, "px", TRAP_GET_ID, &sh1, &sl1));
    E6_BOOL(e,
            "h.get = function (t, k) { return 'h2'; };"
            "r.push(gp(px)); r.join() === 'h1,h1,h2'",
            true);
    const embi_js_shape *sh2 = nullptr;
    int32_t sl2 = -1;
    EMB_CHECK(trap_entry_filled(e, "px", TRAP_GET_ID, &sh2, &sl2));
    EMB_CHECK(sh1 == sh2 && sl1 == sl2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The trap turned into an accessor: its getter result is the trap, found on the next lookup. */
EMB_TEST(js_ic_proxy_accessor_trap)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var h2 = {get: function (t, k) { return 'a1'; }};"
            "var px2 = new Proxy({}, h2); function gp2(o) { return o.foo; }"
            "var r2 = [gp2(px2), gp2(px2)];"
            "Object.defineProperty(h2, 'get', {configurable: true,"
            "  get: function () { return function (t, k) { return 'a2'; }; }});"
            "r2.push(gp2(px2)); r2.join() === 'a1,a1,a2'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The trap deleted: the operation forwards to the target. */
EMB_TEST(js_ic_proxy_trap_deleted_forwards)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var tg = {foo: 'target'}; var h3 = {get: function () { return 'trap'; }};"
            "var px3 = new Proxy(tg, h3); function gp3(o) { return o.foo; }"
            "var r3 = [gp3(px3), gp3(px3)]; delete h3.get; r3.push(gp3(px3));"
            "r3.join() === 'trap,trap,target'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A non-callable trap is a TypeError, and a nullish trap forwards. */
EMB_TEST(js_ic_proxy_trap_value_kinds)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var tg4 = {foo: 'tgt'}; var h4 = {get: function () { return 'trap'; }};"
            "var px4 = new Proxy(tg4, h4); function gp4(o) { return o.foo; }"
            "var r4 = [gp4(px4)]; h4.get = 5;"
            "var bad = false; try { gp4(px4); } catch (x) { bad = x instanceof TypeError; }"
            "h4.get = undefined; r4.push(gp4(px4));"
            "r4.join() === 'trap,tgt' && bad",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* An inherited trap: the own cache is absent, the generic lookup finds the prototype's trap, and
 * removing it from the prototype is seen. An inherited trap is never cached. */
EMB_TEST(js_ic_proxy_inherited_trap)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var base5 = {get: function () { return 'inh'; }}; var h5 = Object.create(base5);"
            "var px5 = new Proxy({foo: 'tg'}, h5); function gp5(o) { return o.foo; }"
            "var r5 = [gp5(px5), gp5(px5)];"
            "delete base5.get; r5.push(gp5(px5));"
            "r5.join() === 'inh,inh,tg'",
            true);
    const embi_js_shape *sh = nullptr;
    int32_t sl = -1;
    EMB_CHECK(!trap_entry_filled(e, "px5", TRAP_GET_ID, &sh, &sl));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* set and has traps are cached by their own entries: each one is found, and the handler's changes
 * are seen. */
EMB_TEST(js_ic_proxy_set_has_traps)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var log6 = []; var tg6 = {};"
            "var h6 = {set: function (t, k, v) { log6.push('s1'); t[k] = v; return true; },"
            "          has: function (t, k) { log6.push('h1'); return true; }};"
            "var px6 = new Proxy(tg6, h6); function st(o, v) { o.x = v; return 'x' in o; }"
            "var ok6 = [st(px6, 1), st(px6, 2)];"
            "h6.set = function (t, k, v) { log6.push('s2'); return true; };"
            "ok6.push(st(px6, 3));"
            "ok6.join() === 'true,true,true' && log6.join() === 's1,h1,s1,h1,s2,h1' && tg6.x === 2",
            true);
    const embi_js_shape *sh = nullptr;
    int32_t sl = -1;
    EMB_CHECK(trap_entry_filled(e, "px6", TRAP_SET_ID, &sh, &sl));
    EMB_CHECK(trap_entry_filled(e, "px6", TRAP_HAS_ID, &sh, &sl));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A handler with many properties is in dictionary mode. It is not a prototype, so the cache does not
 * key on it: the traps are still found, a change to them is seen, and no entry is filled. */
EMB_TEST(js_ic_proxy_dict_handler)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var h7 = {get: function () { return 'd1'; }};"
            "for (var i = 0; i < 100; i++) h7['p' + i] = i;"
            "var px7 = new Proxy({}, h7); function gp7(o) { return o.foo; }"
            "var r7 = [gp7(px7), gp7(px7)]; h7.get = function () { return 'd2'; };"
            "r7.push(gp7(px7)); r7.join() === 'd1,d1,d2'",
            true);
    const embi_js_shape *sh = nullptr;
    int32_t sl = -1;
    EMB_CHECK(!trap_entry_filled(e, "px7", TRAP_GET_ID, &sh, &sl));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A dictionary-mode handler that is also a prototype (Object.create makes it one): its shape renews on
 * add, delete and attribute change, so the cache keys on it. A value write keeps the entry, and a
 * delete followed by a re-add renews the shape, so the entry is refilled under the new shape. */
EMB_TEST(js_ic_proxy_dict_proto_handler)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var h8 = {get: function () { return 'd1'; }};"
            "for (var i = 0; i < 100; i++) h8['p' + i] = i;"
            "var user8 = Object.create(h8);"
            "var px8 = new Proxy({}, h8); function gp8(o) { return o.foo; }"
            "var r8 = [gp8(px8), gp8(px8)];"
            "r8.join() === 'd1,d1'",
            true);
    const embi_js_shape *sh1 = nullptr;
    int32_t sl1 = -1;
    EMB_CHECK(trap_entry_filled(e, "px8", TRAP_GET_ID, &sh1, &sl1));
    E6_BOOL(e,
            "h8.get = function () { return 'd2'; }; r8.push(gp8(px8)); r8.join() === 'd1,d1,d2'",
            true);
    const embi_js_shape *sh2 = nullptr;
    int32_t sl2 = -1;
    EMB_CHECK(trap_entry_filled(e, "px8", TRAP_GET_ID, &sh2, &sl2));
    EMB_CHECK(sh1 == sh2 && sl1 == sl2);
    E6_BOOL(e,
            "delete h8.get; h8.get = function () { return 'd3'; };"
            "r8.push(gp8(px8)); r8.join() === 'd1,d1,d2,d3'",
            true);
    const embi_js_shape *sh3 = nullptr;
    int32_t sl3 = -1;
    EMB_CHECK(trap_entry_filled(e, "px8", TRAP_GET_ID, &sh3, &sl3));
    EMB_CHECK(sh3 != sh1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A revoked proxy throws, whether or not its trap was cached. */
EMB_TEST(js_ic_proxy_revoked_throws)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var rv = Proxy.revocable({}, {get: function () { return 'ok'; }});"
            "function gr(o) { return o.foo; } var rr = [gr(rv.proxy)];"
            "rv.revoke(); var thrown = false;"
            "try { gr(rv.proxy); } catch (x) { thrown = x instanceof TypeError; }"
            "rr[0] === 'ok' && thrown",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Two proxies with different handlers at the same site: each proxy finds its own trap. The two
 * handlers are built by the same literal shape, so they share a shape, and the cache of each proxy
 * still reads the trap from its own handler. */
EMB_TEST(js_ic_proxy_two_handlers_one_site)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var ha = {get: function () { return 'A'; }}; var hb = {get: function () { return 'B'; }};"
            "var pa = new Proxy({}, ha); var pb = new Proxy({}, hb); function gq(o) { return o.foo; }"
            "var rq = [gq(pa), gq(pb), gq(pa), gq(pb)]; rq.join() === 'A,B,A,B'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Generational mode: an old proxy caches a handler shape that is younger than the proxy. The cache
 * slot must be remembered (write barrier on the boxed shape), or a minor collection frees the shape
 * while the slot still points at it and the next trace of the proxy touches freed memory (ASan). */
EMB_TEST(js_ic_proxy_cache_shape_generational)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_gc_set_generational(e.rt, true);
    E6_BOOL(e,
            "var h = {get: function (t, k) { return 'a'; }};"
            "var px = new Proxy({}, h); function gp(o) { return o.foo; } gp(px) === 'a'",
            true);
    embi_js_gc_minor(e.rt);
    embi_js_gc_minor(e.rt);
    for (int i = 0; i < 4; i++) {
        /* A new handler shape each round, cached by the old proxy, then dropped by the handler. */
        char src[96];
        SDL_snprintf(src, sizeof src, "h.p%d = 1; gp(px) === 'a'", i);
        E6_BOOL(e, src, true);
        embi_js_gc_minor(e.rt);
    }
    embi_js_gc(e.rt);
    E6_BOOL(e, "gp(px) === 'a' && px.bar === 'a'", true);
    embi_js_gc_set_generational(e.rt, false);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
