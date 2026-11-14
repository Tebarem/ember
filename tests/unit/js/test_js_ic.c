/* Property inline caches (J5c). Every case warms a site so that its cache
 * fills, changes the world underneath it (shadowing, prototype changes, accessors, freezing,
 * dictionary renewal, hooks), and checks the cached read or write against the generic path. The
 * oracle is a computed-key access (o[k]), which never goes through an inline cache. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/compiler.h"
#include "js/ic.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* The inline caches of the global function named `name` (its own sites, not its callees'), or
 * nullptr when the global is not a closure. */
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

/* Checks site ic: its state, its entry count, and (when it has entries) entry 0's kind and depth.
 * Logs what the site holds on a mismatch, so a failing case shows the state it reached. */
static bool site_is(const embi_js_ic *ic, uint8_t state, uint8_t n, int kind, int depth)
{
    if (!ic) {
        return false;
    }
    bool ok = ic->state == state && ic->nentries == n;
    if (ok && n > 0) {
        ok = ic->e0.kind == kind && ic->e0.depth == depth;
    }
    if (!ok) {
        SDL_Log("    site: state=%u entries=%u kind=%u depth=%u (want state=%u entries=%u kind=%d depth=%d)",
                ic->state, ic->nentries, ic->e0.kind, ic->e0.depth, state, n, kind, depth);
    }
    return ok;
}

/* Runs `call` `times` times (a call that reads or writes the site), then requires the fill counter
 * of site `site` of global function fn to be unchanged: hits never refill. */
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

/* Reads of a cached site track shadowing and prototype changes made after the site filled. */
EMB_TEST(js_ic_shadow_and_proto)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var P = {x: 'proto'}; var o = Object.create(P); var out = [];"
            "function g(q) { return q.x; }"
            "for (var i = 0; i < 5; i++) out.push(g(o));"
            "o.x = 'own'; out.push(g(o));"
            "P.x = 'proto2'; out.push(g(o));"
            "delete o.x; out.push(g(o));"
            "out.join() === 'proto,proto,proto,proto,proto,own,own,proto2'",
            true);
    E6_BOOL(e,
            "var A = {m: function () { return 'A'; }}, B = {m: function () { return 'B'; }};"
            "var o2 = Object.create(A); var r = [];"
            "function call(q) { return q.m(); }"
            "for (var i = 0; i < 4; i++) r.push(call(o2));"
            "Object.setPrototypeOf(o2, B); r.push(call(o2));"
            "A.m = function () { return 'A2'; }; r.push(call(o2));"
            "B.m = function () { return 'B2'; }; r.push(call(o2));"
            "r.join() === 'A,A,A,A,B,B,B2'",
            true);
    /* A shadowing add on an intermediate prototype, between the receiver and the holder. */
    E6_BOOL(e,
            "var base = {z: 1}; var mid = Object.create(base); var leaf = Object.create(mid);"
            "function gz(q) { return q.z; } var r2 = [];"
            "for (var i = 0; i < 3; i++) r2.push(gz(leaf));"
            "mid.z = 2; r2.push(gz(leaf)); base.z = 3; r2.push(gz(leaf));"
            "Object.setPrototypeOf(mid, {z: 9}); r2.push(gz(leaf));"
            "delete mid.z; r2.push(gz(leaf));"
            "r2.join() === '1,1,1,2,2,2,9'",
            true);
    /* Reads through a deep chain, including the holder moving to a longer chain. */
    E6_BOOL(e,
            "var d0 = {deep: 'd'}; var d1 = Object.create(d0); var d2 = Object.create(d1);"
            "var d3 = Object.create(d2); var d4 = Object.create(d3); var d5 = Object.create(d4);"
            "function gd(q) { return q.deep; } var rr = [];"
            "for (var i = 0; i < 3; i++) rr.push(gd(d4));"
            "rr.push(gd(d5)); Object.setPrototypeOf(d4, {deep: 'x'}); rr.push(gd(d5));"
            "rr.push(gd(d1)); rr.join() === 'd,d,d,d,x,d'",
            true);
    es6_env_free(e);
}

/* Accessors: the getter is read from the slot on each use, so a redefinition is seen. */
EMB_TEST(js_ic_accessors)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var p = { get v() { return 'g1'; } }; var o = Object.create(p);"
            "function gv(q) { return q.v; } var r = [];"
            "for (var i = 0; i < 3; i++) r.push(gv(o));"
            "Object.defineProperty(p, 'v', {get: function () { return 'g2'; }});"
            "r.push(gv(o));"
            "Object.defineProperty(p, 'v', {value: 'd', writable: false, configurable: true});"
            "r.push(gv(o));"
            "r.join() === 'g1,g1,g1,g2,d'",
            true);
    /* An own data property redefined as an accessor, and back again. */
    E6_BOOL(e,
            "var w = {w: 'data'}; function gw(q) { return q.w; } var s = [];"
            "s.push(gw(w)); s.push(gw(w));"
            "Object.defineProperty(w, 'w', {get: function () { return 'acc'; }, configurable: true});"
            "s.push(gw(w));"
            "Object.defineProperty(w, 'w', {value: 'back', writable: true, configurable: true});"
            "s.push(gw(w)); s.join() === 'data,data,acc,back'",
            true);
    /* A getter that mutates its receiver during the read. */
    E6_BOOL(e,
            "var m = { get g() { this.h = (this.h || 0) + 1; return this.h; } };"
            "function gg(q) { return q.g; } var t = [];"
            "for (var i = 0; i < 3; i++) t.push(gg(m));"
            "t.join() === '1,2,3' && m.h === 3",
            true);
    /* Store through a prototype setter does not create an own property. */
    E6_BOOL(e,
            "var log = []; var P = { set s(v) { log.push(v); }, get s() { return 'g'; } };"
            "var o = Object.create(P); function ss(q, v) { q.s = v; }"
            "ss(o, 1); ss(o, 2); ss(o, 3);"
            "log.join() === '1,2,3' && o.s === 'g' && Object.keys(o).length === 0",
            true);
    es6_env_free(e);
}

/* Freezing and preventExtensions: stores that were cached must now fail. */
EMB_TEST(js_ic_freeze)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var o = {x: 1}; function sl(q, v) { q.x = v; return q.x; }"
            "function st(q, v) { 'use strict'; q.x = v; return q.x; }"
            "for (var i = 0; i < 3; i++) { sl(o, i); st(o, i); }"
            "Object.freeze(o); var sloppy = sl(o, 99);"
            "var threw = false; try { st(o, 99); } catch (err) { threw = err instanceof TypeError; }"
            "sloppy === 2 && threw && o.x === 2",
            true);
    E6_BOOL(e,
            "var p = {}; function ad(q) { q.newp = 1; return q.newp; }"
            "function sad(q) { 'use strict'; q.newp = 1; return q.newp; }"
            "for (var i = 0; i < 3; i++) { var fresh = {}; ad(fresh); sad(fresh); }"
            "Object.preventExtensions(p); var r1 = ad(p);"
            "var threw2 = false; try { sad(p); } catch (err) { threw2 = err instanceof TypeError; }"
            "r1 === undefined && threw2 && !('newp' in p)",
            true);
    /* Adds through a cached transition after sealing the receiver's shape. */
    E6_BOOL(e,
            "function mk(v) { var o = {}; o.a = v; o.b = v; return o; }"
            "var made = [mk(1), mk(2), mk(3)]; var k = {};"
            "function addc(q, v) { q.c = v; return q.c; }"
            "addc(made[0], 7); addc(made[1], 8);"
            "Object.seal(made[2]); var c3 = addc(made[2], 9);"
            "c3 === undefined && made[2].c === undefined && made[0].c === 7 && made[1].c === 8",
            true);
    es6_env_free(e);
}

/* Dictionary-mode prototypes: a key change renews the shape, a value change is seen live. */
EMB_TEST(js_ic_dict_proto)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var P = {}; for (var i = 0; i < 80; i++) P['k' + i] = i;"
            "var o = Object.create(P); function g70(q) { return q.k70; } var r = [];"
            "for (var j = 0; j < 3; j++) r.push(g70(o));"
            "P.k70 = 'value'; r.push(g70(o));"
            "for (var d = 0; d < 10; d++) delete P['k' + d];"
            "r.push(g70(o));"
            "P.k70 = 'after'; r.push(g70(o));"
            "delete P.k70; r.push(g70(o));"
            "P.k70 = 'readded'; r.push(g70(o));"
            "r.join() === '70,70,70,value,value,after,,readded'",
            true);
    /* A shadowing own add on the receiver of a dictionary prototype's member. */
    E6_BOOL(e,
            "var Q = {}; for (var n = 0; n < 70; n++) Q['m' + n] = n;"
            "var child = Object.create(Q); function gm(q) { return q.m3; } var rs = [];"
            "rs.push(gm(child)); child.m3 = 'own'; rs.push(gm(child)); delete child.m3;"
            "rs.push(gm(child)); Q.m3 = 'q'; rs.push(gm(child));"
            "rs.join() === '3,own,3,q'",
            true);
    es6_env_free(e);
}

/* Sites that see more shapes than the cache holds keep working on the generic path. */
EMB_TEST(js_ic_megamorphic)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function gx(q) { return q.x; } var objs = ["
            "{x: 1}, {a: 0, x: 2}, {a: 0, b: 0, x: 3}, {a: 0, b: 0, c: 0, x: 4},"
            "{a: 0, b: 0, c: 0, d: 0, x: 5}, {x: 6, a: 0}, Object.create({x: 7}), {y: 0}];"
            "var total = 0; for (var round = 0; round < 20; round++)"
            "  for (var i = 0; i < objs.length; i++) { var v = gx(objs[i]); if (v !== undefined) total += v; }"
            "total === 20 * (1 + 2 + 3 + 4 + 5 + 6 + 7)",
            true);
    es6_env_free(e);
}

/* A Proxy shares a shape with an ordinary object, so the class check decides. */
EMB_TEST(js_ic_proxy_receiver)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var target = {x: 'tgt'}; var px = new Proxy(target, {"
            "  get: function (t, k) { return 'trap:' + String(k); } });"
            "function gx(q) { return q.x; } var r = [];"
            "for (var i = 0; i < 3; i++) r.push(gx(target));"
            "r.push(gx(px)); r.push(gx({x: 'plain'}));"
            "r.join() === 'tgt,tgt,tgt,trap:x,plain'",
            true);
    E6_BOOL(e,
            "var seen = []; var hp = new Proxy({}, {"
            "  set: function (t, k, v) { seen.push(k + '=' + v); return true; } });"
            "function sx(q, v) { q.x = v; }"
            "var plain = {}; for (var i = 0; i < 3; i++) sx(plain, i);"
            "sx(hp, 'p'); seen.join() === 'x=p' && plain.x === 2",
            true);
    es6_env_free(e);
}

/* Array named stores: length is never cached as an ordinary slot. */
EMB_TEST(js_ic_array_length)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function setlen(a, n) { a.length = n; return a.length; }"
            "var warm = [1, 2]; setlen(warm, 1); setlen(warm, 2);"
            "var arr = [1, 2, 3, 4, 5]; var len = setlen(arr, 2);"
            "len === 2 && arr[3] === undefined && arr.length === 2",
            true);
    E6_BOOL(e,
            "var a2 = []; a2.named = 'n'; function nm(q) { return q.named; }"
            "nm(a2); nm(a2); a2.length = 3; a2.named2 = 'm';"
            "nm(a2) === 'n' && a2.length === 3 && a2.named2 === 'm'",
            true);
    E6_THROWS(e, "'use strict'; var f = Object.freeze([1]); function fl(q) { q.length = 0; } fl(f);",
              "TypeError");
    es6_env_free(e);
}

/* Object literal construction (DEFINE_FIELD) defines own properties, never through a setter. */
EMB_TEST(js_ic_define_field)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var seen = 0; Object.defineProperty(Object.prototype, 'qq', {"
            "  set: function (v) { seen++; }, configurable: true});"
            "function mk(v) { return {qq: v, rr: 1}; }"
            "var a = mk(1), b = mk(2), c = mk(3);"
            "delete Object.prototype.qq;"
            "a.qq === 1 && b.qq === 2 && c.qq === 3 && seen === 0"
            " && Object.keys(a).join() === 'qq,rr'",
            true);
    /* Literals of several shapes at one site, with a duplicate key. */
    E6_BOOL(e,
            "function lit(i) {"
            "  return i % 3 === 0 ? {a: i, b: i} : i % 3 === 1 ? {a: i, b: i, c: i, a2: 0}"
            "                                           : {b: i, a: i, a: -i}; }"
            "var sum = 0; for (var i = 0; i < 30; i++) { var o = lit(i); sum += o.a + o.b; }"
            "sum === 560",
            true);
    es6_env_free(e);
}

/* Classes and super property reads, through instances of a derived class. */
EMB_TEST(js_ic_classes_super)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "class A { get v() { return 'A.v'; } m() { return 'A.m'; } }"
            "class B extends A { constructor() { super(); this.f = 1; }"
            "  m() { return super.m() + '!' + super.v; } }"
            "function run(o) { return o.m() + o.v + o.f; }"
            "var out = []; var b = new B();"
            "for (var i = 0; i < 3; i++) out.push(run(b));"
            "A.prototype.m = function () { return 'A2'; };"
            "out.push(run(b));"
            "out.join() === 'A.m!A.vA.v1,A.m!A.vA.v1,A.m!A.vA.v1,A2!A.vA.v1'",
            true);
    es6_env_free(e);
}

/* Primitive receivers keep the generic path for named reads. */
EMB_TEST(js_ic_primitive_receivers)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function len(s) { return s.length; } var r = [];"
            "r.push(len('abc')); r.push(len([1, 2])); r.push(len({length: 9}));"
            "String.prototype.foo = 'bar'; function ff(s) { return s.foo; }"
            "r.push(ff('x')); r.push(ff(3));"
            "r.join() === '3,2,9,bar,' && ff(3) === undefined",
            true);
    E6_NUM(e, "var t = new Uint8Array(4); function tl(q) { return q.length; } tl(t) + tl(new Uint8Array(2))",
           6);
    es6_env_free(e);
}

/* Enough allocation to collect, with polymorphic sites refilling: the IC's strong references keep
 * their shapes alive, and results stay exact. */
EMB_TEST(js_ic_gc_churn)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e,
           "var total = 0; function gx(q) { return q.x; }"
           "for (var i = 0; i < 60000; i++) { var o = {}; o['k' + (i % 37)] = i; o.x = i;"
           "  total += gx(o); } total",
           1799970000.0);
    es6_env_free(e);
}

/* A method replaced on Object.prototype after the site filled. */
EMB_TEST(js_ic_proto_method_replaced)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function gt(q) { return q.toString === Object.prototype.toString; }"
            "var r = [gt({}), gt({}), gt([])];"
            "Object.prototype.toString = function () { return 'x'; };"
            "r.push(gt({})); r.join() === 'true,true,false,true'",
            true);
    es6_env_free(e);
}

/* Runtime teardown releases every inline cache, its side records and the shapes they held. */
EMB_TEST(js_ic_releases_all_memory)
{
    size_t before = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var P = {x: 1}; var Q = Object.create(P); function g(q) { return q.x; }"
            "function s(q, v) { q.x = v; return q; }"
            "for (var i = 0; i < 50; i++) { g(Q); g({}); s({}, i); s(Object.create(Q), i); }"
            "g(Q) === 1",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == before);
}

/* The disassembly names each field site's key and reports its cache state. */
EMB_TEST(js_ic_disasm_names_sites)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *src = "var o = {alpha: 1}; o.alpha;";
    embi_js_value code = embi_js_compile(e.r, (const uint8_t *)src, (uint32_t)strlen(src), "ic.js",
                                         false);
    EMB_REQUIRE(code != EMBI_JS_EXCEPTION);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_disasm((const embi_js_code *)embi_js_as_cell(code), &sb);
    const char *text = embi_sb_cstr(&sb);
    EMB_CHECK(strstr(text, "DEFINE_FIELD") != nullptr);
    EMB_CHECK(strstr(text, "key=\"alpha\"") != nullptr);
    EMB_CHECK(strstr(text, "GET_FIELD") != nullptr);
    EMB_CHECK(strstr(text, "state=0 entries=0") != nullptr);
    embi_sb_free(&sb);
    es6_env_free(e);
}

/* Each entry kind engages: after warm-up the site is in the expected state, and hits never refill
 * it (the fill counter stays put). A cache that never fills, or that misses every time, fails here. */
EMB_TEST(js_ic_engages_each_kind)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t n = 0;
    const embi_js_ic *ics = nullptr;

    E6_NUM(e, "function gx(q) { return q.x; } var o1 = {x: 1}; var s = 0;"
              "for (var i = 0; i < 5; i++) s += gx(o1); s",
           5);
    ics = global_fn_ics(e, "gx", &n);
    EMB_CHECK(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DATA, 0));
    EMB_CHECK(ics && ics[0].fills == 1);
    EMB_CHECK(hits_do_not_refill(e, "gx", 0, "gx(o1)", 300));

    /* GET through one prototype hop: the holder's shape is in the chain. */
    E6_NUM(e, "function gp(q) { return q.p; } var P = {p: 7}; var c1 = Object.create(P); var s2 = 0;"
              "for (var i = 0; i < 5; i++) s2 += gp(c1); s2",
           35);
    ics = global_fn_ics(e, "gp", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DATA, 1));
    EMB_CHECK(ics && ics[0].chain0 != nullptr && ics[0].chain0->n == 1);
    EMB_CHECK(hits_do_not_refill(e, "gp", 0, "gp(c1)", 300));

    /* A getter on the receiver: the accessor is called on every hit. */
    E6_NUM(e, "function ga(q) { return q.v; } var acc = {get v() { return 3; }}; var s3 = 0;"
              "for (var i = 0; i < 5; i++) s3 += ga(acc); s3",
           15);
    ics = global_fn_ics(e, "ga", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_ACCESSOR, 0));
    EMB_CHECK(hits_do_not_refill(e, "ga", 0, "ga(acc)", 300));

    /* A store into an existing own writable data property. */
    E6_NUM(e, "function st(q, v) { q.x = v; } var w = {x: 0}; for (var i = 0; i < 5; i++) st(w, i); w.x",
           4);
    ics = global_fn_ics(e, "st", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_STORE, 0));
    EMB_CHECK(hits_do_not_refill(e, "st", 0, "st(w, 9)", 300));

    /* An add by transition (SET on an absent key). */
    E6_NUM(e, "function ad(q) { q.y = 1; } for (var i = 0; i < 5; i++) ad({}); 1", 1);
    ics = global_fn_ics(e, "ad", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_ADD, 1));
    EMB_CHECK(hits_do_not_refill(e, "ad", 0, "ad({})", 300));

    /* A DEFINE by an object literal (CreateDataProperty on an absent key). */
    E6_NUM(e, "function mk(v) { return {k: v}; } for (var i = 0; i < 5; i++) mk(i); mk(7).k", 7);
    ics = global_fn_ics(e, "mk", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DEF_ADD, 0));
    EMB_CHECK(hits_do_not_refill(e, "mk", 0, "mk(3)", 300));

    /* Three receiver shapes: polymorphic, with hits on each. */
    E6_NUM(e, "function gq(q) { return q.x; } var pa = {x: 1}; var pb = {a: 0, x: 2};"
              "var pc = {a: 0, b: 0, x: 3}; var s4 = 0;"
              "for (var i = 0; i < 4; i++) s4 += gq(pa) + gq(pb) + gq(pc); s4",
           24);
    ics = global_fn_ics(e, "gq", &n);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_POLYMORPHIC, 3, EMBI_JS_IC_DATA, 0));
    EMB_CHECK(hits_do_not_refill(e, "gq", 0, "gq(pa) + gq(pb) + gq(pc)", 100));
    es6_env_free(e);
}

/* A site that sees a fifth shape goes megamorphic and runs generically, then refills after its
 * wait: the stopgap for the per-runtime cache. */
EMB_TEST(js_ic_megamorphic_retries)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t n = 0;
    E6_NUM(e, "function gm(q) { return q.x; } var five = ["
              "{x: 1}, {a: 0, x: 2}, {a: 0, b: 0, x: 3}, {a: 0, b: 0, c: 0, x: 4},"
              "{a: 0, b: 0, c: 0, d: 0, x: 5}];"
              "var t = 0; for (var i = 0; i < 5; i++) t += gm(five[i]); t",
           15);
    const embi_js_ic *ics = global_fn_ics(e, "gm", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MEGA, 0, -1, 0));
    /* Generic executions run through the wait; the site then fills again for the one shape. */
    E6_NUM(e, "var t2 = 0; for (var j = 0; j < 300; j++) t2 += gm(five[0]); t2", 300);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DATA, 0));
    es6_env_free(e);
}

/* A site that can never describe its receiver (a setter on the chain) stops filling after a few
 * failures, and every execution still runs the setter. */
EMB_TEST(js_ic_fill_failures_suspend)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t n = 0;
    E6_NUM(e, "var log = 0; var SP = {set s(v) { log++; }}; function ss(q, v) { q.s = v; }"
              "var sc = Object.create(SP); for (var i = 0; i < 100; i++) ss(sc, i); log",
           100);
    const embi_js_ic *ics = global_fn_ics(e, "ss", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_UNINIT, 0, -1, 0));
    EMB_CHECK(ics[0].fills == 0);
    es6_env_free(e);
}

/* Map and Set receivers are cached only when their class is flagged IC-capable. The check follows
 * the flag, so flagging the classes later keeps this case valid. */
EMB_TEST(js_ic_collections_follow_class_flag)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    uint32_t n = 0;
    E6_NUM(e, "function gm(q) { return q.foo; } var mp = new Map(); var s = 0;"
              "for (var i = 0; i < 5; i++) if (gm(mp) === undefined) s++; s",
           5);
    const embi_js_ic *ics = global_fn_ics(e, "gm", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    embi_js_value mv = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "mp"));
    EMB_REQUIRE(embi_js_is_object(mv));
    bool flagged = (embi_js_as_object(mv)->klass->flags & EMBI_JS_CLASS_IC) != 0;
    EMB_CHECK(flagged ? ics[0].nentries > 0 : ics[0].nentries == 0);

    E6_NUM(e, "function gs(q) { return q.foo; } var st = new Set(); var t = 0;"
              "for (var i = 0; i < 5; i++) if (gs(st) === undefined) t++; t",
           5);
    ics = global_fn_ics(e, "gs", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    embi_js_value sv = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "st"));
    EMB_REQUIRE(embi_js_is_object(sv));
    bool sflagged = (embi_js_as_object(sv)->klass->flags & EMBI_JS_CLASS_IC) != 0;
    EMB_CHECK(sflagged ? ics[0].nentries > 0 : ics[0].nentries == 0);
    es6_env_free(e);
}

/* ADD cached, then a setter defined on the prototype: the chain shapes change, so the add misses
 * and the setter runs, never a shadowing own add. */
EMB_TEST(js_ic_add_then_proto_setter)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var PS = {}; var log = []; function sz(q, v) { q.z = v; }"
            "for (var i = 0; i < 3; i++) { var r = Object.create(PS); sz(r, i); }"
            "Object.keys(r).join() === 'z'",
            true);
    uint32_t n = 0;
    /* The chain of Object.create(PS) is PS and Object.prototype: depth 2. */
    const embi_js_ic *ics = global_fn_ics(e, "sz", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_ADD, 2));
    E6_BOOL(e,
            "Object.defineProperty(PS, 'z', {set: function (v) { log.push('set:' + v); },"
            "  get: function () { return 'g'; }, configurable: true});"
            "var t = Object.create(PS); sz(t, 'after');"
            "log.join() === 'set:after' && !t.hasOwnProperty('z') && t.z === 'g'",
            true);
    es6_env_free(e);
}

/* ADD cached, then a non-writable property on the prototype: sloppy stores are ignored and strict
 * stores throw, both through the generic path. */
EMB_TEST(js_ic_add_then_proto_readonly)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var PN = {}; function sn(q, v) { q.w = v; return q.w; }"
            "function snS(q, v) { 'use strict'; q.w = v; }"
            "for (var i = 0; i < 3; i++) { sn(Object.create(PN), i); snS(Object.create(PN), i); }"
            "true",
            true);
    uint32_t n = 0;
    /* sn has two sites (the store, then the read): the store is site 0. The chain is PN and
     * Object.prototype, so the add's depth is 2. */
    const embi_js_ic *ics = global_fn_ics(e, "sn", &n);
    EMB_REQUIRE(ics != nullptr && n == 2);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_ADD, 2));
    E6_BOOL(e,
            "Object.defineProperty(PN, 'w', {value: 'ro', writable: false, configurable: true});"
            "var t2 = Object.create(PN); var res = sn(t2, 'x');"
            "var threw = false; try { snS(Object.create(PN), 'y'); } catch (err) { threw = err instanceof TypeError; }"
            "res === 'ro' && !t2.hasOwnProperty('w') && threw",
            true);
    es6_env_free(e);
}

/* A STORE cached on an own writable property, then the property becomes an accessor on the same
 * receiver: the setter runs. */
EMB_TEST(js_ic_store_then_own_accessor)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function st2(q, v) { q.x = v; } var sobj = {x: 0}; for (var i = 0; i < 3; i++) st2(sobj, i);"
            "true",
            true);
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, "st2", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_STORE, 0));
    E6_BOOL(e,
            "var setlog = [];"
            "Object.defineProperty(sobj, 'x', {get: function () { return 'G'; },"
            "  set: function (v) { setlog.push(v); }, configurable: true});"
            "st2(sobj, 'new'); setlog.join() === 'new' && sobj.x === 'G'",
            true);
    es6_env_free(e);
}

/* A DEFINE (object literal) cached as DEF_ADD, then a setter defined on Object.prototype for the
 * same key: a literal defines its own property and never calls the setter. */
EMB_TEST(js_ic_def_add_ignores_later_setter)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function mk2(v) { return {k2: v}; } for (var i = 0; i < 3; i++) mk2(i); true", true);
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, "mk2", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DEF_ADD, 0));
    E6_BOOL(e,
            "var setcalls = 0; Object.defineProperty(Object.prototype, 'k2', {set: function (v) {"
            "  setcalls++; }, configurable: true});"
            "var made = mk2(5); var ok = made.k2 === 5 && made.hasOwnProperty('k2') && setcalls === 0;"
            "delete Object.prototype.k2; ok",
            true);
    es6_env_free(e);
}

/* A dictionary-mode prototype (flagged IS_PROTO) changed in place by setPrototypeOf to a prototype
 * with the same shape: the chain's holder has moved, so the cached read must see the new holder. */
EMB_TEST(js_ic_dict_proto_setprototypeof_same_shape)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var P = {}; for (var i = 0; i < 70; i++) P['k' + i] = i;"
            "var Q1 = {deep: 'one'}; var Q2 = {deep: 'two'};"
            "Object.setPrototypeOf(P, Q1);"
            "var C = Object.create(P); function gd(q) { return q.deep; } var rr = [];"
            "for (var j = 0; j < 3; j++) rr.push(gd(C));"
            "rr.join() === 'one,one,one'",
            true);
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, "gd", &n);
    EMB_REQUIRE(ics != nullptr && n == 1);
    EMB_CHECK(site_is(ics, EMBI_JS_IC_MONO, 1, EMBI_JS_IC_DATA, 2));
    E6_BOOL(e,
            "Object.setPrototypeOf(P, Q2); rr.push(gd(C));"
            "rr.join() === 'one,one,one,two' && gd(C) === 'two'",
            true);
    es6_env_free(e);
}

EMB_TEST_MAIN()
