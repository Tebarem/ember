/*
 * Web Storage through the bindings (W7c): Storage methods, named properties, area independence and persistence across
 * contexts of one backend, quota, opaque origins, and storage events delivered to the other context.
 */
#include "loader_env.h"

#include "loader/storage.h"

EMB_TEST(storage_methods_and_length)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.setItem('a', '1'); localStorage.setItem('b', 2); localStorage.length === 2 && localStorage.getItem('a') === '1' && localStorage.getItem('b') === '2'"));
    EMB_CHECK(js_true(&e, "localStorage.getItem('zzz') === null && localStorage.key(0) === 'a' && localStorage.key(1) === 'b' && localStorage.key(2) === null"));
    EMB_CHECK(js_true(&e, "localStorage.setItem('a', 'changed'); localStorage.length === 2 && localStorage.getItem('a') === 'changed'"));
    EMB_CHECK(js_true(&e, "localStorage.removeItem('a'); localStorage.removeItem('missing'); localStorage.length === 1 && localStorage.getItem('a') === null"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.length === 0 && String(localStorage) === '[object Storage]'"));
    EMB_CHECK(js_true(&e, "localStorage === window.localStorage && localStorage !== sessionStorage"));
    env_done(&e);
}

EMB_TEST(storage_named_properties)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.foo = 'bar'; localStorage.foo === 'bar' && localStorage.getItem('foo') === 'bar' && localStorage.length === 1"));
    EMB_CHECK(js_true(&e, "localStorage['x y'] = 5; localStorage.getItem('x y') === '5' && ('x y' in localStorage)"));
    EMB_CHECK(js_true(&e, "Object.keys(localStorage).sort().join() === 'foo,x y'"));
    EMB_CHECK(js_true(&e, "delete localStorage.foo; localStorage.getItem('foo') === null && localStorage.length === 1"));
    /* A stored key named like a method does not shadow it. */
    EMB_CHECK(js_true(&e, "localStorage.setItem('getItem', 'v'); typeof localStorage.getItem === 'function' && localStorage.getItem('getItem') === 'v'"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.nope === undefined"));
    env_done(&e);
}

EMB_TEST(storage_utf16_keys_and_lone_surrogates)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.setItem('k\\u00e9\\u20ac', 'v\\ud83d\\ude00'); localStorage.getItem('k\\u00e9\\u20ac') === 'v\\ud83d\\ude00'"));
    EMB_CHECK(js_true(&e, "localStorage.setItem('lone', 'a\\ud800b'); localStorage.getItem('lone') === 'a\\ud800b'"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); true"));
    env_done(&e);
}

/* Named-property keys are UTF-16 straight from the JS string: a lone surrogate is kept, and two keys that differ only in
 * a lone surrogate stay distinct (a UTF-8 round trip would turn both into U+FFFD). */
EMB_TEST(storage_named_property_keys_keep_lone_surrogates)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage['k\\ud800'] = 'x'; localStorage.getItem('k\\ud800') === 'x' && localStorage.length === 1"));
    EMB_CHECK(js_true(&e, "localStorage['k\\ud801'] === undefined && localStorage.getItem('k\\ud801') === null && localStorage['k\\ufffd'] === undefined"));
    EMB_CHECK(js_true(&e, "'k\\ud800' in localStorage && !('k\\ud801' in localStorage)"));
    EMB_CHECK(js_true(&e, "(function () { var ks = Object.keys(localStorage); return ks.length === 1 && ks[0].length === 2 && ks[0].charCodeAt(1) === 0xd800; })()"));
    EMB_CHECK(js_true(&e, "localStorage['k\\ud801'] = 'y'; localStorage.length === 2 && localStorage['k\\ud800'] === 'x' && localStorage['k\\ud801'] === 'y'"));
    EMB_CHECK(js_true(&e, "delete localStorage['k\\ud800']; localStorage.getItem('k\\ud800') === null && !('k\\ud800' in localStorage) && localStorage['k\\ud801'] === 'y' && localStorage.length === 1"));
    EMB_CHECK(js_true(&e, "(function () { var ks = Object.keys(localStorage); return ks.length === 1 && ks[0].charCodeAt(1) === 0xd801; })()"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); true"));
    env_done(&e);
}

EMB_TEST(session_storage_is_per_context_and_not_shared)
{
    env a, b;
    env_init(&a);
    env_init(&b);
    EMB_CHECK(js_true(&a, "sessionStorage.setItem('s', '1'); sessionStorage.getItem('s') === '1'"));
    EMB_CHECK(js_true(&b, "sessionStorage.getItem('s') === null && sessionStorage.length === 0"));
    EMB_CHECK(js_true(&a, "sessionStorage.clear(); sessionStorage.length === 0"));
    env_done(&b);
    env_done(&a);
}

EMB_TEST(local_storage_is_per_origin_and_shared_by_backend)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    env a, b, other;
    env_init(&a);
    env_init(&b);
    env_init(&other);
    embi_loader_env le = {0};
    le.storage = st;
    embi_loader_attach(a.c, &le);
    embi_loader_attach(b.c, &le);
    embi_loader_attach(other.c, &le);
    other.url = "http://other.test/";
    EMB_CHECK(js_true(&a, "localStorage.setItem('shared', 'yes'); true"));
    EMB_CHECK(js_true(&b, "localStorage.getItem('shared') === 'yes'"));
    EMB_CHECK(js_true(&other, "localStorage.getItem('shared') === null"));
    env_done(&other);
    env_done(&b);
    env_done(&a);
    embi_storage_close(st);
}

EMB_TEST(local_storage_opaque_origin_throws_security_error)
{
    env e;
    env_init(&e);
    e.url = "about:blank";
    EMB_CHECK(js_true(&e, "(function () { try { localStorage.length; return false; } catch (err) { return err.name === 'SecurityError'; } })()"));
    EMB_CHECK(js_true(&e, "sessionStorage.setItem('ok', '1'); sessionStorage.getItem('ok') === '1'"));
    env_done(&e);
}

EMB_TEST(storage_quota_exceeded)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); var chunk = 'x'.repeat(1024 * 1024); var n = 0, err = null;"
                          "try { for (var i = 0; i < 8; i++) { localStorage.setItem('k' + i, chunk); n++; } } catch (x) { err = x; }"
                          "n >= 4 && n < 8 && err !== null && err.name === 'QuotaExceededError' && localStorage.length === n"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.length === 0"));
    env_done(&e);
}

EMB_TEST(storage_event_constructor_and_attributes)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var ev = new StorageEvent('storage', { key: 'k', oldValue: 'o', newValue: 'n', url: 'http://t.test/', storageArea: localStorage, bubbles: true });"
                          "ev.type === 'storage' && ev.key === 'k' && ev.oldValue === 'o' && ev.newValue === 'n' && ev.url === 'http://t.test/' && ev.storageArea === localStorage && ev.bubbles"));
    EMB_CHECK(js_true(&e, "var ev2 = new StorageEvent('storage'); ev2.key === null && ev2.oldValue === null && ev2.newValue === null && ev2.url === '' && ev2.storageArea === null"));
    EMB_CHECK(js_true(&e, "ev instanceof Event"));
    env_done(&e);
}

/* The post hook of a context: queues the call, as the embedder queues it for the view thread. It must not run it inline (the
 * hook is called with the loader registry locked). */
typedef struct poster {
    int retained, unretained;
    int posted;
    void (*fn[16])(void *);
    void *arg[16];
    void (*drop[16])(void *);
} poster;

static bool poster_post(void *user, void (*fn)(void *arg), void (*drop)(void *arg), void *arg)
{
    poster *p = user;
    if (p->posted >= 16) {
        return false;
    }
    p->drop[p->posted] = drop;
    p->fn[p->posted] = fn;
    p->arg[p->posted] = arg;
    p->posted++;
    return true;
}

static void poster_retain(void *user)
{
    ((poster *)user)->retained++;
}

static void poster_unretain(void *user)
{
    ((poster *)user)->unretained++;
}

/* The queued tasks are discarded unrun, as a navigation or a view destroy does. */
static void poster_drop_all(poster *p)
{
    for (int i = 0; i < p->posted; i++) {
        p->drop[i](p->arg[i]);
    }
    p->posted = 0;
}

static void poster_run(poster *p)
{
    for (int i = 0; i < p->posted; i++) {
        p->fn[i](p->arg[i]);
    }
    p->posted = 0;
}

EMB_TEST(storage_events_reach_other_contexts_only)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    env a, b;
    poster pa = {0}, pb = {0};
    env_init(&a);
    env_init(&b);
    embi_loader_env la = {0}, lb = {0};
    la.storage = st;
    la.user = &pa;
    la.post = poster_post;
    lb.storage = st;
    lb.user = &pb;
    lb.post = poster_post;
    embi_loader_attach(a.c, &la);
    embi_loader_attach(b.c, &lb);
    EMB_CHECK(js_run(&b, "var log = []; window.addEventListener('storage', function (ev) { log.push([String(ev.key), String(ev.oldValue), String(ev.newValue), ev.storageArea === localStorage, ev.url].join('|')); });"));
    EMB_CHECK(js_run(&a, "var alog = []; window.addEventListener('storage', function (ev) { alog.push(ev.key); });"
                         "localStorage.clear(); localStorage.setItem('k', '1'); localStorage.setItem('k', '2'); localStorage.setItem('k', '2'); localStorage.removeItem('k'); localStorage.removeItem('k');"
                         "localStorage.setItem('z', '9'); localStorage.clear();"));
    EMB_CHECK(js_true(&b, "log.length === 0"));
    poster_run(&pb);
    EMB_CHECK(js_true(&b, "log.join(';') === 'k|null|1|true|http://t.test/index.html;k|1|2|true|http://t.test/index.html;k|2|null|true|http://t.test/index.html;z|null|9|true|http://t.test/index.html;null|null|null|true|http://t.test/index.html'"));
    EMB_CHECK(js_true(&a, "alog.length === 0"));
    /* sessionStorage never leaves its context. */
    EMB_CHECK(js_run(&a, "sessionStorage.setItem('q', '1');"));
    EMB_CHECK(js_true(&b, "log.length === 5"));
    env_done(&b);
    env_done(&a);
    embi_storage_close(st);
}

/* Storage events go only to same-origin contexts, and carry the URL of the writer's document. */
EMB_TEST(storage_events_are_per_origin_and_carry_the_writer_url)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    env a, b, other;
    poster pa = {0}, pb = {0}, po = {0};
    env_init(&a);
    env_init(&b);
    env_init(&other);
    other.url = "http://other.test/";
    b.url = "http://t.test/second.html";
    embi_loader_env la = {0}, lb = {0}, lo = {0};
    la.storage = lb.storage = lo.storage = st;
    la.user = &pa;
    lb.user = &pb;
    lo.user = &po;
    la.post = lb.post = lo.post = poster_post;
    embi_loader_attach(a.c, &la);
    embi_loader_attach(b.c, &lb);
    embi_loader_attach(other.c, &lo);
    EMB_CHECK(js_run(&b, "var log = []; window.addEventListener('storage', function (ev) { log.push(ev.key + '|' + ev.url); });"));
    EMB_CHECK(js_run(&other, "var olog = []; window.addEventListener('storage', function (ev) { olog.push(ev.key); });"));
    EMB_CHECK(js_run(&a, "localStorage.setItem('k', '1'); localStorage.removeItem('k'); localStorage.setItem('j', '1'); localStorage.clear();"));
    poster_run(&pb);
    poster_run(&po);
    EMB_CHECK(js_true(&b, "log.join(';') === 'k|http://t.test/index.html;k|http://t.test/index.html;j|http://t.test/index.html;null|http://t.test/index.html'"));
    EMB_CHECK(js_true(&other, "olog.length === 0"));
    /* sessionStorage never fires to other views. */
    EMB_CHECK(js_run(&a, "sessionStorage.setItem('s', '1'); sessionStorage.removeItem('s'); sessionStorage.setItem('s', '2'); sessionStorage.clear();"));
    EMB_CHECK_EQ_INT(pb.posted, 0);
    EMB_CHECK_EQ_INT(po.posted, 0);
    /* The other origin's own changes reach neither same-origin context. */
    EMB_CHECK(js_run(&other, "localStorage.setItem('o', '1');"));
    poster_run(&pa);
    poster_run(&pb);
    EMB_CHECK(js_true(&b, "log.length === 4"));
    env_done(&other);
    env_done(&b);
    env_done(&a);
    embi_storage_close(st);
}

EMB_TEST(queued_storage_events_are_released_when_dropped)
{
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    env a, b;
    poster pa = {0}, pb = {0};
    env_init(&a);
    env_init(&b);
    embi_loader_env la = {0}, lb = {0};
    la.storage = st;
    la.user = &pa;
    la.post = poster_post;
    lb.storage = st;
    lb.user = &pb;
    lb.post = poster_post;
    embi_loader_attach(a.c, &la);
    embi_loader_attach(b.c, &lb);
    EMB_CHECK(js_run(&a, "localStorage.setItem('k', 'aaaa'); var n = 0;"));
    poster_run(&pb);
    /* A change queues one event for b. Discarding it runs no JavaScript, so exactly the event's memory goes away. */
    EMB_CHECK(js_run(&a, "localStorage.setItem('k', 'bbbb');"));
    EMB_CHECK_EQ_INT(pb.posted, 1);
    size_t queued = embi_mem_live();
    poster_drop_all(&pb);
    size_t after = embi_mem_live();
    EMB_CHECK(queued > after && queued - after >= 64);
    env_done(&b);
    env_done(&a);
    embi_storage_close(st);
}

/* More peers than ldr_for_peers' stack buffer: the list is grown outside the lock and every peer hears the event; every
 * peer is retained and unretained in balance around its post. */
EMB_TEST(storage_events_reach_more_than_eight_contexts)
{
    enum { N = 11 };
    embi_storage *st = embi_storage_open(nullptr, nullptr);
    env es[N];
    poster ps[N];
    memset(ps, 0, sizeof ps);
    for (int i = 0; i < N; i++) {
        env_init(&es[i]);
        embi_loader_env l = {0};
        l.storage = st;
        l.user = &ps[i];
        l.post = poster_post;
        l.retain = poster_retain;
        l.unretain = poster_unretain;
        embi_loader_attach(es[i].c, &l);
        EMB_CHECK(js_run(&es[i], "var n = 0; window.addEventListener('storage', function () { n++; });"));
    }
    EMB_CHECK(js_run(&es[0], "localStorage.setItem('multi', '1');"));
    EMB_CHECK_EQ_INT(ps[0].posted, 0);
    EMB_CHECK_EQ_INT(ps[0].retained, 0);
    for (int i = 1; i < N; i++) {
        EMB_CHECK_EQ_INT(ps[i].posted, 1);
        /* A retry of the collection pass retains and unretains again, so only the balance is fixed. */
        EMB_CHECK(ps[i].retained >= 1);
        EMB_CHECK_EQ_INT(ps[i].retained, ps[i].unretained);
        poster_run(&ps[i]);
        EMB_CHECK(js_true(&es[i], "n === 1"));
    }
    EMB_CHECK(js_run(&es[0], "localStorage.clear();"));
    for (int i = N - 1; i >= 0; i--) {
        poster_drop_all(&ps[i]);
        env_done(&es[i]);
    }
    embi_storage_close(st);
}

static int g_released;

static void count_release(void *user)
{
    (void)user;
    g_released++;
}

/* Attaching a new environment aborts the context's in-flight fetches (their operations belong to the old transport) and runs
 * the old environment's release hook once; a context attached without a release hook is not an error. */
EMB_TEST(attach_aborts_in_flight_fetches_and_releases_old_env)
{
    env e;
    env_init(&e);
    fake_route_add(&e, "http://t.test/slow", 200, "text/plain", nullptr, "late", true);
    EMB_CHECK(js_run(&e, "var settled = 0; fetch('http://t.test/slow').then(function () { settled++; }, function () { settled++; });"));
    EMB_CHECK_EQ_INT(e.net.nrequests, 1);
    EMB_CHECK_EQ_INT(e.net.aborted, 0);
    g_released = 0;
    embi_loader_env first = {0};
    first.release = count_release;
    first.transport.user = &e.net;
    first.transport.start = fake_start;
    first.transport.abort = fake_abort;
    embi_loader_attach(e.c, &first);
    EMB_CHECK_EQ_INT(e.net.aborted, 1);
    EMB_CHECK_EQ_INT(g_released, 0);
    settle(&e);
    EMB_CHECK(js_true(&e, "settled === 0"));
    /* Replacing an environment that has jobs of none and a release hook calls the hook once. */
    embi_loader_env second = {0};
    embi_loader_attach(e.c, &second);
    EMB_CHECK_EQ_INT(g_released, 1);
    embi_loader_attach(e.c, &second);
    EMB_CHECK_EQ_INT(g_released, 1);
    env_done(&e);
}

/* QuotaExceededError is a DOMException subclass with quota and requested (null when Storage throws it). */
EMB_TEST(storage_quota_error_is_a_quota_exceeded_error)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); var chunk = 'x'.repeat(1024 * 1024); var err = null;"
                          "try { for (var i = 0; i < 8; i++) localStorage.setItem('k' + i, chunk); } catch (x) { err = x; }"
                          "err instanceof QuotaExceededError && err instanceof DOMException && err instanceof Error && "
                          "err.constructor === QuotaExceededError && err.code === 22 && err.name === 'QuotaExceededError' && "
                          "err.quota === null && err.requested === null && !err.hasOwnProperty('quota')"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); Object.getPrototypeOf(QuotaExceededError.prototype) === DOMException.prototype"));
    EMB_CHECK(js_true(&e, "var q = new QuotaExceededError('m', {quota: 1, requested: 2}); q.message === 'm' && q.quota === 1 && "
                          "q.requested === 2 && q.code === 22 && q.name === 'QuotaExceededError' && new QuotaExceededError().quota === null"));
    EMB_CHECK(js_true(&e, "(function () { var n = 0; try { new QuotaExceededError('', {quota: -1}); } catch (x) { n += x instanceof RangeError; }"
                          "try { new QuotaExceededError('', {quota: 5, requested: 4}); } catch (x) { n += x instanceof RangeError; }"
                          "try { new QuotaExceededError('', {quota: NaN}); } catch (x) { n += x instanceof TypeError; } return n === 3; })()"));
    env_done(&e);
}

/* [[Set]] of a string key is the named setter, whatever the prototype chain holds; symbols are ordinary properties. */
EMB_TEST(storage_set_define_and_symbol_semantics)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "localStorage.clear(); Storage.prototype.x = 'proto'; localStorage.x = 'v';"
                          "var ok = localStorage.x === 'proto' && localStorage.getItem('x') === 'v' && "
                          "Object.getOwnPropertyDescriptor(localStorage, 'x') === undefined; delete Storage.prototype.x; ok"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); var hit = 0; Object.defineProperty(Storage.prototype, 'y', {get: function () { return 'g'; },"
                          "set: function () { hit++; }, configurable: true}); localStorage.y = 'v';"
                          "var ok = hit === 0 && localStorage.y === 'g' && localStorage.getItem('y') === 'v'; delete Storage.prototype.y; ok"));
    /* A key named like a method stores an item and leaves the method. */
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage.key = 'k'; typeof localStorage.key === 'function' && localStorage.getItem('key') === 'k' && localStorage.length === 1"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); var s = Symbol(); localStorage[s] = 't'; var ok = localStorage[s] === 't' && localStorage.length === 0;"
                          "ok = ok && delete localStorage[s] && localStorage[s] === undefined;"
                          "Object.defineProperty(localStorage, s, {value: 'd', configurable: false}); ok && localStorage[s] === 'd' && !delete localStorage[s]"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); localStorage[7] = 'n'; localStorage.getItem('7') === 'n' && localStorage[7] === 'n' && Object.keys(localStorage).join() === '7'"));
    EMB_CHECK(js_true(&e, "localStorage.clear(); true"));
    env_done(&e);
}

EMB_TEST(storage_event_init_storage_event)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var ev = new StorageEvent('storage'); ev.initStorageEvent('t', true, false, 'k', 'o', 'n', 'u', localStorage);"
                          "ev.type === 't' && ev.bubbles && !ev.cancelable && ev.key === 'k' && ev.oldValue === 'o' && ev.newValue === 'n' && "
                          "ev.url === 'u' && ev.storageArea === localStorage && StorageEvent.prototype.initStorageEvent.length === 1"));
    EMB_CHECK(js_true(&e, "var e2 = new StorageEvent('storage'); e2.initStorageEvent('only'); e2.type === 'only' && e2.key === null && e2.url === '' && e2.storageArea === null"));
    env_done(&e);
}

BIND_LOADER_MAIN()
