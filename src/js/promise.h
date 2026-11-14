#pragma once
/*
 * Promise internals (J3a). builtins_promise.c implements the Promise
 * constructor and its functions; this header exposes the abstract operations that the async
 * functions (J3b) and modules (J3c) use, and the job entry points that jobs.c calls.
 */
#include "js/object.h"
#include "js/runtime.h"
#include "js/value.h"

#include <stdbool.h>

/* A PromiseCapability record. All three fields are values (the record lives on the C stack, which
 * the collector scans). */
typedef struct embi_js_pcap {
    embi_js_value promise;
    embi_js_value resolve;
    embi_js_value reject;
} embi_js_pcap_t;

/* AggregateError(errors) with %AggregateError.prototype% (builtins_error.c). errors is an array. */
embi_js_object *embi_js_aggregate_error_new(embi_js_runtime *rt, embi_js_value errors);

/* IsPromise: a Promise object with [[PromiseState]]. */
bool embi_js_is_promise(embi_js_value v);

/* A new pending promise whose prototype is %Promise.prototype% (CreateResolvingFunctions is not
 * needed by the caller; use embi_js_promise_resolve_with and embi_js_promise_reject_with). */
embi_js_object *embi_js_promise_new(embi_js_runtime *rt);

/* The steps of a promise's resolve function, without the alreadyResolved check: a thenable is
 * adopted through a PromiseResolveThenableJob, anything else fulfills the promise. Returns
 * EMBI_JS_UNDEFINED. */
embi_js_value embi_js_promise_resolve_with(embi_js_runtime *rt, embi_js_object *promise,
                                           embi_js_value resolution);
/* Rejects a pending promise with reason. Returns EMBI_JS_UNDEFINED. */
embi_js_value embi_js_promise_reject_with(embi_js_runtime *rt, embi_js_object *promise,
                                          embi_js_value reason);

/* PromiseResolve(C, x): x itself when it is a promise whose constructor is C, else a new promise
 * of C resolved with x. Returns the promise or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_promise_resolve(embi_js_runtime *rt, embi_js_value c, embi_js_value x);

/* NewPromiseCapability(C). Returns false with the exception pending when it throws. */
bool embi_js_promise_capability_new(embi_js_runtime *rt, embi_js_value c, embi_js_pcap_t *out);

/* PerformPromiseThen(promise, onFulfilled, onRejected, resultCapability). cap may be nullptr when
 * the reactions have no capability (await). Non-callable handlers are the identity or thrower.
 * Returns the derived promise (or undefined without a capability). */
embi_js_value embi_js_promise_then(embi_js_runtime *rt, embi_js_object *promise,
                                   embi_js_value on_fulfilled, embi_js_value on_rejected,
                                   const embi_js_pcap_t *cap);

/* Rejection bookkeeping kept in the promise state, for the job queue (jobs.c). handled is
 * [[PromiseIsHandled]]; reported is set when the job queue has reported the rejection as
 * unhandled (a later handler then gets a HANDLE notification). p must be a promise. */
bool embi_js_promise_is_handled(embi_js_value p);
bool embi_js_promise_is_reported(embi_js_value p);
void embi_js_promise_set_reported(embi_js_value p);

/* Job bodies (called from the job queue, jobs.c). Return EMBI_JS_UNDEFINED or EMBI_JS_EXCEPTION
 * for an abrupt completion that the job queue reports to the host. The thenable job with
 * then == EMBI_JS_UNDEFINED is the fast path for a native promise whose then is the intrinsic:
 * it does the same observable lookups as the spec (the constructor, through the species
 * constructor) but creates no resolving functions and no derived promise. */
embi_js_value embi_js_promise_job_reaction(embi_js_runtime *rt, embi_js_value reaction,
                                           embi_js_value argument);
embi_js_value embi_js_promise_job_thenable(embi_js_runtime *rt, embi_js_value promise,
                                           embi_js_value thenable, embi_js_value then);
