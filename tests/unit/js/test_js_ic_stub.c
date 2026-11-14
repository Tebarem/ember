/* Megamorphic stub cache and inline first-entry IC hits (J5m1, items 1
 * and 2). Each case drives a site past EMBI_JS_IC_POLY shapes so that it goes megamorphic and reads
 * or writes through the runtime's stub cache, changes the world under it (own and inherited values,
 * accessors, deletes, prototype changes, freezing, read-only and setter-only properties, collections),
 * and checks every result against the computed-key oracle o[k], which never uses a cache. */
#include "es6_util.h"

#include "js/bytecode.h"
#include "js/ic.h"
#include "js/interp.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* The inline caches of the global function named `name`, or nullptr when it is not a closure. */
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

/* True when site `site` of global function fn has gone megamorphic. */
static bool site_is_mega(es6_env e, const char *fn, uint32_t site)
{
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, fn, &n);
    bool mega = ics && site < n && ics[site].state == EMBI_JS_IC_MEGA;
    if (!mega) {
        SDL_Log("    site %u of %s is not megamorphic", site, fn);
    }
    return mega;
}

/* Runs `call` `times` times, then requires the fill counter of site `site` of fn to be unchanged:
 * hits never refill (the inline first-entry path and the entries it checks). */
static bool hits_do_not_refill(es6_env e, const char *fn, uint32_t site, const char *call, int times)
{
    uint32_t n = 0;
    const embi_js_ic *ics = global_fn_ics(e, fn, &n);
    if (!ics || site >= n) {
        return false;
    }
    /* Warm once: the first run may fill. */
    if (es6_run(e, call) == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
        return false;
    }
    uint16_t before = ics[site].fills;
    for (int i = 0; i < times; i++) {
        if (es6_run(e, call) == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(e.rt);
            return false;
        }
    }
    if (ics[site].fills != before) {
        SDL_Log("    fills changed %u -> %u on hits of %s", before, ics[site].fills, fn);
    }
    return ics[site].fills == before;
}

/* Six object shapes (the first has one key, the last six), each with a named x. */
#define STUB_SHAPES_JS                                                                         \
    "var mk = ["                                                                               \
    " function() { return {x: 1}; },"                                                          \
    " function() { return {a: 0, x: 2}; },"                                                    \
    " function() { return {a: 0, b: 0, x: 3}; },"                                              \
    " function() { return {a: 0, b: 0, c: 0, x: 4}; },"                                        \
    " function() { return {a: 0, b: 0, c: 0, d: 0, x: 5}; },"                                  \
    " function() { return {a: 0, b: 0, c: 0, d: 0, e: 0, x: 6}; }];"                          \
    "var ok = true;"                                                                           \
    "function chk(o, want) { if (g(o) !== want || o['x'] !== want) { ok = false; } }"

/* Reads of a megamorphic site track every change made after it went megamorphic. */
EMB_TEST(js_ic_stub_mega_reads_follow_changes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, STUB_SHAPES_JS
               "function g(q) { return q.x; }"
               "var objs = []; for (var i = 0; i < 6; i++) { objs.push(mk[i]()); }"
               "for (var r = 0; r < 3; r++) { for (var i = 0; i < 6; i++) { chk(objs[i], i + 1); } }"
               "ok", true);
    EMB_CHECK(site_is_mega(e, "g", 0));
    /* An own write to a cached holder. */
    E6_BOOL(e, "objs[0].x = 10; chk(objs[0], 10); ok", true);
    /* An own accessor replaces a data property. */
    E6_BOOL(e, "Object.defineProperty(objs[1], 'x', {get: function() { return 'getter'; }, configurable: true});"
               "chk(objs[1], 'getter'); ok", true);
    /* A deleted own property falls back to Object.prototype (depth 1), and a shadowing add there
     * changes the answer again. */
    E6_BOOL(e, "delete objs[2].x; chk(objs[2], undefined); Object.prototype.x = 'op';"
               "chk(objs[2], 'op'); delete Object.prototype.x; chk(objs[2], undefined); ok", true);
    /* A changed prototype with the key on it (depth 1), then on a longer chain (depth 2). */
    E6_BOOL(e, "delete objs[3].x; Object.setPrototypeOf(objs[3], {x: 'np'}); chk(objs[3], 'np'); ok", true);
    E6_BOOL(e, "var A = {x: 'A'}; var B = Object.create(A); var deep = Object.create(B);"
               "chk(deep, 'A'); A.x = 'A2'; chk(deep, 'A2'); B.x = 'B'; chk(deep, 'B');"
               "delete B.x; chk(deep, 'A2'); ok", true);
    /* Frozen objects read the same. */
    E6_BOOL(e, "Object.freeze(objs[4]); chk(objs[4], 5); ok", true);
    /* A getter on the prototype runs with the receiver as this. */
    E6_BOOL(e, "var G = {get x() { return this.tag; }}; var gobj = Object.create(G); gobj.tag = 'self';"
               "chk(gobj, 'self'); gobj.tag = 'changed'; chk(gobj, 'changed'); ok", true);
    /* Results still match after a collection empties the stub cache. */
    EMB_CHECK(embi_js_stub_cache_count(e.rt) > 0);
    embi_js_gc(e.rt);
    EMB_CHECK_EQ_INT(embi_js_stub_cache_count(e.rt), 0);
    E6_BOOL(e, "chk(objs[0], 10); chk(objs[4], 5); ok", true);
    es6_env_free(e);
}

/* Writes at a megamorphic site: own stores, frozen and read-only targets, setter-only inheritance,
 * and adds on fresh objects. */
EMB_TEST(js_ic_stub_mega_writes_follow_changes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, STUB_SHAPES_JS
               "function s(q, v) { q.x = v; }"
               "var objs = []; for (var i = 0; i < 6; i++) { objs.push(mk[i]()); }"
               "for (var r = 0; r < 3; r++) { for (var i = 0; i < 6; i++) { s(objs[i], i * 10); } }"
               "for (var i = 0; i < 6; i++) { if (objs[i]['x'] !== i * 10) { ok = false; } }"
               "ok", true);
    EMB_CHECK(site_is_mega(e, "s", 0));
    /* A frozen target ignores the write (sloppy mode). */
    E6_BOOL(e, "Object.freeze(objs[0]); s(objs[0], 99); objs[0]['x'] === 0", true);
    /* A setter on the prototype takes the write, and no own property is created. */
    E6_BOOL(e, "var seen = null; var Q = {set x(v) { seen = v; }}; var fresh = Object.create(Q);"
               "s(fresh, 7); seen === 7 && !fresh.hasOwnProperty('x')", true);
    /* A read-only inherited property refuses the write. */
    E6_BOOL(e, "var R = {}; Object.defineProperty(R, 'x', {value: 1, writable: false});"
               "var z = Object.create(R); s(z, 3); z['x'] === 1 && !z.hasOwnProperty('x')", true);
    /* A fresh object of an unseen shape takes the add. */
    E6_BOOL(e, "var nw = {}; s(nw, 'added'); nw['x'] === 'added' && Object.keys(nw).length === 1", true);
    /* An own writable property written after a shape change. */
    E6_BOOL(e, "objs[5].extra = 1; s(objs[5], 'after'); objs[5]['x'] === 'after' && objs[5].extra === 1", true);
    es6_env_free(e);
}

/* Read-only and accessor results at a megamorphic site with primitive receivers: the method comes
 * from the intrinsic prototype, and a replaced method is seen at once. */
EMB_TEST(js_ic_stub_mega_primitive_receivers)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, STUB_SHAPES_JS
               "function m(v) { return v.toString; }"
               "var objs = []; for (var i = 0; i < 6; i++) { objs.push(mk[i]()); }"
               "var prims = [1, 'a', true, 2.5];"
               "for (var r = 0; r < 2; r++) {"
               "  for (var i = 0; i < 6; i++) { if (m(objs[i]) !== Object.prototype.toString) { ok = false; } }"
               "  if (m(1) !== Number.prototype.toString) { ok = false; }"
               "  if (m('a') !== String.prototype.toString) { ok = false; }"
               "  if (m(true) !== Boolean.prototype.toString) { ok = false; }"
               "}"
               "ok", true);
    EMB_CHECK(site_is_mega(e, "m", 0));
    E6_BOOL(e, "var nf = function() { return 'n'; }; Number.prototype.toString = nf;"
               "var prim_ok = m(1) === nf && m(2.5) === nf && m(prims[1]) === String.prototype.toString;"
               "prim_ok && ok", true);
    es6_env_free(e);
}

/* Inline first-entry hits: a monomorphic read and write of an own property refill nothing after
 * the first run. */
EMB_TEST(js_ic_inline_first_entry_hits_do_not_refill)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function g(q) { return q.x; } function s(q, v) { q.x = v; }"
               "var o = {x: 1, y: 2}; var ok = true; for (var i = 0; i < 5; i++) { g(o); s(o, i); }"
               "ok", true);
    EMB_CHECK(hits_do_not_refill(e, "g", 0, "g(o)", 50));
    EMB_CHECK(hits_do_not_refill(e, "s", 0, "s(o, 3)", 50));
    E6_BOOL(e, "g(o) === 3", true);
    es6_env_free(e);
}

/* The first entry must not answer for a receiver it does not describe: a shape-equal object whose
 * class has a hook, or a property that became an accessor, takes the generic path. */
EMB_TEST(js_ic_inline_first_entry_respects_changes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function g(q) { return q.x; } var a = {x: 'a'}; var ok = true;"
               "for (var i = 0; i < 5; i++) { if (g(a) !== 'a') { ok = false; } }"
               "Object.defineProperty(a, 'x', {get: function() { return 'g'; }});"
               "if (g(a) !== 'g') { ok = false; }"
               "var b = {x: 'b'}; if (g(b) !== 'b') { ok = false; }"
               "ok", true);
    E6_BOOL(e, "function s(q, v) { q.x = v; } var c = {x: 1}; var frozen = {x: 1};"
               "for (var i = 0; i < 5; i++) { s(c, i); } Object.freeze(frozen); s(frozen, 9);"
               "c.x === 4 && frozen.x === 1", true);
    es6_env_free(e);
}

/* J5n1: a read-modify-write (q.c = q.c + 1) on a few shapes at megamorphic sites. The read and the store
 * of one (shape, key) pair use separate stub entries, so after warm-up the loop records nothing: before
 * the split each side evicted the other's entry and every step refilled both. */
EMB_TEST(js_ic_stub_mega_read_modify_write_hits)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var objs = [{c: 0}, {a: 0, c: 0}, {a: 0, b: 0, c: 0}, {a: 0, b: 0, d: 0, c: 0},"
               " {a: 0, b: 0, d: 0, e: 0, c: 0}, {a: 0, b: 0, d: 0, e: 0, f: 0, c: 0}];"
               "function inc(q) { q.c = q.c + 1; }"
               "for (var r = 0; r < 20; r++) { for (var i = 0; i < 6; i++) { inc(objs[i]); } }"
               "true", true);
    EMB_CHECK(site_is_mega(e, "inc", 0));
    EMB_CHECK(site_is_mega(e, "inc", 1));
    uint64_t before = embi_js_stub_cache_fills(e.rt);
    uint32_t entries = embi_js_stub_cache_count(e.rt);
    E6_BOOL(e, "for (var r = 0; r < 1000; r++) { for (var i = 0; i < 6; i++) { inc(objs[i]); } } true", true);
    uint64_t refills = embi_js_stub_cache_fills(e.rt) - before;
    SDL_Log("    stub entries %u, refills over 6000 steps: %llu", entries, (unsigned long long)refills);
    /* Twelve pairs live in a 1 024-slot table: a few may collide, but the refills stay far below the
     * 12 000 that evicting read and store entries would cost. */
    EMB_CHECK(entries >= 8);
    EMB_CHECK(refills < 600);
    E6_BOOL(e, "var okc = true; for (var i = 0; i < 6; i++) { if (objs[i].c !== 1020) { okc = false; } } okc", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A stub cache in use returns the same values across an allocation-heavy loop with collections. */
EMB_TEST(js_ic_stub_mega_survives_collections)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, STUB_SHAPES_JS
               "function g(q) { return q.x; }"
               "var sum = 0; for (var r = 0; r < 400; r++) {"
               "  var o = mk[r % 6](); var junk = []; for (var j = 0; j < 40; j++) { junk.push({x: j}); }"
               "  if (g(o) !== (r % 6) + 1) { ok = false; }"
               "  sum += g(junk[39]);"
               "}"
               "ok && sum === 400 * 39", true);
    /* The loop ends in whatever phase of the site's wait cycle its 800 calls left it in. Twelve calls on
     * six shapes put the site back in megamorphic state from any phase, so the check below is about the
     * stub path rather than the timing of the loop. */
    E6_BOOL(e, "for (var r = 0; r < 12; r++) { if (g(mk[r % 6]()) !== (r % 6) + 1) { ok = false; } }"
               "ok", true);
    EMB_CHECK(site_is_mega(e, "g", 0));
    es6_env_free(e);
}

EMB_TEST_MAIN()
