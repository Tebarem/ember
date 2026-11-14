#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* J5k: the write barrier audit. Runtimes start generational, so every store of a young value into
 * an old cell or a cell-owned buffer must be barriered. Each case makes its owner old with two
 * minors, then stores a freshly made value into it, then runs a minor with that value reachable
 * only through the owner, and reads it back. The owner classes covered are: named slots, dense
 * elements, dictionary entries (with string and symbol keys), shape transitions from an old shape,
 * generator saved frames, promise reaction lists, module records, class private-method
 * initializers, Map tables, and closure boxes (captured variables). The fresh value is allocated
 * under a collection before every allocation (embi_js_gc_set_stress 1), so the owner is promoted
 * before the value exists. The stress is turned off before the store, so nothing runs between the
 * value's creation and the store except the barrier. In debug builds the verifier aborts when a
 * barrier is missing, and the release check reads the value back. Each entry point runs inside
 * embi_js_enter/embi_js_leave, as a host call does. */

#define BAR_PAYLOAD "barrier-payload-0123456789"

static EMB_TEST_NOIPA void scrub_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub() scrub_impl(0, 0, 0, 0)

static void minor_twice(embi_js_runtime *rt)
{
    embi_js_enter(rt);
    embi_js_gc_minor(rt);
    embi_js_gc_minor(rt);
    embi_js_leave(rt);
}

static void drain_jobs(embi_js_runtime *rt)
{
    embi_js_enter(rt);
    embi_js_run_jobs(rt);
    embi_js_leave(rt);
}

/* Stores a string made after the owner was promoted, then drops every local reference to it. The
 * collection-before-every-allocation mode covers only the string's allocation, so the store that
 * follows runs with the owner old and the string young and no collection in between. */
static EMB_TEST_NOIPA void store_fresh_string(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                              uint32_t tag)
{
    char text[sizeof BAR_PAYLOAD + 8];
    SDL_snprintf(text, sizeof text, "%s-%u", BAR_PAYLOAD, (unsigned)tag);
    embi_js_enter(rt);
    embi_js_gc_set_stress(rt, 1);
    embi_js_string *s = embi_js_str_new_utf8(rt, text, strlen(text));
    embi_js_gc_set_stress(rt, 0);
    embi_js_obj_set(rt, o, k, embi_js_from_string(s), embi_js_from_object(o));
    embi_js_leave(rt);
}

static EMB_TEST_NOIPA bool value_is(embi_js_value v, uint32_t tag)
{
    char text[sizeof BAR_PAYLOAD + 8];
    SDL_snprintf(text, sizeof text, "%s-%u", BAR_PAYLOAD, (unsigned)tag);
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_as_string(v);
    size_t n = strlen(text);
    if (embi_js_str_len(s) != n || embi_js_str_is_wide(s)) {
        return false;
    }
    return memcmp(embi_js_str_latin1(s), text, n) == 0;
}

/* A realm for the scripted cases. The memory check compares live bytes with the start. */
typedef struct bar_env {
    embi_js_runtime *rt;
    embi_js_realm *r;
    size_t mem0;
} bar_env;

static bar_env bar_new(void)
{
    bar_env e;
    e.mem0 = embi_mem_live();
    e.rt = embi_js_runtime_new();
    EMB_CHECK(e.rt != nullptr);
    e.r = embi_js_realm_new(e.rt);
    EMB_CHECK(e.r != nullptr);
    return e;
}

static void bar_free(bar_env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
    EMB_CHECK(embi_mem_live() == e.mem0);
}

/* Runs src as a script and reports whether it evaluates to true. With stress, a collection runs
 * before every allocation of the script. */
static bool js_true_at(embi_js_realm *r, const char *src, bool stress)
{
    embi_js_runtime *rt = r->rt;
    embi_js_enter(rt);
    if (stress) {
        embi_js_gc_set_stress(rt, 1);
    }
    embi_js_value v = embi_js_eval(r, src, strlen(src), "barrier.js", 0);
    if (stress) {
        embi_js_gc_set_stress(rt, 0);
    }
    bool ok = v == EMBI_JS_TRUE;
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
    }
    embi_js_leave(rt);
    if (!ok) {
        SDL_Log("    script did not return true: %s", src);
    }
    return ok;
}

static bool js_true(embi_js_realm *r, const char *src)
{
    return js_true_at(r, src, false);
}

/* Named property in the inline or out-of-line slots of an old object. */
EMB_TEST(gc_barrier_named_slot_store_into_old_object)
{
    scrub();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_key k = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "named", 5));
    uint32_t id = embi_js_persistent_new(rt, embi_js_from_object(o));
    embi_js_leave(rt);
    minor_twice(rt);
    store_fresh_string(rt, o, k, 1);
    scrub();
    minor_twice(rt);
    embi_js_enter(rt);
    EMB_CHECK(value_is(embi_js_obj_get(rt, o, k, embi_js_from_object(o)), 1));
    embi_js_leave(rt);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub();
}

/* Index property in the dense elements of an old array-like object. */
EMB_TEST(gc_barrier_element_store_into_old_object)
{
    scrub();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_key k = embi_js_key_from_index(0);
    uint32_t id = embi_js_persistent_new(rt, embi_js_from_object(o));
    embi_js_leave(rt);
    minor_twice(rt);
    store_fresh_string(rt, o, k, 2);
    scrub();
    minor_twice(rt);
    embi_js_enter(rt);
    EMB_CHECK(value_is(embi_js_obj_get(rt, o, k, embi_js_from_object(o)), 2));
    embi_js_leave(rt);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub();
}

/* A dictionary-mode object: more named properties than the shape chain takes, so the store lands
 * in a dictionary entry that the owner's trace reaches. */
EMB_TEST(gc_barrier_dict_store_into_old_object)
{
    scrub();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    uint32_t id = embi_js_persistent_new(rt, embi_js_from_object(o));
    char name[16];
    for (uint32_t i = 0; i < 80; i++) {
        SDL_snprintf(name, sizeof name, "p%u", (unsigned)i);
        embi_js_obj_set(rt, o, embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, name, strlen(name))),
                        embi_js_from_int32((int32_t)i), embi_js_from_object(o));
    }
    embi_js_key k = embi_js_key_from_str(rt, embi_js_str_new_utf8(rt, "dictkey", 7));
    embi_js_leave(rt);
    minor_twice(rt);
    store_fresh_string(rt, o, k, 3);
    scrub();
    minor_twice(rt);
    embi_js_enter(rt);
    EMB_CHECK(value_is(embi_js_obj_get(rt, o, k, embi_js_from_object(o)), 3));
    embi_js_leave(rt);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub();
}

/* Two runtimes on one thread: an old object of the first one must keep its barrier after the
 * second one was created and used. The barrier target follows the entered runtime. */
EMB_TEST(gc_barrier_follows_the_entered_runtime)
{
    scrub();
    embi_js_runtime *a = embi_js_runtime_new();
    embi_js_enter(a);
    embi_js_object *o = embi_js_object_new_plain(a);
    embi_js_key k = embi_js_key_from_str(a, embi_js_str_new_utf8(a, "shared", 6));
    uint32_t id = embi_js_persistent_new(a, embi_js_from_object(o));
    embi_js_leave(a);
    minor_twice(a);

    embi_js_runtime *b = embi_js_runtime_new();
    embi_js_enter(b);
    embi_js_object *ob = embi_js_object_new_plain(b);
    uint32_t idb = embi_js_persistent_new(b, embi_js_from_object(ob));
    embi_js_gc_minor(b);
    embi_js_leave(b);

    store_fresh_string(a, o, k, 4);
    scrub();
    minor_twice(a);
    embi_js_enter(a);
    EMB_CHECK(value_is(embi_js_obj_get(a, o, k, embi_js_from_object(o)), 4));
    embi_js_leave(a);
    embi_js_persistent_free(b, idb);
    embi_js_runtime_free(b);
    embi_js_persistent_free(a, id);
    embi_js_runtime_free(a);
    scrub();
}

/* A generator's saved frame. The generator is old; it yields with a freshly made local, which the
 * yield copies into its saved slots, and the minor then runs with that local reachable only
 * through the generator. Resuming must read the local back. */
EMB_TEST(gc_barrier_generator_saved_local_in_old_generator)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "var it = (function* g() { var local; for (;;) { local = { tag: 'fresh' }; "
                           "yield 0; yield local.tag; } })(); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "it.next().value === 0"));
    scrub();
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "it.next().value === 'fresh'"));
    bar_free(e);
    scrub();
}

/* A promise's reaction lists. Reactions appended to a list whose tail node is old must be
 * barriered on that node: the second and third reactions are added after collections. */
EMB_TEST(gc_barrier_promise_reactions_across_minors)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.cnt = 0; globalThis.P = new Promise(function (r) { "
                           "globalThis.res = r; }); globalThis.P.then(function () { "
                           "globalThis.cnt++; }); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.P.then(function () { globalThis.cnt++; }); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.P.then(function () { globalThis.cnt++; }); "
                           "globalThis.res('x'); true"));
    drain_jobs(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.cnt === 3"));
    bar_free(e);
    scrub();
}

/* A module record. The loader answers synchronously, and a collection runs before every
 * allocation while the module is loaded, so the holder is promoted between its creation and the
 * stores of its code, its requested modules and its namespace. */
static void bar_loader(embi_js_runtime *rt, void *user, uint32_t req, const char *ref, size_t rlen,
                       const char *spec, size_t slen, const char *type)
{
    (void)user;
    (void)ref;
    (void)rlen;
    (void)type;
    const char *src = nullptr;
    if (slen == 1 && spec[0] == 'a') {
        src = "import { x } from 'b'; globalThis.got = x;";
    } else if (slen == 1 && spec[0] == 'b') {
        src = "export let x = 'bval';";
    }
    if (src == nullptr) {
        embi_js_module_load_failed(rt, req, "no such module");
        return;
    }
    embi_js_module_loaded(rt, req, spec, slen, src, strlen(src));
}

EMB_TEST(gc_barrier_module_record_stores_under_stress)
{
    scrub();
    bar_env e = bar_new();
    embi_js_set_module_loader(e.rt, bar_loader, nullptr);
    EMB_CHECK(js_true(e.r, "globalThis.got = 'unset'; true"));
    embi_js_enter(e.rt);
    embi_js_gc_set_stress(e.rt, 1);
    (void)embi_js_module_eval(e.r, "a", 1);
    embi_js_gc_set_stress(e.rt, 0);
    embi_js_leave(e.rt);
    minor_twice(e.rt);
    drain_jobs(e.rt);
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.got === 'bval'"));
    bar_free(e);
    scrub();
}

/* A class with a private method and no fields: its instance initializer (pinit) is a young
 * closure stored into the constructor, which a collection during the class body has promoted. */
EMB_TEST(gc_barrier_class_pinit_into_old_constructor)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true_at(e.r,
                         "globalThis.C = class { #m() { return 'ok'; } r() { return this.#m(); } }; "
                         "true",
                         true));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "new globalThis.C().r() === 'ok'"));
    bar_free(e);
    scrub();
}

/* A dictionary-mode object that gets a fresh symbol key. The symbol is referenced only by the
 * dictionary entry. */
EMB_TEST(gc_barrier_symbol_key_in_dict_object)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.o = {}; for (var i = 0; i < 70; i++) { o['p' + i] = i; } true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "o[Symbol('k')] = 'fresh-' + 1; true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "var ks = Object.getOwnPropertySymbols(o); ks.length === 1 && "
                           "ks[0].description === 'k' && o[ks[0]] === 'fresh-1'"));
    bar_free(e);
    scrub();
}

/* Shape transitions from an old shape. The temporary object's child shape is reachable only
 * through the parent shape's child link, so the link must be barriered on the parent. A second
 * object then takes the same transition. */
EMB_TEST(gc_barrier_shape_transition_from_old_shape)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.keep = {}; true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "(function () { var t = {}; t.x = 1; })(); true"));
    minor_twice(e.rt);
    scrub();
    EMB_CHECK(js_true(e.r, "var u = {}; u.x = 2; u.x === 2 && Object.keys(u).join() === 'x' && "
                           "keep.x === undefined"));
    bar_free(e);
    scrub();
}

/* A Map's table. The entry is added to an old map after its promotion. */
EMB_TEST(gc_barrier_map_entry_into_old_map)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.m = new Map(); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.m.set('k', 'fresh-' + 2); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.m.get('k') === 'fresh-2'"));
    bar_free(e);
    scrub();
}

/* A captured variable. Its box is old (the closures are old); a closure stores a fresh string
 * into the box, and the minor must reach it through the box. */
EMB_TEST(gc_barrier_closure_box_store_into_old_box)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.get = null; globalThis.set = null; (function () { "
                           "let x = 'a'; get = function () { return x; }; "
                           "set = function (v) { x = v; }; })(); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "set('fresh-' + 3); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "get() === 'fresh-3'"));
    bar_free(e);
    scrub();
}

EMB_TEST_MAIN()

/* Full collection, then the queued jobs, as a host loop would run them. */
static void full_gc_and_jobs(embi_js_runtime *rt)
{
    embi_js_enter(rt);
    embi_js_gc(rt);
    embi_js_run_jobs(rt);
    embi_js_leave(rt);
}

/* FinalizationRegistry.prototype.register on an old registry: the held value is young and only the
 * registry's C-heap cell list refers to it. The target dies at once, so the cleanup callback must
 * receive the held string intact. */
EMB_TEST(gc_barrier_finalization_registry_held_into_old_registry)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.got = 'unset'; globalThis.fr = new FinalizationRegistry("
                           "function (h) { globalThis.got = h; }); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "(function () { fr.register({}, 'held-' + 12345 + '-payload'); })(); true"));
    scrub();
    embi_js_enter(e.rt);
    embi_js_gc_minor(e.rt);
    embi_js_leave(e.rt);
    full_gc_and_jobs(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.got === 'held-12345-payload'"));
    bar_free(e);
    scrub();
}

/* A registry created under collect-before-every-allocation: its cleanup callback must survive. */
EMB_TEST(gc_barrier_finalization_registry_cleanup_under_stress)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true_at(e.r, "globalThis.got = 'unset'; globalThis.fr = new FinalizationRegistry("
                              "function (h) { globalThis.got = 'cb-' + h; }); true",
                         true));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "(function () { fr.register({}, 'h'); })(); true"));
    full_gc_and_jobs(e.rt);
    EMB_CHECK(js_true(e.r, "globalThis.got === 'cb-h'"));
    bar_free(e);
    scrub();
}

/* Intl.NumberFormat / DateTimeFormat / Collator: the bound function of the format / compare getter
 * is created lazily, after the owner can be old, and only the C-heap state refers to it. */
EMB_TEST(gc_barrier_intl_bound_getters_on_old_instances)
{
    scrub();
    bar_env e = bar_new();
    EMB_CHECK(js_true(e.r, "globalThis.nf = new Intl.NumberFormat('en-US'); "
                           "globalThis.dtf = new Intl.DateTimeFormat('en-US', {timeZone: 'UTC'}); "
                           "globalThis.col = new Intl.Collator('en-US'); true"));
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "nf.format; dtf.format; col.compare; true"));
    scrub();
    minor_twice(e.rt);
    EMB_CHECK(js_true(e.r, "nf.format(1234) === '1,234' && nf.format === nf.format && "
                           "typeof dtf.format(0) === 'string' && dtf.format === dtf.format && "
                           "col.compare('a', 'b') < 0 && col.compare === col.compare"));
    bar_free(e);
    scrub();
}

/* The sweep over the state-struct classes: for each, the owner is made old with two minors, then
 * the mutating operations run with fresh values, then a minor, the queued jobs, two more minors
 * and a read back. Each case is a (setup, mutate, check, final) set of scripts; final runs after
 * the jobs that check queued. In debug builds the verifier aborts on a missing barrier; in release
 * the checks read the values back. */
typedef struct bar_case {
    const char *name;
    const char *setup;
    const char *mutate;
    const char *check;
    const char *final;
} bar_case;

static const bar_case bar_cases[] = {
    {"map iterator",
     "globalThis.m = new Map([[1, 'a'], [2, 'b']]); globalThis.it = m.entries(); true",
     "m.set(3, 'fresh-' + 3); it.next(); it.next(); true",
     "it.next().value[1] === 'fresh-3' && it.next().done", nullptr},
    {"set iterator", "globalThis.s = new Set([1]); globalThis.it = s.values(); true",
     "s.add('fresh-' + 1); it.next(); true", "it.next().value === 'fresh-1' && it.next().done",
     nullptr},
    {"array iterator", "globalThis.a = [0]; globalThis.it = a[Symbol.iterator](); true",
     "a.push('fresh-' + 1); it.next(); true", "it.next().value === 'fresh-1' && it.next().done",
     nullptr},
    {"string iterator", "globalThis.it = 'abc'[Symbol.iterator](); true", "it.next(); true",
     "it.next().value === 'b'", nullptr},
    {"iterator helper map",
     "globalThis.src = [1, 2, 3, 4]; globalThis.h = src.values().map(function (x) { "
     "return 'v-' + x; }); true",
     "h.next(); true", "h.next().value === 'v-2' && h.next().value === 'v-3'", nullptr},
    {"iterator helper flatMap",
     "globalThis.h = [1, 2].values().flatMap(function (x) { return [x + 'a-' + x, x + 'b']; }); true",
     "h.next(); true", "h.next().value === '1b' && h.next().value === '2a-2'", nullptr},
    {"iterator helper take drop", "globalThis.h = [1, 2, 3, 4, 5].values().drop(1).take(3); true",
     "h.next(); true", "h.next().value === 3 && h.next().value === 4 && h.next().done", nullptr},
    {"iterator wrap",
     "globalThis.w = Iterator.from({ i: 0, next() { return { done: this.i > 2, value: 'w-' + "
     "this.i++ }; } }); true",
     "w.next(); true", "w.next().value === 'w-1'", nullptr},
    {"proxy revocable",
     "var p = Proxy.revocable({ k: 'v' }, {}); globalThis.px = p.proxy; globalThis.rv = p.revoke; true",
     "px.k === 'v' && (rv(), true)",
     "(function () { try { px.k; } catch (e) { return e instanceof TypeError; } })()", nullptr},
    {"weakmap", "globalThis.key = {}; globalThis.wm = new WeakMap(); true",
     "wm.set(key, 'fresh-' + 1); true", "wm.get(key) === 'fresh-1'", nullptr},
    {"weakset and weakref",
     "globalThis.key = {}; globalThis.ws = new WeakSet(); globalThis.wr = new WeakRef(key); true",
     "ws.add(key); true", "ws.has(key) && wr.deref() === key", nullptr},
    {"promise all elements",
     "globalThis.res = []; globalThis.ps = [0, 1].map(function (i) { return new Promise(function (r) { "
     "res[i] = r; }); }); globalThis.out = null; Promise.all(ps).then(function (v) { out = v; }); true",
     "res[0]('fresh-' + 0); true", "(res[1]('fresh-' + 1), true)",
     "out !== null && out.join() === 'fresh-0,fresh-1'"},
    {"async function frame",
     "globalThis.go = null; globalThis.out = 'unset'; (async function () { var loc = { t: 'x' }; "
     "await new Promise(function (r) { go = r; }); loc = { t: 'fresh-' + 1 }; await 0; "
     "out = loc.t; })(); true",
     "go(); true", "true", "out === 'fresh-1'"},
    {"class home object super",
     "globalThis.B = class { m() { return 'base'; } }; globalThis.D = class extends B { "
     "m() { return super.m() + '-d'; } }; true",
     "globalThis.d = new D(); true", "d.m() === 'base-d'", nullptr},
    {"object literal super",
     "globalThis.ob = { __proto__: { m() { return 'p'; } }, m() { return super.m() + '-o'; } }; true",
     "true", "ob.m() === 'p-o'", nullptr},
    {"eval home",
     "globalThis.eo = { __proto__: { v: 'pv' }, f() { return eval('super.v'); } }; true", "true",
     "eo.f() === 'pv'", nullptr},
    {"bound function args", "globalThis.bf = function (a, b) { return a + b; }.bind(null, 'x'); true",
     "globalThis.bf2 = bf.bind(null, 'fresh-' + 1); true", "bf2('!') === 'xfresh-1'", nullptr},
    {"error cause and aggregate", "globalThis.er = new Error('m'); true",
     "er.cause = 'fresh-' + 1; globalThis.ag = new AggregateError(['fresh-' + 2], 'a'); true",
     "er.cause === 'fresh-1' && ag.errors[0] === 'fresh-2'", nullptr},
    {"typed array and buffer",
     "globalThis.ab = new ArrayBuffer(8); globalThis.ta = new Uint8Array(ab); true",
     "ta[0] = 7; globalThis.sl = ab.slice(0); true", "ta[0] === 7 && new Uint8Array(sl)[0] === 7",
     nullptr},
    {"regexp last match", "globalThis.re = /(b+)/g; true", "re.exec('abbc'); true",
     "re.lastIndex === 3 && 'abbc'.replace(re, '[$1]') === 'a[bb]c'", nullptr},
    {"generator return value",
     "globalThis.g = (function* () { var x = yield 1; return 'r-' + x; })(); g.next(); true",
     "globalThis.rr = g.next('fresh-' + 1); true", "rr.done && rr.value === 'r-fresh-1'", nullptr},
    {"array sort and push", "globalThis.arr = [3, 1, 2]; true",
     "arr.sort(function (a, b) { return a - b; }); arr.push({ q: 'fresh-' + 1 }); true",
     "JSON.stringify(arr) === '[1,2,3,{\"q\":\"fresh-1\"}]'", nullptr},
};

EMB_TEST(gc_barrier_state_class_sweep)
{
    scrub();
    for (size_t i = 0; i < sizeof bar_cases / sizeof bar_cases[0]; i++) {
        const bar_case *c = &bar_cases[i];
        SDL_Log("  case: %s", c->name);
        bar_env e = bar_new();
        EMB_CHECK(js_true(e.r, c->setup));
        minor_twice(e.rt);
        EMB_CHECK(js_true(e.r, c->mutate));
        scrub();
        embi_js_enter(e.rt);
        embi_js_gc_minor(e.rt);
        embi_js_leave(e.rt);
        drain_jobs(e.rt);
        minor_twice(e.rt);
        EMB_CHECK(js_true(e.r, c->check));
        drain_jobs(e.rt);
        minor_twice(e.rt);
        if (c->final != nullptr) {
            EMB_CHECK(js_true(e.r, c->final));
        }
        bar_free(e);
        scrub();
    }
}
