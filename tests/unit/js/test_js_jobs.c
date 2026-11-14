/*
 * The job queue (jobs.c, J3a): FIFO order of microtasks, exceptions routed to the host without
 * stopping the drain, the interrupt stop and resume, values kept alive by queued jobs, and the
 * unhandled-rejection report (once per drain, and HANDLE when a late handler arrives).
 */
#include "emb_test.h"

#include "js/jobs.h"
#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
    size_t mem0; /* live bytes before the runtime existed: must return here */
} env;

static env env_new(void)
{
    env e;
    e.mem0 = embi_mem_live();
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
    EMB_CHECK(embi_mem_live() == e.mem0);
}

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "jobs.js", 0);
}

static embi_js_value text(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_str_new_utf8(rt, s, strlen(s)));
}

/* True when the global named `name` holds the string want. */
static bool global_text_is(env e, const char *name, const char *want)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_as_string(v);
    size_t n = strlen(want);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint16_t)(unsigned char)want[i]) {
            return false;
        }
    }
    return true;
}

static embi_js_value global_fn(env e, const char *name)
{
    return embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
}

/* ------------------------------------------------------------------ order and drain */

EMB_TEST(jobs_run_in_fifo_order)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function add(x) { log += x; }") != EMBI_JS_EXCEPTION);
    embi_js_value add = global_fn(e, "add");
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "a"));
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "b"));
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "c"));
    EMB_CHECK(global_text_is(e, "log", ""));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "abc"));
    /* An empty queue drains to nothing, and a second drain does not repeat work. */
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "abc"));
    env_free(e);
}

/* A job that queues another job (a promise reaction) extends the same drain: the nested job runs
 * after the jobs that were already queued. */
EMB_TEST(jobs_drain_extends_with_nested_jobs)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = '';"
                       "function leaf(x) { log += 'l' + x; }"
                       "function outer(x) {"
                       "  log += 'o' + x;"
                       "  Promise.resolve().then(function () { log += 'n'; });"
                       "}") != EMBI_JS_EXCEPTION);
    embi_js_value outer = global_fn(e, "outer");
    embi_js_value leaf = global_fn(e, "leaf");
    embi_js_enqueue_microtask(e.rt, outer, text(e.rt, "1"));
    /* Queued from C before the drain: runs after outer, before the nested reaction. */
    embi_js_enqueue_microtask(e.rt, leaf, text(e.rt, "2"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "o1l2n"));
    env_free(e);
}

/* ------------------------------------------------------------------ host exception report */

typedef struct report_state {
    int reports;
    embi_js_value last;
} report_state;

static void count_report(embi_js_runtime *rt, embi_js_value error, void *user)
{
    (void)rt;
    report_state *s = user;
    s->reports++;
    s->last = error;
}

EMB_TEST(jobs_exceptions_go_to_host_and_drain_continues)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function bad() { throw new TypeError('boom'); }"
                       "function ok(x) { log += x; }") != EMBI_JS_EXCEPTION);
    report_state rs = {0, EMBI_JS_UNDEFINED};
    embi_js_set_host_report_error(e.rt, count_report, &rs);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "ok"), text(e.rt, "x"));
    embi_js_enqueue_microtask(e.rt, global_fn(e, "bad"), EMBI_JS_UNDEFINED);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "ok"), text(e.rt, "y"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(rs.reports, 1);
    EMB_CHECK(embi_js_is_object(rs.last));
    EMB_CHECK(global_text_is(e, "log", "xy"));
    /* The pending exception is not left behind for the embedder to see. */
    EMB_CHECK(embi_js_exception_get(e.rt) == EMBI_JS_UNDEFINED);
    embi_js_set_host_report_error(e.rt, nullptr, nullptr);
    env_free(e);
}

/* ------------------------------------------------------------------ interrupt stop and resume */

typedef struct budget {
    int calls_left; /* the hook answers true (stop) once this reaches zero */
} budget;

static bool stop_after(embi_js_runtime *rt, void *user)
{
    (void)rt;
    budget *b = user;
    if (b->calls_left <= 0) {
        return true;
    }
    b->calls_left--;
    return false;
}

EMB_TEST(jobs_interrupt_stops_and_next_drain_resumes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function add(x) { log += x; }") != EMBI_JS_EXCEPTION);
    embi_js_value add = global_fn(e, "add");
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "a"));
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "b"));
    embi_js_enqueue_microtask(e.rt, add, text(e.rt, "c"));
    budget b = {1};
    embi_js_set_interrupt(e.rt, stop_after, &b);
    /* One job runs; the hook then stops the drain with two jobs still queued. */
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "a"));
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "abc"));
    env_free(e);
}

/* ------------------------------------------------------------------ GC keeps queued values */

/* Creates {v: "kept"} and queues read(o). Only the queued record refers to the object afterwards. */
static EMB_TEST_NOIPA void enqueue_reader(env e)
{
    embi_js_object *o = embi_js_object_new_plain(e.rt);
    embi_js_define_raw(e.rt, o, embi_js_key_c(e.rt, "v"), text(e.rt, "kept"),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "read"), embi_js_from_object(o));
}

EMB_TEST(jobs_keep_queued_values_alive)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function read(o) { log = o.v; }") != EMBI_JS_EXCEPTION);
    enqueue_reader(e);
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    embi_js_gc(e.rt);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "kept"));
    env_free(e);
}

/* ------------------------------------------------------------------ unhandled rejections */

typedef struct tracker_state {
    int rejects;
    int handles;
} tracker_state;

static void track(embi_js_runtime *rt, embi_js_value promise, int op, void *user)
{
    (void)rt;
    (void)promise;
    tracker_state *t = user;
    if (op == EMBI_JS_REJECTION_REJECT) {
        t->rejects++;
    } else {
        t->handles++;
    }
}

/* A rejection with no handler is reported once when the drain ends. */
EMB_TEST(rejections_report_once_per_drain)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    tracker_state ts = {0, 0};
    embi_js_set_host_rejection_tracker(e.rt, track, &ts);
    EMB_REQUIRE(run(e, "var p = Promise.reject(new Error('unhandled'));") != EMBI_JS_EXCEPTION);
    EMB_CHECK_EQ_INT(ts.rejects, 0);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.rejects, 1);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.rejects, 1);
    /* A handler that arrives after the report gets a HANDLE notification. */
    EMB_REQUIRE(run(e, "p.catch(function () {});") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.handles, 1);
    EMB_CHECK_EQ_INT(ts.rejects, 1);
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

/* A handler added before the drain ends means the rejection is never reported. */
EMB_TEST(rejections_handled_in_same_drain_are_silent)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    tracker_state ts = {0, 0};
    embi_js_set_host_rejection_tracker(e.rt, track, &ts);
    EMB_REQUIRE(run(e, "Promise.reject(1).catch(function () {});") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.rejects, 0);
    EMB_CHECK_EQ_INT(ts.handles, 0);
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

/* ------------------------------------------------------------------ stops driven by a flag */

/* The stop request is a flag that the host callbacks set, so the hook answers only what the test
 * decided. (A counting hook would also be called by the interpreter on every call.) */
typedef struct flag_stop {
    bool stop;
    int reports;
} flag_stop;

static bool stop_when_set(embi_js_runtime *rt, void *user)
{
    (void)rt;
    return ((const flag_stop *)user)->stop;
}

/* Counts the error and asks the drain to stop, so the job that threw is the last one run. */
static void report_and_stop(embi_js_runtime *rt, embi_js_value error, void *user)
{
    (void)rt;
    (void)error;
    flag_stop *f = user;
    f->reports++;
    f->stop = true;
}

/* A job that throws, and the stop that follows it: the next job waits for the next drain, and
 * the thrown exception does not stay pending. */
EMB_TEST(jobs_interrupt_after_throwing_job)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function bad() { throw new TypeError('boom'); }"
                       "function ok(x) { log += x; }") != EMBI_JS_EXCEPTION);
    flag_stop f = {false, 0};
    embi_js_set_host_report_error(e.rt, report_and_stop, &f);
    embi_js_set_interrupt(e.rt, stop_when_set, &f);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "bad"), EMBI_JS_UNDEFINED);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "ok"), text(e.rt, "x"));
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(f.reports, 1);
    EMB_CHECK(global_text_is(e, "log", ""));
    EMB_CHECK(embi_js_exception_get(e.rt) == EMBI_JS_UNDEFINED);
    f.stop = false;
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "x"));
    EMB_CHECK_EQ_INT(f.reports, 1);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_set_host_report_error(e.rt, nullptr, nullptr);
    env_free(e);
}

/* A drain that the hook stops before the queue is empty reports no unhandled rejection. The next
 * drain, which runs to the end, reports it once. */
EMB_TEST(interrupted_drain_reports_no_rejection)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "function reject_later() { Promise.reject(new Error('late')); }"
                       "function bad() { throw new TypeError('boom'); }") != EMBI_JS_EXCEPTION);
    tracker_state ts = {0, 0};
    flag_stop f = {false, 0};
    embi_js_set_host_rejection_tracker(e.rt, track, &ts);
    embi_js_set_host_report_error(e.rt, report_and_stop, &f);
    embi_js_set_interrupt(e.rt, stop_when_set, &f);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "reject_later"), EMBI_JS_UNDEFINED);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "bad"), EMBI_JS_UNDEFINED);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.rejects, 0);
    f.stop = false;
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(ts.rejects, 1);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_set_host_report_error(e.rt, nullptr, nullptr);
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

/* A job the hook interrupts while it runs: the interrupt error is not a job failure, so the host
 * hears nothing; the job is consumed (not retried), the drain stops, and the later job waits. */
EMB_TEST(jobs_interrupted_mid_run_is_not_reported)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function spin() { for (;;) {} }"
                       "function ok(x) { log += x; }") != EMBI_JS_EXCEPTION);
    report_state rs = {0, EMBI_JS_UNDEFINED};
    embi_js_set_host_report_error(e.rt, count_report, &rs);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "spin"), EMBI_JS_UNDEFINED);
    embi_js_enqueue_microtask(e.rt, global_fn(e, "ok"), text(e.rt, "x"));
    budget b = {20};
    embi_js_set_interrupt(e.rt, stop_after, &b);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(rs.reports, 0);
    EMB_CHECK(embi_js_exception_get(e.rt) == EMBI_JS_UNDEFINED);
    EMB_CHECK(global_text_is(e, "log", ""));
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "x"));
    EMB_CHECK_EQ_INT(rs.reports, 0);
    embi_js_set_host_report_error(e.rt, nullptr, nullptr);
    env_free(e);
}

typedef struct reentrant_state {
    env *e;
    int rejects;
} reentrant_state;

/* The first report enqueues a microtask and attaches a handler to the second, still unreported
 * rejection. */
static void track_reentrant(embi_js_runtime *rt, embi_js_value promise, int op, void *user)
{
    (void)promise;
    reentrant_state *s = user;
    if (op != EMBI_JS_REJECTION_REJECT) {
        return;
    }
    s->rejects++;
    embi_js_enqueue_microtask(rt, global_fn(*s->e, "add"), text(rt, "m"));
    embi_js_call(rt, global_fn(*s->e, "handle2"), EMBI_JS_UNDEFINED, 0, nullptr);
}

EMB_TEST(rejection_report_callbacks_run_in_same_drain)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = ''; function add(x) { log += x; }"
                       "var p1 = Promise.reject(1), p2 = Promise.reject(2);"
                       "function handle2() { p2.catch(function () {}); }") != EMBI_JS_EXCEPTION);
    reentrant_state rs = {&e, 0};
    embi_js_set_host_rejection_tracker(e.rt, track_reentrant, &rs);
    embi_js_run_jobs(e.rt);
    /* Only p1 is reported: p2 was handled by the callback. The microtask ran in this drain. */
    EMB_CHECK_EQ_INT(rs.rejects, 1);
    EMB_CHECK(global_text_is(e, "log", "m"));
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

/* ------------------------------------------------------------------ combinators and ticks */

/* Promise.all: a throw from the per-element resolve closes the iterator (RangeError, return
 * called once). A throw from next does not close it. */
EMB_TEST(combinator_closes_iterator_when_resolve_throws)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var closed = 0, nexts = 0, out = 'pending';"
                       "var iterable = {};"
                       "iterable[Symbol.iterator] = function () {"
                       "  return {"
                       "    next: function () {"
                       "      nexts++; return {value: 1, done: false};"
                       "    },"
                       "    return: function () { closed++; return {}; }"
                       "  };"
                       "};"
                       "Promise.resolve = function () { throw new RangeError('resolve'); };"
                       "Promise.all(iterable).catch(function (e) {"
                       "  out = e.name + ':' + closed + ':' + nexts; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "out", "RangeError:1:1"));
    EMB_REQUIRE(run(e, "var closed2 = 0, out2 = 'pending';"
                       "var it2 = {};"
                       "it2[Symbol.iterator] = function () {"
                       "  return {"
                       "    next: function () { throw new Error('n'); },"
                       "    return: function () { closed2++; return {}; }"
                       "  };"
                       "};"
                       "Promise.all(it2).catch(function (e) {"
                       "  out2 = e.message + ':' + closed2; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "out2", "n:0"));
    env_free(e);
}

/* finally: the callback runs in the first tick, and the value passes on after the thenable
 * returned by the thunk chain (three ticks to the caller's then). */
EMB_TEST(finally_tick_order)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = '';"
                       "Promise.resolve(1).finally(function () { log += 'f'; })"
                       "  .then(function () { log += 't'; });"
                       "Promise.resolve().then(function () { log += 'a'; })"
                       "  .then(function () { log += 'b'; })"
                       "  .then(function () { log += 'c'; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "fabct"));
    env_free(e);
}

/* The native-promise fast path keeps the spec's ticks: resolving with a fulfilled promise takes
 * one extra tick before the handler of the outer promise runs. */
EMB_TEST(thenable_fast_path_ticks)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = '';"
                       "var p = new Promise(function (r) { r(Promise.resolve('x')); });"
                       "p.then(function (v) { log += 'p' + v; });"
                       "Promise.resolve().then(function () { log += '1'; })"
                       "  .then(function () { log += '2'; })"
                       "  .then(function () { log += '3'; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "12px3"));
    env_free(e);
}

/* A rejected native promise adopted through the fast path rejects the outer promise, and the
 * source's rejection counts as handled (nothing is reported). */
EMB_TEST(thenable_fast_path_rejection)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    tracker_state ts = {0, 0};
    embi_js_set_host_rejection_tracker(e.rt, track, &ts);
    EMB_REQUIRE(run(e, "var log = '';"
                       "var rj = Promise.reject('no');"
                       "var p = new Promise(function (r) { r(rj); });"
                       "p.catch(function (v) { log += 'c' + v; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "cno"));
    EMB_CHECK_EQ_INT(ts.rejects, 0);
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

/* A non-native thenable keeps the general path: its then is called in the job, and it resolves
 * synchronously, so the outer promise settles one tick later. */
EMB_TEST(thenable_general_path_ticks)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_REQUIRE(run(e, "var log = '';"
                       "var th = { then: function (res) { log += 'T'; res('v'); } };"
                       "var q = new Promise(function (r) { r(th); });"
                       "q.then(function (v) { log += v; });"
                       "Promise.resolve().then(function () { log += '1'; });") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK(global_text_is(e, "log", "T1v"));
    env_free(e);
}

/* A host callback that stops the pass during the report: the entries not yet reported stay for
 * the next drain. */
static void track_and_stop(embi_js_runtime *rt, embi_js_value promise, int op, void *user)
{
    (void)rt;
    (void)promise;
    (void)op;
    flag_stop *f = user;
    f->reports++;
    f->stop = true;
}

EMB_TEST(report_pass_stopped_by_interrupt_resumes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    flag_stop f = {false, 0};
    embi_js_set_host_rejection_tracker(e.rt, track_and_stop, &f);
    embi_js_set_interrupt(e.rt, stop_when_set, &f);
    EMB_REQUIRE(run(e, "Promise.reject(1); Promise.reject(2);") != EMBI_JS_EXCEPTION);
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(f.reports, 1);
    f.stop = false;
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(f.reports, 2);
    f.stop = false;
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(f.reports, 2);
    embi_js_set_interrupt(e.rt, nullptr, nullptr);
    embi_js_set_host_rejection_tracker(e.rt, nullptr, nullptr);
    env_free(e);
}

EMB_TEST_MAIN()
