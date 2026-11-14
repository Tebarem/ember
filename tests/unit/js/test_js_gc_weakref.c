#include "emb_test.h"

#include "js/heap.h"
#include "js/js.h"
#include "js/jobs.h"
#include "js/object.h"
#include "js/ops.h"

#include <stdint.h>
#include <string.h>

/* WeakRef targets and FinalizationRegistry cells in the collector (ES2026 26.1, 26.2), with the
 * kept set (KeepDuringJob) that the job queue clears. The cleanup callbacks are host jobs: a
 * collection only queues them, and they run in the next drain. */

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
 * calls are not scanned by the next collection (the same technique as test_js_gc_weak.c). */
static EMB_TEST_NOIPA void scrub_stack_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub_stack() scrub_stack_impl(0, 0, 0, 0)

static EMB_TEST_NOIPA bool run_script(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "gc_weakref.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    script threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    return true;
}

/* Runs the collector from a clean stack. */
static EMB_TEST_NOIPA void collect_clean(env e)
{
    scrub_stack();
    embi_js_gc(e.rt);
}

/* Evaluates a boolean expression. */
static EMB_TEST_NOIPA bool script_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "gc_weakref.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    return v == EMBI_JS_TRUE;
}

/* A target that nothing else holds dies in a collection: deref then reads undefined. The
 * constructor's kept-set entry goes with the drain, so the drain comes first. */
EMB_TEST(weakref_target_dies_after_drain)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var wr = (function () { return new WeakRef({ big: 1 }); })();"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    EMB_CHECK(script_true(e, "wr.deref() === undefined"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A target that the script still holds survives collections. */
EMB_TEST(weakref_target_kept_by_script_survives)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var held = {tag: 'held'}; var wh = new WeakRef(held);"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    collect_clean(e);
    EMB_CHECK(script_true(e, "wh.deref() === held && held.tag === 'held'"));
    env_free(e);
}

/* KeepDuringJob: deref and the constructor keep the target until the job queue drains, even
 * when nothing else refers to it. */
EMB_TEST(weakref_target_kept_until_drain)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var wk = (function () { var w = new WeakRef({ v: 42 }); "
                            "return w; })();"));
    collect_clean(e);
    /* Still inside the same synchronous job: the constructor kept the target. */
    EMB_CHECK(script_true(e, "wk.deref() !== undefined && wk.deref().v === 42"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    EMB_CHECK(script_true(e, "wk.deref() === undefined"));
    env_free(e);
}

/* deref in a job keeps the target through a collection in that same job, and the drain lets it
 * go. */
EMB_TEST(weakref_deref_keeps_target_during_job)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var wd = (function () { return new WeakRef({ n: 5 }); })();"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(run_script(e, "wd.deref().n;"));
    collect_clean(e);
    EMB_CHECK(script_true(e, "wd.deref() !== undefined && wd.deref().n === 5"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    EMB_CHECK(script_true(e, "wd.deref() === undefined"));
    env_free(e);
}

/* A registered target's cleanup runs as a job after the collection that finds it dead. */
EMB_TEST(finalization_cleanup_runs_after_collection)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var log = []; var fr = new FinalizationRegistry(function (h) { "
                            "log.push(h); }); (function () { fr.register({}, 'first'); "
                            "fr.register({}, 'second'); })();"));
    embi_js_run_jobs(e.rt);
    /* No cleanup before a collection finds the targets dead. */
    EMB_CHECK(script_true(e, "log.length === 0"));
    collect_clean(e);
    /* The cleanups are jobs now: nothing ran yet. */
    EMB_CHECK(script_true(e, "log.length === 0"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(script_true(e, "log.length === 2 && log.indexOf('first') >= 0 && "
                             "log.indexOf('second') >= 0"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Cleanup is not run for a target that is still alive, and a registration with an unregister
 * token that was removed is never cleaned. */
EMB_TEST(finalization_live_target_and_unregistered_token)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var log2 = []; var fr2 = new FinalizationRegistry(function (h) { "
                            "log2.push(h); }); var alive = {}; fr2.register(alive, 'alive');"
                            "var tok = {}; (function () { fr2.register({}, 'gone', tok); "
                            "})(); fr2.unregister(tok);"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    embi_js_run_jobs(e.rt);
    /* 'gone' was unregistered before its target died, so nothing is queued for it. 'alive' is
     * still held by the script. */
    EMB_CHECK(script_true(e, "log2.length === 0 && alive !== undefined"));
    env_free(e);
}

/* A registry that the script drops dies with its cells, and a later collection with its target
 * gone queues nothing for it. */
EMB_TEST(finalization_registry_dies_quietly)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var calls = 0; (function () { var f = new FinalizationRegistry(function () "
                            "{ calls++; }); f.register({}, 1); f.register({}, 2); })();"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    collect_clean(e);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(script_true(e, "calls === 0"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A cleanup callback that throws does not stop the drain: the error goes to the host. The later
 * registration is still cleaned. */
EMB_TEST(finalization_cleanup_throw_does_not_block_others)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var seen = []; var fr3 = new FinalizationRegistry(function (h) { "
                            "seen.push(h); if (h === 'bad') { throw new Error('boom'); } }); "
                            "(function () { fr3.register({}, 'bad'); fr3.register({}, 'good'); "
                            "})();"));
    embi_js_run_jobs(e.rt);
    collect_clean(e);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(script_true(e, "seen.length === 2"));
    env_free(e);
}

/* Many targets through many collections: the registry list and the weak handles stay bounded. */
EMB_TEST(finalization_churn_is_bounded)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_mem_stats before;
    embi_js_get_mem_stats(e.rt, &before);
    EMB_CHECK(run_script(e, "var n = 0; var churn = new FinalizationRegistry(function () { n++; });"));
    for (int round = 0; round < 20; round++) {
        EMB_CHECK(run_script(e, "for (var i = 0; i < 200; i++) { churn.register({}, i); "
                                "new WeakRef({}); }"));
        embi_js_run_jobs(e.rt);
        collect_clean(e);
        embi_js_run_jobs(e.rt);
    }
    embi_js_mem_stats after;
    embi_js_get_mem_stats(e.rt, &after);
    EMB_CHECK(script_true(e, "n > 0"));
    /* 4000 registrations and 4000 WeakRefs were made. Their weak handles are released once they
     * have died, so only the handles of the last round can still be in place. */
    EMB_CHECK(after.weak_handles < before.weak_handles + 1000);
    env_free(e);
}

/* Registrations that share one long-lived unregister token: each dead target releases its
 * registration, token handle included, so the weak table stays bounded even though the token
 * never dies. */
EMB_TEST(finalization_live_token_does_not_leak)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_mem_stats before;
    embi_js_get_mem_stats(e.rt, &before);
    EMB_CHECK(run_script(e, "var tokHits = 0; var tokReg = new FinalizationRegistry(function () { "
                            "tokHits++; }); var longTok = {};"));
    for (int round = 0; round < 20; round++) {
        EMB_CHECK(run_script(e, "for (var i = 0; i < 200; i++) { tokReg.register({}, i, longTok); }"));
        embi_js_run_jobs(e.rt);
        collect_clean(e);
        embi_js_run_jobs(e.rt);
    }
    embi_js_mem_stats after;
    embi_js_get_mem_stats(e.rt, &after);
    EMB_CHECK(script_true(e, "tokHits > 0 && longTok !== undefined"));
    /* 4000 registrations were made, each with its own token handle. Only the last round's
     * handles can still be in place. */
    EMB_CHECK(after.weak_handles < before.weak_handles + 1000);
    env_free(e);
}

/* A collection that runs inside the registry list's growth (the allocation of the bigger list)
 * compacts the list: a dead registry drops out and the count goes down. The new registration must
 * then go to the compacted end of the list, with no hole, and the next collection must still walk
 * the list cleanly. The constructor is called from C so the allocation that grows the list is the
 * one that trips the collector: the heap is primed so the object allocation stays below the
 * trigger and the grow's allocation crosses it. */
EMB_TEST(finalization_registry_push_survives_collection_during_grow)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(run_script(e, "var log = []; var cbk = function (h) { log.push(h); }; "
                            "var keepers = []; for (var i = 0; i < 7; i++) { "
                            "keepers.push(new FinalizationRegistry(cbk)); } "
                            "(function () { new FinalizationRegistry(cbk); })(); "
                            "for (var j = 0; j < 7; j++) { keepers[j].register({}, 'k' + j); }"));
    embi_js_run_jobs(e.rt);
    scrub_stack();

    /* The list now holds 8 entries (its full capacity), one of them a dead registry. */
    embi_js_value ctor = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "FinalizationRegistry"));
    embi_js_value cbk = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "cbk"));
    EMB_REQUIRE(embi_js_is_object(ctor) && embi_js_is_callable(cbk));

    embi_js_heap *h = &e.rt->heap;
    h->live_after_gc = 0;
    h->bytes_since_gc = h->min_trigger - 1u;
    size_t gc0 = h->gc_count;
    embi_js_value args[1] = {cbk};
    embi_js_value nf = embi_js_construct(e.rt, ctor, 1, args, ctor);
    EMB_REQUIRE(nf != EMBI_JS_EXCEPTION);
    EMB_CHECK(h->gc_count > gc0);

    /* A second collection walks the list again; before the fix this read a hole. */
    scrub_stack();
    embi_js_gc(e.rt);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(script_true(e, "log.length === 7"));
    env_free(e);
}

EMB_TEST_MAIN()
