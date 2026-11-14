/*
 * The job queue (J3a): enqueueing, the drain loop with its interrupt
 * stop, exception reporting to the host, and the unhandled-rejection bookkeeping that reports a
 * rejection with no handler once per drain.
 */
#include "js/jobs.h"

#include "js/interp.h"
#include "js/js.h"
#include "js/promise.h"

#define UNDEF EMBI_JS_UNDEFINED
#define EXC EMBI_JS_EXCEPTION

/* ------------------------------------------------------------------ records */

static bool is_record(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_VALUE_ARRAY;
}

void embi_js_enqueue_job(embi_js_runtime *rt, enum embi_js_job_kind kind, embi_js_value a,
                         embi_js_value b, embi_js_value c)
{
    embi_js_varray *rec = embi_js_varray_new(rt, 4);
    EMBI_JS_STORE(rec, rec->elems[0], embi_js_from_int32((int32_t)kind));
    EMBI_JS_STORE(rec, rec->elems[1], a);
    EMBI_JS_STORE(rec, rec->elems[2], b);
    EMBI_JS_STORE(rec, rec->elems[3], c);
    embi_js_job_push(rt, embi_js_from_cell(rec));
}

void embi_js_enqueue_microtask(embi_js_runtime *rt, embi_js_value callback, embi_js_value argument)
{
    embi_js_enqueue_job(rt, EMBI_JS_JOB_CALL, callback, argument, UNDEF);
}

void embi_js_set_host_report_error(embi_js_runtime *rt, embi_js_report_error_fn fn, void *user)
{
    rt->host_report_error = fn;
    rt->host_report_error_user = user;
}

void embi_js_set_host_rejection_tracker(embi_js_runtime *rt, embi_js_rejection_fn fn, void *user)
{
    rt->host_rejection = fn;
    rt->host_rejection_user = user;
}

/* The interrupt hook is sticky for an embedder's stop request, so asking it again after a job
 * answers the same way. */
static bool interrupted(embi_js_runtime *rt)
{
    embi_js_vm *vm = rt->vm;
    return vm != nullptr && vm->interrupt != nullptr && vm->interrupt(rt, vm->interrupt_user);
}

/* ------------------------------------------------------------------ unhandled rejections */

/* The list holds the promises rejected with no handler since the last report pass. */
static void mark_unhandled(embi_js_tracer *t, void *user)
{
    const embi_js_runtime *rt = user;
    for (uint32_t i = 0; i < rt->unhandled.len; i++) {
        embi_js_mark_value(t, rt->unhandled.data[i]);
    }
}

void embi_js_track_reject(embi_js_runtime *rt, embi_js_value promise)
{
    if (!rt->unhandled_rooted) {
        embi_js_root_add(rt, mark_unhandled, rt);
        rt->unhandled_rooted = true;
    }
    embi_vec_push(&rt->unhandled, promise);
}

/* A handler is added to a rejected promise. Nothing is removed from the list: the report pass
 * skips a promise whose handled flag is set, so this costs O(1) whatever the list holds. A
 * rejection that was already reported gets the HANDLE notification. */
void embi_js_track_handle(embi_js_runtime *rt, embi_js_value promise)
{
    if (embi_js_promise_is_reported(promise) && rt->host_rejection) {
        rt->host_rejection(rt, promise, EMBI_JS_REJECTION_HANDLE, rt->host_rejection_user);
    }
}

/* Reports the listed rejections that still have no handler. Each entry is taken from the list
 * (by moving the head index) before the host is called. The host may run engine code that adds
 * handlers or rejections, so the loop reads the list again for every entry. Returns false when
 * the interrupt hook stops the pass; the entries that remain stay listed for the next pass. A
 * completed pass clears the list, so a reported rejection is no longer held by the runtime. */
static bool report_unhandled(embi_js_runtime *rt)
{
    while (rt->unhandled_head < rt->unhandled.len) {
        if (interrupted(rt)) {
            return false;
        }
        embi_js_value p = rt->unhandled.data[rt->unhandled_head++];
        if (embi_js_promise_is_handled(p)) {
            continue;
        }
        embi_js_promise_set_reported(p);
        if (rt->host_rejection) {
            rt->host_rejection(rt, p, EMBI_JS_REJECTION_REJECT, rt->host_rejection_user);
        }
    }
    embi_vec_clear(&rt->unhandled);
    rt->unhandled_head = 0;
    return true;
}

/* ------------------------------------------------------------------ KeepDuringJob (ES2026) */

/* The kept set is a value array [list, count] that the root list keeps alive. list holds the
 * kept values in its first count slots and is replaced by a larger one when it fills up. */
static void kept_root_mark(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, user);
}

static embi_js_varray *kept_find(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == kept_root_mark) {
            return rt->roots.data[i].user;
        }
    }
    return nullptr;
}

void embi_js_keep_during_job(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_cell(v)) {
        return;
    }
    embi_js_varray *h = kept_find(rt);
    if (!h) {
        h = embi_js_varray_new(rt, 2);
        EMBI_JS_STORE(h, h->elems[0], embi_js_from_cell(embi_js_varray_new(rt, 16)));
        EMBI_JS_STORE(h, h->elems[1], embi_js_from_int32(0));
        embi_js_root_add(rt, kept_root_mark, h);
    }
    embi_js_varray *list = (embi_js_varray *)embi_js_as_cell(h->elems[0]);
    uint32_t n = (uint32_t)embi_js_as_int32(h->elems[1]);
    if (n == list->hdr.aux) {
        list = embi_js_varray_grow(rt, list, n * 2u);
        EMBI_JS_STORE(h, h->elems[0], embi_js_from_cell(list));
    }
    EMBI_JS_STORE(list, list->elems[n], v);
    EMBI_JS_STORE(h, h->elems[1], embi_js_from_int32((int32_t)(n + 1u)));
}

/* ClearKeptObjects: forgets every kept value. Nothing allocates here. */
static void clear_kept_objects(embi_js_runtime *rt)
{
    embi_js_varray *h = kept_find(rt);
    if (!h) {
        return;
    }
    embi_js_varray *list = (embi_js_varray *)embi_js_as_cell(h->elems[0]);
    uint32_t n = (uint32_t)embi_js_as_int32(h->elems[1]);
    for (uint32_t i = 0; i < n; i++) {
        EMBI_JS_STORE(list, list->elems[i], EMBI_JS_EMPTY);
    }
    EMBI_JS_STORE(h, h->elems[1], embi_js_from_int32(0));
}

/* ------------------------------------------------------------------ the drain */

static void report_exception(embi_js_runtime *rt)
{
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    if (rt->host_report_error) {
        rt->host_report_error(rt, ex, rt->host_report_error_user);
    }
}

static void run_job(embi_js_runtime *rt, embi_js_value job)
{
    embi_js_value res = UNDEF;
    if (!is_record(job)) {
        /* A raw value pushed with embi_js_job_push: a callable runs without arguments. */
        if (embi_js_is_callable(job)) {
            res = embi_js_call(rt, job, UNDEF, 0, nullptr);
        }
    } else {
        embi_js_varray *rec = (embi_js_varray *)embi_js_as_cell(job);
        embi_js_value a = rec->elems[1];
        embi_js_value b = rec->elems[2];
        embi_js_value c = rec->elems[3];
        switch ((enum embi_js_job_kind)embi_js_as_int32(rec->elems[0])) {
        case EMBI_JS_JOB_CALL:
            res = embi_js_call(rt, a, UNDEF, 1, &b);
            break;
        case EMBI_JS_JOB_REACTION:
            res = embi_js_promise_job_reaction(rt, a, b);
            break;
        case EMBI_JS_JOB_THENABLE:
            res = embi_js_promise_job_thenable(rt, a, b, c);
            break;
        }
    }
    if (res == EXC) {
        if (interrupted(rt)) {
            /* The embedder's stop request unwound this job: that is not a job failure, so the
             * host is not told. The job is consumed and is not retried. */
            embi_js_exception_clear(rt);
        } else {
            report_exception(rt);
        }
    }
}

void embi_js_run_jobs(embi_js_runtime *rt)
{
    embi_js_enter(rt);
    for (;;) {
        /* Run jobs until the queue is empty or the hook stops the drain. */
        bool stopped = false;
        embi_js_value job;
        for (;;) {
            if (interrupted(rt)) {
                stopped = true;
                break;
            }
            if (!embi_js_job_pop(rt, &job)) {
                break;
            }
            run_job(rt, job);
        }
        if (stopped || !report_unhandled(rt)) {
            break;
        }
        /* The report may have queued jobs (a host callback that calls queueMicrotask, or
         * engine code it ran). They belong to this drain, so the loop goes round again. */
        if (rt->job_head >= rt->jobs.len) {
            break;
        }
    }
    /* The queue is empty: the synchronous work that kept WeakRef targets is over. */
    if (rt->job_head >= rt->jobs.len) {
        clear_kept_objects(rt);
    }
    embi_js_leave(rt);
}
