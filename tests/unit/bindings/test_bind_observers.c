/*
 * Binding tests (W6d): MutationObserver, ResizeObserver and IntersectionObserver from JS against a real document. Each test
 * builds a fresh document and binding context, and checks that the allocation count is back at its start when it ends.
 * The layout-driven tests of ResizeObserver and IntersectionObserver are in test_bind_observer_ro.c and
 * test_bind_observer_io.c.
 */
#include "emb_test.h"

#include "base/base.h"
#include "bindings/bind.h"
#include "bindings/observers.h"
#include "js/js.h"
#include "observer_harness.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
    size_t mem0;
} env;

static env env_new(void)
{
    static bool warmed;
    if (!warmed) {
        warmed = true;
        ob_warm();
    }
    env e = {0};
    e.mem0 = embi_mem_live();
    e.rt = embi_js_runtime_new();
    e.doc = embi_document_new(true);
    embi_element *html = nullptr;
    embi_element *body = nullptr;
    (void)embi_dom_create_element(e.doc, EMBI_STR("html"), &html);
    (void)embi_append_child(&e.doc->base, &html->base);
    (void)embi_dom_create_element(e.doc, EMBI_STR("body"), &body);
    (void)embi_append_child(&html->base, &body->base);
    e.c = embi_bind_new(e.rt, e.doc);
    return e;
}

static void env_free(env e)
{
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_document_free(e.doc);
    /* Everything the test made is freed again (the observers, their records, the held nodes after release). */
    EMB_CHECK((long long)embi_mem_live() - (long long)e.mem0 == 0);
}

static embi_js_realm *realm(env e)
{
    return embi_bind_realm(e.c);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(realm(e), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = v == embi_js_from_bool(true);
    if (!ok) {
        SDL_Log("    js not true: %s", src);
    }
    return ok;
}

static bool js_throws(env e, const char *src)
{
    embi_js_value v = embi_js_eval(realm(e), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e.rt);
        return true;
    }
    return false;
}

/* Runs the pending microtasks, which is where MutationObserver callbacks are delivered. */
static void drain(env e)
{
    embi_js_run_jobs(e.rt);
}

EMB_TEST(bind_mo_delivers_child_records_in_a_microtask)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var log = []; var box = document.createElement('div'); document.body.appendChild(box);"
                         "var mo = new MutationObserver(function (recs, obs) { log.push([recs.length, obs === mo]); });"
                         "mo.observe(box, {childList: true}); "
                         "box.appendChild(document.createElement('span')); log.length === 0"));
    drain(e);
    EMB_CHECK(js_true(e, "log.length === 1 && log[0][0] === 1 && log[0][1] === true"));
    env_free(e);
}

EMB_TEST(bind_mo_take_records_returns_and_clears_the_queue)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var box = document.createElement('div'); document.body.appendChild(box);"
                         "var calls = 0; var mo = new MutationObserver(function () { calls++; });"
                         "mo.observe(box, {attributes: true, attributeOldValue: true}); "
                         "box.setAttribute('a', '1'); box.setAttribute('a', '2');"
                         "var recs = mo.takeRecords(); recs.length === 2 && recs[0].type === 'attributes' && "
                         "recs[0].attributeName === 'a' && recs[0].oldValue === null && recs[1].oldValue === '1' && "
                         "mo.takeRecords().length === 0"));
    drain(e);
    EMB_CHECK(js_true(e, "calls === 0"));
    env_free(e);
}

EMB_TEST(bind_mo_disconnect_drops_pending_records)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var box = document.createElement('div'); document.body.appendChild(box);"
                         "var calls = 0; var mo = new MutationObserver(function () { calls++; });"
                         "mo.observe(box, {childList: true}); box.appendChild(document.createElement('i'));"
                         "mo.disconnect(); mo.takeRecords().length === 0"));
    drain(e);
    EMB_CHECK(js_true(e, "calls === 0"));
    env_free(e);
}

EMB_TEST(bind_mo_option_errors_are_type_errors)
{
    env e = env_new();
    /* no target kind selected: a TypeError */
    EMB_CHECK(js_throws(e, "new MutationObserver(function () {}).observe(document.body, {});"));
    EMB_CHECK(js_throws(e, "new MutationObserver(function () {}).observe(document.body, {attributes: false, attributeOldValue: true});"));
    EMB_CHECK(js_throws(e, "new MutationObserver(function () {}).observe(document.body, {attributes: false, attributeFilter: ['a']});"));
    EMB_CHECK(js_throws(e, "new MutationObserver(function () {}).observe(document.body, {characterData: false, characterDataOldValue: true});"));
    EMB_CHECK(js_throws(e, "new MutationObserver(42);"));
    /* the presence of the old-value and filter members implies attributes or characterData */
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {attributeOldValue: true});"));
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {characterDataOldValue: true});"));
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {attributeFilter: ['a']});"));
    EMB_CHECK(js_true(e, "var mo = new MutationObserver(function () {}); mo.observe(document.body, {attributeFilter: ['a']}) === undefined"));
    env_free(e);
}

EMB_TEST(bind_mo_false_and_undefined_old_value_members_do_not_throw)
{
    env e = env_new();
    /* A flag that is false (or undefined, which is absent) throws only when the old value is asked for without the kind. */
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {childList: true, attributes: false, attributeOldValue: false});"));
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {childList: true, attributeOldValue: undefined});"));
    EMB_CHECK(!js_throws(e, "new MutationObserver(function () {}).observe(document.body, {childList: true, characterData: false, characterDataOldValue: false});"));
    EMB_CHECK(js_throws(e, "new MutationObserver(function () {}).observe(document.body, {childList: true, attributes: false, attributeOldValue: true});"));
    env_free(e);
}

EMB_TEST(bind_mo_microtask_order_against_promise_jobs)
{
    env e = env_new();
    /* The mutation's microtask is queued at the mutation, so a promise job queued after the mutation runs after it. */
    EMB_CHECK(js_true(e, "var log = []; var box = document.createElement('div'); document.body.appendChild(box);"
                         "var mo = new MutationObserver(function () { log.push('mo'); });"
                         "mo.observe(box, {childList: true}); box.appendChild(document.createElement('i'));"
                         "Promise.resolve().then(function () { log.push('p'); }); true"));
    drain(e);
    EMB_CHECK(js_true(e, "log.join(',') === 'mo,p'"));
    /* A promise job queued before the mutation runs first. */
    EMB_CHECK(js_true(e, "log = []; Promise.resolve().then(function () { log.push('p'); });"
                         "box.appendChild(document.createElement('i')); true"));
    drain(e);
    EMB_CHECK(js_true(e, "log.join(',') === 'p,mo'"));
    env_free(e);
}

EMB_TEST(bind_mo_observers_are_called_in_creation_order)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var log = []; var box = document.createElement('div'); document.body.appendChild(box);"
                         "var a = new MutationObserver(function () { log.push('a'); });"
                         "var b = new MutationObserver(function () { log.push('b'); });"
                         "b.observe(box, {childList: true}); a.observe(box, {childList: true});"
                         "box.appendChild(document.createElement('i')); true"));
    drain(e);
    EMB_CHECK(js_true(e, "log.join(',') === 'a,b'"));
    env_free(e);
}

EMB_TEST(bind_mo_transient_registration_reports_changes_inside_a_removed_subtree)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var root = document.body; var x = document.createElement('div');"
                         "var y = document.createElement('span'); x.appendChild(y); root.appendChild(x);"
                         "var seen = []; var mo = new MutationObserver(function (recs) {"
                         "  for (var i = 0; i < recs.length; i++) seen.push(recs[i].type + ':' + (recs[i].target === y)); });"
                         "mo.observe(root, {subtree: true, attributes: true});"
                         "root.removeChild(x); y.setAttribute('k', '1'); true"));
    drain(e);
    EMB_CHECK(js_true(e, "seen.join(',') === 'attributes:true'"));
    /* The transient registration ended with the notification: a later change inside x queues nothing. */
    EMB_CHECK(js_true(e, "y.setAttribute('k', '2'); true"));
    drain(e);
    EMB_CHECK(js_true(e, "seen.length === 1"));
    env_free(e);
}

EMB_TEST(bind_mo_nested_removal_keeps_observing_the_inner_subtree)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var root = document.body; var a = document.createElement('div');"
                         "var b = document.createElement('span'); a.appendChild(b); root.appendChild(a);"
                         "var seen = []; var mo = new MutationObserver(function (recs) {"
                         "  for (var i = 0; i < recs.length; i++) seen.push(recs[i].type + ':' + (recs[i].target === b)); });"
                         "mo.observe(root, {subtree: true, attributes: true});"
                         "root.removeChild(a); a.removeChild(b); b.setAttribute('k', '1'); true"));
    drain(e);
    EMB_CHECK(js_true(e, "seen.join(',') === 'attributes:true'"));
    env_free(e);
}

EMB_TEST(bind_mo_character_data_records_the_old_value)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = document.createTextNode('one'); document.body.appendChild(t);"
                         "var got = null; var mo = new MutationObserver(function (recs) {"
                         "  got = [recs[0].type, recs[0].oldValue, recs[0].target === t].join('|'); });"
                         "mo.observe(t, {characterData: true, characterDataOldValue: true}); t.data = 'two'; true"));
    drain(e);
    EMB_CHECK(js_true(e, "got === 'characterData|one|true'"));
    env_free(e);
}

EMB_TEST(bind_mo_attribute_filter_selects_the_named_attributes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var box = document.createElement('div'); document.body.appendChild(box);"
                         "var seen = []; var mo = new MutationObserver(function (recs) {"
                         "  for (var i = 0; i < recs.length; i++) seen.push(recs[i].attributeName); });"
                         "mo.observe(box, {attributes: true, attributeFilter: ['data-x']});"
                         "box.setAttribute('data-y', '1'); box.setAttribute('data-x', '1'); true"));
    drain(e);
    EMB_CHECK(js_true(e, "seen.join(',') === 'data-x'"));
    env_free(e);
}

EMB_TEST(bind_mo_subtree_reports_a_deep_change)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var deep = document.createElement('p'); var mid = document.createElement('section');"
                         "mid.appendChild(deep); document.body.appendChild(mid);"
                         "var targets = []; var mo = new MutationObserver(function (recs) {"
                         "  for (var i = 0; i < recs.length; i++) targets.push(recs[i].target === deep); });"
                         "mo.observe(document.body, {childList: true, subtree: true});"
                         "deep.appendChild(document.createElement('b')); true"));
    drain(e);
    EMB_CHECK(js_true(e, "targets.length === 1 && targets[0] === true"));
    env_free(e);
}

EMB_TEST(bind_mo_callback_disconnecting_a_later_observer_stops_its_callback)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var box = document.createElement('div'); document.body.appendChild(box);"
                         "var calls2 = 0; var mo2 = null;"
                         "var mo1 = new MutationObserver(function () { mo2.disconnect(); });"
                         "mo2 = new MutationObserver(function () { calls2++; });"
                         "mo1.observe(box, {childList: true}); mo2.observe(box, {childList: true});"
                         "box.appendChild(document.createElement('i')); true"));
    drain(e);
    EMB_CHECK(js_true(e, "calls2 === 0"));
    env_free(e);
}

EMB_TEST(bind_mo_normalize_queues_the_data_change_before_the_removals)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var box = document.createElement('div'); document.body.appendChild(box);"
                         "var a = document.createTextNode('a'); var b = document.createTextNode('b');"
                         "box.appendChild(a); box.appendChild(b);"
                         "var mo = new MutationObserver(function () {});"
                         "mo.observe(box, {childList: true, characterData: true, subtree: true});"
                         "box.normalize(); var recs = mo.takeRecords();"
                         "recs.length === 2 && recs[0].type === 'characterData' && recs[0].target === a &&"
                         "recs[1].type === 'childList' && recs[1].removedNodes.length === 1 && a.data === 'ab'"));
    env_free(e);
}

EMB_TEST(bind_ro_constructor_requires_a_callable)
{
    env e = env_new();
    EMB_CHECK(js_throws(e, "new ResizeObserver(1);"));
    EMB_CHECK(js_true(e, "typeof new ResizeObserver(function () {}).observe === 'function'"));
    env_free(e);
}

EMB_TEST(bind_ro_after_layout_without_observers_is_a_no_op)
{
    env e = env_new();
    embi_observers_after_layout(e.c);
    EMB_CHECK(js_true(e, "var ro = new ResizeObserver(function () {}); ro.unobserve(document.body); true"));
    embi_observers_after_layout(e.c);
    env_free(e);
}

EMB_TEST(bind_io_constructor_validates_options)
{
    env e = env_new();
    EMB_CHECK(js_throws(e, "new IntersectionObserver(1);"));
    EMB_CHECK(js_throws(e, "new IntersectionObserver(function () {}, {threshold: 1.5});"));
    EMB_CHECK(js_throws(e, "new IntersectionObserver(function () {}, {threshold: [0, -0.1]});"));
    EMB_CHECK(js_throws(e, "new IntersectionObserver(function () {}, {rootMargin: '10 px'});"));
    EMB_CHECK(js_true(e, "var nm = ''; try { new IntersectionObserver(function () {}, {rootMargin: '10 px'}); }"
                         "catch (x) { nm = x.name; } nm === 'SyntaxError'"));
    EMB_CHECK(js_true(e, "var io = new IntersectionObserver(function () {}, {threshold: [1, 0, 0.5]});"
                         "io.thresholds.length === 3 && io.thresholds[0] === 0 && io.thresholds[2] === 1 && "
                         "io.rootMargin === '0px 0px 0px 0px' && io.root === null"));
    env_free(e);
}

EMB_TEST(bind_io_threshold_list_is_not_truncated)
{
    env e = env_new();
    /* 100 thresholds: more than any fixed buffer of the constructor holds. */
    EMB_CHECK(js_true(e, "var t = []; for (var i = 0; i <= 99; i++) t.push(i / 100);"
                         "var io = new IntersectionObserver(function () {}, {threshold: t});"
                         "io.thresholds.length === 100 && io.thresholds[99] === 0.99"));
    env_free(e);
}

/* Timer hooks record the request that the IntersectionObserver scheduled, so the test can fire it. */
typedef struct fake_timers {
    void *payload;
    uint32_t n;
} fake_timers;

static uint32_t fake_add(void *user, double timeout_ms, bool repeat, void *payload)
{
    fake_timers *f = user;
    (void)timeout_ms;
    (void)repeat;
    f->payload = payload;
    return ++f->n;
}

static void *fake_clear(void *user, uint32_t id)
{
    (void)user;
    (void)id;
    return nullptr;
}

EMB_TEST(bind_io_observe_then_after_layout_schedules_delivery_through_the_timer)
{
    env e = env_new();
    fake_timers f = {nullptr, 0};
    embi_bind_timers hooks = {&f, fake_add, fake_clear};
    embi_bind_set_timers(e.c, &hooks);
    EMB_CHECK(js_true(e, "var seen = 0; var io = new IntersectionObserver(function (es) { seen += es.length; });"
                         "io.observe(document.body); true"));
    embi_observers_after_layout(e.c);
    EMB_CHECK(f.payload != nullptr);
    if (f.payload) {
        embi_js_value r = embi_bind_fire_timer(e.c, f.payload);
        EMB_CHECK(r != EMBI_JS_EXCEPTION);
        embi_bind_release_timer(e.c, f.payload);
        drain(e);
    }
    EMB_CHECK(js_true(e, "seen === 1"));
    env_free(e);
}

/* CF1: the binding array helpers (embi_bind_array_len and _get, which attributeFilter reads) take a fast path on a plain
 * dense array and fall back to the ordinary [[Get]] otherwise. Each case gives what the ordinary path gives: holes read
 * through to Array.prototype once it has an index, an accessor runs, a Proxy is asked, a frozen or non-extensible array
 * reads as usual, and the length is read once (as before CF1), so a shrink during the reads gives undefined. */
EMB_TEST(bind_mo_attribute_filter_reads_any_array_like_the_ordinary_get)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "function filt(arr) { var b = document.createElement('b'); document.body.appendChild(b);"
                         "  var mo = new MutationObserver(function () {}); mo.observe(b, {attributeFilter: arr});"
                         "  ['p', 'q', 'r', 's', 'undefined', 'x'].forEach(function (n) { b.setAttribute(n, '1'); });"
                         "  var r = mo.takeRecords().map(function (x) { return x.attributeName; }).join();"
                         "  mo.disconnect(); b.remove(); return r; } true"));
    EMB_CHECK(js_true(e, "filt(['p', 'r']) === 'p,r'"));
    EMB_CHECK(js_true(e, "filt(['p', , 'r']) === 'p,r,undefined'")); /* a hole, nothing on the prototype */
    EMB_CHECK(js_true(e, "filt(Object.freeze(['p', , 'r'])) === 'p,r,undefined'"));
    EMB_CHECK(js_true(e, "filt(Object.preventExtensions(['p', 'q'])) === 'p,q'"));
    EMB_CHECK(js_true(e, "var ga = ['p', 'x', 'r']; Object.defineProperty(ga, 1, {get: function () { return 'q'; }});"
                         "filt(ga) === 'p,q,r'"));
    EMB_CHECK(js_true(e, "var px = new Proxy(['p', 'x'], {get: function (t, k) {"
                         "  return k === '1' ? 's' : k === 'length' ? 3 : t[k]; }}); filt(px) === 'p,s,undefined'"));
    EMB_CHECK(js_true(e, "var sh = [{toString: function () { sh.length = 1; return 'p'; }}, 'q', 'r'];"
                         "filt(sh) === 'p,undefined' && sh.length === 1"));
    EMB_CHECK(js_true(e, "var gr = [{toString: function () { gr.push('s'); return 'p'; }}, 'q']; filt(gr) === 'p,q'"));
    EMB_CHECK(js_true(e, "var sp = ['p', 'x', 'r']; sp[1] = {toString: function () { sp.splice(0, 3); return 'q'; }};"
                         "filt(sp) === 'p,q,undefined'"));
    /* last: an index on Array.prototype turns the no-prototype-elements protection off for this realm */
    EMB_CHECK(js_true(e, "Array.prototype[1] = 'q'; var r1 = filt(['p', , 'r']); delete Array.prototype[1]; r1 === 'p,q,r'"));
    EMB_CHECK(js_true(e, "filt(['p', , 'r']) === 'p,r,undefined'"));
    env_free(e);
}

EMB_TEST_MAIN()
