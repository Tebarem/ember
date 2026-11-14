#pragma once
/*
 * Job queue (J3a). One microtask queue per runtime (the agent): the
 * PromiseJobs of ECMA-262 and the host's queueMicrotask callbacks. A job is a value array record
 * [kind, a, b, c] kept in rt->jobs (a GC root), so queued jobs keep their values alive.
 *
 * Realms: the spec's job record also carries a realm (section 1 of J3.md). The queue does not
 * store one: a job's callback runs in the realm of the function it calls, and the promise jobs
 * use the intrinsics of the realm of the promise's constructor. This is correct while one runtime
 * has one realm for the promise intrinsics. A multi-realm embedder (an iframe with its own
 * %Promise%) needs a realm slot in the record, which is not added yet.
 */
#include "js/runtime.h"
#include "js/value.h"

/* Job kinds (the first slot of a job record). */
enum embi_js_job_kind {
    EMBI_JS_JOB_CALL,     /* a: callable, b: argument. Calls a with b as its only argument. */
    EMBI_JS_JOB_REACTION, /* a: promise reaction record, b: argument (PromiseReactionJob). */
    EMBI_JS_JOB_THENABLE, /* a: promise to resolve, b: thenable, c: then (PromiseResolveThenableJob).
                           * c undefined: the native-promise fast path (builtins_promise.c). */
};

/* Queues a job of the given kind. Used by the promise machinery (builtins_promise.c). */
void embi_js_enqueue_job(embi_js_runtime *rt, enum embi_js_job_kind kind, embi_js_value a,
                         embi_js_value b, embi_js_value c);

/* HostEnqueueGenericJob for embedders (queueMicrotask): queues callback(argument). */
void embi_js_enqueue_microtask(embi_js_runtime *rt, embi_js_value callback,
                               embi_js_value argument);

/* Runs queued jobs until the queue is empty or the interrupt hook asks to stop (the queue then
 * keeps the rest for the next drain). A job that queues more jobs extends the drain. An exception
 * thrown by a job goes to HostReportErrors and does not stop the drain. When the queue is empty,
 * each rejected promise that still has no handler is reported once through the rejection tracker
 * (a drain the hook stopped reports nothing; the next drain does). Jobs that the host callbacks
 * queue during the report run in the same call. The embedder calls this after every task.
 *
 * Race: a job that fails is taken for an interrupt unwind (the host is not told) when the
 * interrupt hook answers true right after it, so a genuine error thrown just before the embedder
 * sets its stop flag is dropped without a HostReportErrors call. */
void embi_js_run_jobs(embi_js_runtime *rt);

/* Host hooks. Either may be nullptr (exceptions from jobs are then dropped, and unhandled
 * rejections are not reported). */
void embi_js_set_host_report_error(embi_js_runtime *rt, embi_js_report_error_fn fn, void *user);
void embi_js_set_host_rejection_tracker(embi_js_runtime *rt, embi_js_rejection_fn fn, void *user);

/* KeepDuringJob (ES2026, WeakRef): adds v to the kept set, which keeps it alive until the job
 * queue is empty and embi_js_run_jobs clears the set (ClearKeptObjects). Non-cells are ignored. */
void embi_js_keep_during_job(embi_js_runtime *rt, embi_js_value v);

/* Called by the collector after a collection has finished (gc.c). The WeakRef and
 * FinalizationRegistry bookkeeping (builtins_weakref.c) queues the cleanup jobs of the registries
 * whose targets died. It may allocate, because it runs after the collection. */
void embi_js_weakref_after_gc(embi_js_runtime *rt);

/* Rejection bookkeeping for the promise machinery. track_reject is called when a promise is
 * rejected with no handler; track_handle when a handler is added to a rejected promise that had
 * none (it notifies HANDLE only when the rejection was already reported). */
void embi_js_track_reject(embi_js_runtime *rt, embi_js_value promise);
void embi_js_track_handle(embi_js_runtime *rt, embi_js_value promise);
