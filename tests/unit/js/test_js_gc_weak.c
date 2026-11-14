#include "emb_test.h"

#include "js/collections.h"
#include "js/js.h"

#include <stdint.h>
#include <string.h>

/* Ephemerons (WeakMap, WeakSet) in the collector, and the collection cursors that Map and Set
 * iterators register. The tests create the cells in scripts, drop the script references, and check
 * with embi_js_coll_size which entries the collector kept. */

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

/* Overwrites a few kilobytes of dead stack below the caller, so stale pointers left by earlier
 * calls are not scanned by the next collection (the same technique as test_js_gc.c). */
static EMB_TEST_NOIPA void scrub_stack_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub_stack() scrub_stack_impl(0, 0, 0, 0)

/* Runs a script. False when it threw (the exception is logged and cleared). */
static EMB_TEST_NOIPA bool run_script(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "gc_weak.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    script threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    return true;
}

/* The collection object held by a global variable. */
static embi_js_object *global_object(env e, const char *name)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    if (!embi_js_is_object(v)) {
        return nullptr;
    }
    return embi_js_as_object(v);
}

/* Runs the collector from a clean stack. */
static EMB_TEST_NOIPA void collect_clean(env e)
{
    scrub_stack();
    embi_js_gc(e.rt);
}

/* Keys that nothing else references are dropped with their entries. */
EMB_TEST(weakmap_entries_die_with_their_keys)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var wm = new WeakMap(); var keep = {}; wm.set(keep, 'kept');"
                  "for (var i = 0; i < 200; i++) { wm.set({}, i); }");
    embi_js_object *wm = global_object(e, "wm");
    EMB_REQUIRE(wm != nullptr);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm), 201);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm), 1);
    EMB_CHECK(run_script(e, "if (wm.get(keep) !== 'kept') { throw new Error('lost the kept entry'); }"));
    env_free(e);
}

/* A value that refers to its own key does not keep the key alive: the map marks a value only when
 * its key is marked. */
EMB_TEST(weakmap_value_does_not_keep_its_key_alive)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var wm2 = new WeakMap(); var k2 = {}; wm2.set(k2, { ref: k2 }); k2 = null;");
    embi_js_object *wm2 = global_object(e, "wm2");
    EMB_REQUIRE(wm2 != nullptr);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm2), 1);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm2), 0);
    env_free(e);
}

/* A chain a -> b -> 'end' through the table survives while a is held, and dies with a. */
EMB_TEST(weakmap_chain_follows_the_keys)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var wm3 = new WeakMap(); var a = {}; var b = {}; wm3.set(a, b); wm3.set(b, 'end');"
                  "var keepA = a; a = null; b = null;");
    embi_js_object *wm3 = global_object(e, "wm3");
    EMB_REQUIRE(wm3 != nullptr);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm3), 2);
    EMB_CHECK(run_script(e, "if (wm3.get(wm3.get(keepA)) !== 'end') { throw new Error('chain broken'); } keepA = null;"));
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(wm3), 0);
    env_free(e);
}

/* Symbols that are not registered are valid weak keys and follow the same rules. */
EMB_TEST(weakset_holds_symbols_and_objects)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var ws = new WeakSet(); var s1 = Symbol('held'); ws.add(s1); ws.add({}); ws.add(Symbol('dropped'));"
                  "var held = s1; s1 = null;");
    embi_js_object *ws = global_object(e, "ws");
    EMB_REQUIRE(ws != nullptr);
    EMB_CHECK_EQ_INT(embi_js_coll_size(ws), 3);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(ws), 1);
    EMB_CHECK(run_script(e, "if (!ws.has(held)) { throw new Error('symbol key lost'); }"));
    env_free(e);
}

/* Strong collections keep their entries, whatever the script still references. */
EMB_TEST(map_and_set_entries_stay_alive)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var sm = new Map(); var ss = new Set(); for (var i = 0; i < 100; i++) { sm.set({}, i); ss.add({}); }");
    embi_js_object *sm = global_object(e, "sm");
    embi_js_object *ss = global_object(e, "ss");
    EMB_REQUIRE(sm != nullptr && ss != nullptr);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(sm), 100);
    EMB_CHECK_EQ_INT(embi_js_coll_size(ss), 100);
    env_free(e);
}

/* A large weak table survives collections, and its half-deleted entries are removed. */
EMB_TEST(weakmap_big_table_survives_collections)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var big = new WeakMap(); var keys = []; for (var i = 0; i < 10000; i++) {"
                  " var k = {}; big.set(k, i); if (i % 2 === 0) { keys.push(k); } }"
                  "k = null;");
    embi_js_object *big = global_object(e, "big");
    EMB_REQUIRE(big != nullptr);
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(big), 5000);
    EMB_CHECK(run_script(e, "var ok = true; for (var j = 0; j < keys.length; j++) { if (big.get(keys[j]) !== j * 2) { ok = false; } }"
                  "if (!ok) { throw new Error('value mismatch'); }"));
    run_script(e, "keys = null;");
    /* One scrubbed collection clears the table: the stack scan starts at the collection entry's
     * frame and reads no word of the frames below it (gc.c collect_entry). */
    collect_clean(e);
    EMB_CHECK_EQ_INT(embi_js_coll_size(big), 0);
    env_free(e);
}

/* Iterators that are dropped do not pile up in the cursor registry: the weak handles of finished
 * or collected cursors are released. Without that, weak_handles would grow with every iterator. */
EMB_TEST(abandoned_iterators_do_not_accumulate)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var m = new Map(); m.set(1, 1); m.set(2, 2);");
    embi_js_mem_stats before;
    embi_js_get_mem_stats(e.rt, &before);
    for (int round = 0; round < 10; round++) {
        run_script(e, "for (var i = 0; i < 500; i++) { m.keys(); m.values().next(); }");
        collect_clean(e);
    }
    embi_js_mem_stats after;
    embi_js_get_mem_stats(e.rt, &after);
    /* 10000 iterators were created. The registry purges lazily, at most about twice the iterators
     * made between two collections stay registered, so the count stays well below 10000. */
    EMB_CHECK(after.weak_handles < before.weak_handles + 3000);
    env_free(e);
}

/* An iterator held by the script keeps working across a collection and a rebuild. */
EMB_TEST(live_iterator_survives_collection_and_rebuild)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    run_script(e, "var lm = new Map(); for (var i = 0; i < 50; i++) { lm.set(i, i); }"
                  "var li = lm.keys(); li.next(); li.next();"
                  "for (var j = 0; j < 40; j++) { lm.delete(j + 2); }"
                  "for (var q = 50; q < 300; q++) { lm.set(q, q); }");
    collect_clean(e);
    EMB_CHECK(run_script(e, "var seen = 0; var first = -1; var r = li.next(); while (!r.done) { if (first < 0) { first = r.value; } seen++; r = li.next(); }"
                  "if (seen !== 258 || first !== 42) { throw new Error('cursor wrong: ' + seen + ' ' + first); }"));
    env_free(e);
}

EMB_TEST_MAIN()
