/*
 * Promise (ECMA-262 section 27.2, J3a): the constructor, the reaction and capability records, the
 * resolving functions, then/catch/finally, the static combinators (all, allSettled, any, race),
 * resolve, reject, withResolvers and try, and @@species.
 *
 * Records are value arrays, so the collector traces them through the values they hold:
 *   promise state  [status, result, fulfill head, fulfill tail, reject head, reject tail, handled,
 *                   reported]
 *   reaction       [capability promise, capability resolve, capability reject, handler, type, next]
 *   function state [kind, ...]  the internal slots of the built-in functions this file creates
 * A Promise object keeps its state record in o->internal (class trace marks it). The built-in
 * functions are objects of class_pfunc, which dispatches on the kind in their state record.
 */
#include "js/builtins.h"
#include "js/interp.h"
#include "js/jobs.h"
#include "js/js.h"
#include "js/promise.h"

#include <string.h>

#define UNDEF EMBI_JS_UNDEFINED
#define EXC EMBI_JS_EXCEPTION
#define EMPTY EMBI_JS_EMPTY
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define ATTR_WEC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE)

enum promise_status { ST_PENDING = 0, ST_FULFILLED = 1, ST_REJECTED = 2 };

enum promise_slot {
    PS_STATUS,
    PS_RESULT,
    PS_FHEAD,
    PS_FTAIL,
    PS_RHEAD,
    PS_RTAIL,
    PS_HANDLED,
    PS_REPORTED, /* the job queue reported this rejection as unhandled (jobs.c) */
    PS_LEN,
};

enum reaction_slot {
    RX_CAP_PROMISE,
    RX_CAP_RESOLVE,
    RX_CAP_REJECT,
    RX_HANDLER,
    RX_TYPE,
    RX_NEXT,
    RX_LEN,
};

enum reaction_type { RX_FULFILL = 0, RX_REJECT = 1 };

enum pfunc_kind {
    PF_RESOLVING,   /* [kind, promise, already record, which (0 resolve, 1 reject)] */
    PF_EXECUTOR,    /* [kind, resolve, reject] */
    PF_ELEMENT,     /* [kind, index, values, remaining record, cap resolve, cap reject,
                     *  already record, mode] */
    PF_FINALLY,     /* [kind, onFinally, C, which (0 then, 1 catch)] */
    PF_VALUE_THUNK, /* [kind, value] */
    PF_THROWER,     /* [kind, reason] */
};

enum element_mode {
    EL_ALL,               /* Promise.all: fulfilled values, rejections reject the whole */
    EL_SETTLED_FULFILLED, /* Promise.allSettled: {status: "fulfilled", value} */
    EL_SETTLED_REJECTED,  /* Promise.allSettled: {status: "rejected", reason} */
    EL_ANY,               /* Promise.any: rejection reasons, collected into an AggregateError */
};

enum combinator { CB_ALL, CB_ALL_SETTLED, CB_ANY, CB_RACE };

/* ------------------------------------------------------------------ helpers */

static embi_js_varray *as_varray(embi_js_value v) { return (embi_js_varray *)embi_js_as_cell(v); }

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i) {
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value take_exc(embi_js_runtime *rt) {
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    return ex;
}

/* Makes a TypeError pending and returns EXC. */
static embi_js_value throw_type(embi_js_runtime *rt, const char *msg) {
    return embi_js_throw_type_error(rt, "%s", msg);
}

static bool is_object_value(embi_js_value v) { return embi_js_is_object(v); }

static embi_js_value str_value(embi_js_runtime *rt, const char *s) {
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* ------------------------------------------------------------------ object classes */

static void trace_internal(embi_js_object *o, embi_js_tracer *t) {
    if (o->internal != nullptr) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static embi_js_value pfunc_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                int argc, const embi_js_value *argv);

/* Promise objects: the class tells IsPromise apart. */
static const embi_js_class class_promise = {
    .name = "Promise",
    .trace = trace_internal,
};

/* The built-in functions of this file (resolving functions, executors, combinator elements,
 * finally helpers). They are not constructors and have no prototype property. */
static const embi_js_class class_pfunc = {
    .name = "Function",
    .call = pfunc_call,
    .trace = trace_internal,
};

/* ------------------------------------------------------------------ records */

static embi_js_varray *new_state(embi_js_runtime *rt) {
    embi_js_varray *st = embi_js_varray_new(rt, PS_LEN);
    EMBI_JS_STORE(st, st->elems[PS_STATUS], embi_js_from_int32(ST_PENDING));
    EMBI_JS_STORE(st, st->elems[PS_RESULT], UNDEF);
    EMBI_JS_STORE(st, st->elems[PS_FHEAD], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_FTAIL], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_RHEAD], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_RTAIL], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_HANDLED], EMBI_JS_FALSE);
    EMBI_JS_STORE(st, st->elems[PS_REPORTED], EMBI_JS_FALSE);
    return st;
}

static embi_js_varray *state_of(const embi_js_object *p) { return (embi_js_varray *)p->internal; }

/* A reaction record. cap is nullptr (or undefined members) for a reaction without capability. */
static embi_js_varray *reaction_new(embi_js_runtime *rt, const embi_js_pcap_t *cap,
                                    embi_js_value handler, enum reaction_type type) {
    embi_js_varray *rx = embi_js_varray_new(rt, RX_LEN);
    EMBI_JS_STORE(rx, rx->elems[RX_CAP_PROMISE], cap != nullptr ? cap->promise : UNDEF);
    EMBI_JS_STORE(rx, rx->elems[RX_CAP_RESOLVE], cap != nullptr ? cap->resolve : UNDEF);
    EMBI_JS_STORE(rx, rx->elems[RX_CAP_REJECT], cap != nullptr ? cap->reject : UNDEF);
    EMBI_JS_STORE(rx, rx->elems[RX_HANDLER], handler);
    EMBI_JS_STORE(rx, rx->elems[RX_TYPE], embi_js_from_int32((int32_t)type));
    EMBI_JS_STORE(rx, rx->elems[RX_NEXT], EMPTY);
    return rx;
}

/* Appends node to the list kept in st[head .. tail]. */
static void list_append(embi_js_varray *st, enum promise_slot head, enum promise_slot tail,
                        embi_js_varray *node) {
    if (st->elems[tail] == EMPTY) {
        EMBI_JS_STORE(st, st->elems[head], embi_js_from_cell(node));
    } else {
        /* The old tail node may be old while node is young: it owns the link. */
        embi_js_varray *t = as_varray(st->elems[tail]);
        EMBI_JS_STORE(t, t->elems[RX_NEXT], embi_js_from_cell(node));
    }
    EMBI_JS_STORE(st, st->elems[tail], embi_js_from_cell(node));
}

/* ------------------------------------------------------------------ built-in function objects */

static embi_js_object *pfunc_new(embi_js_runtime *rt, int length, embi_js_varray *state) {
    embi_js_object *f =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], &class_pfunc);
    f->internal = state;
    EMBI_JS_WB_STRUCT(f);
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                       embi_js_number((double)length), ATTR_C);
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_NAME), str_value(rt, ""), ATTR_C);
    return f;
}

/* CreateResolvingFunctions: a resolve and a reject function sharing one alreadyResolved flag. */
static void create_resolving(embi_js_runtime *rt, embi_js_object *p, embi_js_object **res,
                             embi_js_object **rej) {
    embi_js_varray *already = embi_js_varray_new(rt, 1);
    EMBI_JS_STORE(already, already->elems[0], EMBI_JS_FALSE);
    embi_js_varray *st_res = embi_js_varray_new(rt, 4);
    EMBI_JS_STORE(st_res, st_res->elems[0], embi_js_from_int32(PF_RESOLVING));
    EMBI_JS_STORE(st_res, st_res->elems[1], embi_js_from_object(p));
    EMBI_JS_STORE(st_res, st_res->elems[2], embi_js_from_cell(already));
    EMBI_JS_STORE(st_res, st_res->elems[3], embi_js_from_int32(0));
    *res = pfunc_new(rt, 1, st_res);
    embi_js_varray *st_rej = embi_js_varray_new(rt, 4);
    EMBI_JS_STORE(st_rej, st_rej->elems[0], embi_js_from_int32(PF_RESOLVING));
    EMBI_JS_STORE(st_rej, st_rej->elems[1], embi_js_from_object(p));
    EMBI_JS_STORE(st_rej, st_rej->elems[2], embi_js_from_cell(already));
    EMBI_JS_STORE(st_rej, st_rej->elems[3], embi_js_from_int32(1));
    *rej = pfunc_new(rt, 1, st_rej);
}

/* ------------------------------------------------------------------ settling and jobs */

/* Queues a reaction job for each reaction of a list, in order. */
static void trigger(embi_js_runtime *rt, embi_js_value list, embi_js_value argument) {
    embi_js_value node = list;
    while (node != EMPTY) {
        embi_js_value next = as_varray(node)->elems[RX_NEXT];
        embi_js_enqueue_job(rt, EMBI_JS_JOB_REACTION, node, argument, UNDEF);
        node = next;
    }
}

/* FulfillPromise / RejectPromise: records the result and triggers the matching reactions. */
static void settle(embi_js_runtime *rt, embi_js_object *p, enum promise_status status,
                   embi_js_value value) {
    embi_js_varray *st = state_of(p);
    embi_js_value list = status == ST_FULFILLED ? st->elems[PS_FHEAD] : st->elems[PS_RHEAD];
    EMBI_JS_STORE(st, st->elems[PS_STATUS], embi_js_from_int32(status));
    EMBI_JS_STORE(st, st->elems[PS_RESULT], value);
    EMBI_JS_STORE(st, st->elems[PS_FHEAD], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_FTAIL], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_RHEAD], EMPTY);
    EMBI_JS_STORE(st, st->elems[PS_RTAIL], EMPTY);
    if (status == ST_REJECTED && st->elems[PS_HANDLED] != EMBI_JS_TRUE) {
        embi_js_track_reject(rt, embi_js_from_object(p));
    }
    trigger(rt, list, value);
}

embi_js_object *embi_js_promise_new(embi_js_runtime *rt) {
    embi_js_object *p =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_PROMISE], &class_promise);
    p->internal = new_state(rt);
    EMBI_JS_WB_STRUCT(p);
    return p;
}

bool embi_js_is_promise(embi_js_value v) {
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == &class_promise;
}

bool embi_js_promise_is_handled(embi_js_value p) {
    return state_of(embi_js_as_object(p))->elems[PS_HANDLED] == EMBI_JS_TRUE;
}

bool embi_js_promise_is_reported(embi_js_value p) {
    return state_of(embi_js_as_object(p))->elems[PS_REPORTED] == EMBI_JS_TRUE;
}

void embi_js_promise_set_reported(embi_js_value p) {
    EMBI_JS_STORE(state_of(embi_js_as_object(p)), state_of(embi_js_as_object(p))->elems[PS_REPORTED], EMBI_JS_TRUE);
}

embi_js_value embi_js_promise_reject_with(embi_js_runtime *rt, embi_js_object *promise,
                                          embi_js_value reason) {
    settle(rt, promise, ST_REJECTED, reason);
    return UNDEF;
}

embi_js_value embi_js_promise_resolve_with(embi_js_runtime *rt, embi_js_object *promise,
                                           embi_js_value resolution) {
    embi_js_value self = embi_js_from_object(promise);
    if (resolution == self) {
        embi_js_throw_type_error(rt, "Chaining cycle detected for promise");
        settle(rt, promise, ST_REJECTED, take_exc(rt));
        return UNDEF;
    }
    if (!embi_js_is_object(resolution)) {
        settle(rt, promise, ST_FULFILLED, resolution);
        return UNDEF;
    }
    embi_js_value then =
        embi_js_get(rt, embi_js_as_object(resolution), embi_js_name_val(rt, EMBI_JS_NAME_THEN));
    if (then == EXC) {
        settle(rt, promise, ST_REJECTED, take_exc(rt));
        return UNDEF;
    }
    if (!embi_js_is_callable(then)) {
        settle(rt, promise, ST_FULFILLED, resolution);
        return UNDEF;
    }
    if (then == rt->protos[EMBI_JS_PROTO_PROMISE_THEN] && embi_js_is_promise(resolution)) {
        /* Fast path: the same job with the then left out (see embi_js_promise_job_thenable). */
        embi_js_enqueue_job(rt, EMBI_JS_JOB_THENABLE, self, resolution, UNDEF);
        return UNDEF;
    }
    embi_js_enqueue_job(rt, EMBI_JS_JOB_THENABLE, self, resolution, then);
    return UNDEF;
}

/* PromiseReactionJob. */
embi_js_value embi_js_promise_job_reaction(embi_js_runtime *rt, embi_js_value reaction,
                                           embi_js_value argument) {
    embi_js_varray *rx = as_varray(reaction);
    embi_js_value handler = rx->elems[RX_HANDLER];
    bool rejected = embi_js_as_int32(rx->elems[RX_TYPE]) == RX_REJECT;
    embi_js_value value = argument;
    bool abrupt = rejected;
    if (rx->elems[RX_CAP_RESOLVE] == UNDEF && rx->elems[RX_CAP_PROMISE] != UNDEF &&
        handler == EMPTY) {
        /* Internal reaction of the thenable fast path: the target promise is settled by the steps
         * of its resolving functions, which the fast path does not create. */
        embi_js_object *target = embi_js_as_object(rx->elems[RX_CAP_PROMISE]);
        if (rejected) {
            return embi_js_promise_reject_with(rt, target, argument);
        }
        return embi_js_promise_resolve_with(rt, target, argument);
    }
    if (handler != EMPTY) {
        value = embi_js_call(rt, handler, UNDEF, 1, &argument);
        abrupt = value == EXC;
        if (abrupt) {
            value = take_exc(rt);
        }
    }
    if (rx->elems[RX_CAP_PROMISE] == UNDEF) {
        /* No capability (await): an abrupt handler result has nowhere to go, so it is reported. */
        return abrupt ? embi_js_throw_value(rt, value) : UNDEF;
    }
    if (rx->elems[RX_CAP_RESOLVE] == UNDEF) {
        /* Internal capability (async-from-sync, J3b): settle the target promise directly. */
        embi_js_object *target = embi_js_as_object(rx->elems[RX_CAP_PROMISE]);
        if (abrupt) {
            return embi_js_promise_reject_with(rt, target, value);
        }
        return embi_js_promise_resolve_with(rt, target, value);
    }
    embi_js_value fn = abrupt ? rx->elems[RX_CAP_REJECT] : rx->elems[RX_CAP_RESOLVE];
    return embi_js_call(rt, fn, UNDEF, 1, &value);
}

/* PerformPromiseThen with the derived capability made from c: the body of then once the species
 * constructor is known. Returns EXC when the capability cannot be made. */
static embi_js_value then_with(embi_js_runtime *rt, embi_js_object *p, embi_js_value c,
                               embi_js_value on_f, embi_js_value on_r) {
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, c, &cap)) {
        return EXC;
    }
    return embi_js_promise_then(rt, p, on_f, on_r, &cap);
}

/* PromiseResolveThenableJob. With then undefined it is the fast path for a native promise whose
 * then is the intrinsic. The only observable step of that then is the species lookup, which is
 * done here as the spec does it. For %Promise% the derived promise is not observable, so the
 * reaction settles promise directly, in the tick the spec's resolving functions would. */
embi_js_value embi_js_promise_job_thenable(embi_js_runtime *rt, embi_js_value promise,
                                           embi_js_value thenable, embi_js_value then) {
    embi_js_object *p = embi_js_as_object(promise);
    embi_js_object *t = embi_js_as_object(thenable);
    if (then == UNDEF) {
        embi_js_value intrinsic = rt->protos[EMBI_JS_PROTO_PROMISE_CTOR];
        embi_js_value c = embi_js_species_constructor(rt, t, intrinsic);
        if (c == EXC) {
            return embi_js_promise_reject_with(rt, p, take_exc(rt));
        }
        if (c == intrinsic) {
            embi_js_pcap_t internal = {promise, UNDEF, UNDEF};
            (void)embi_js_promise_then(rt, t, EMPTY, EMPTY, &internal);
            return UNDEF;
        }
        embi_js_object *res;
        embi_js_object *rej;
        create_resolving(rt, p, &res, &rej);
        embi_js_value args[2] = {embi_js_from_object(res), embi_js_from_object(rej)};
        if (then_with(rt, t, c, args[0], args[1]) == EXC) {
            embi_js_value err = take_exc(rt);
            (void)embi_js_call(rt, args[1], UNDEF, 1, &err);
        }
        return UNDEF;
    }
    embi_js_object *res;
    embi_js_object *rej;
    create_resolving(rt, p, &res, &rej);
    embi_js_value args[2] = {embi_js_from_object(res), embi_js_from_object(rej)};
    embi_js_value r = embi_js_call(rt, then, thenable, 2, args);
    if (r == EXC) {
        embi_js_value err = take_exc(rt);
        (void)embi_js_call(rt, args[1], UNDEF, 1, &err);
    }
    return UNDEF;
}

/* PerformPromiseThen. */
embi_js_value embi_js_promise_then(embi_js_runtime *rt, embi_js_object *p, embi_js_value on_f,
                                   embi_js_value on_r, const embi_js_pcap_t *cap) {
    if (!embi_js_is_callable(on_f)) {
        on_f = EMPTY;
    }
    if (!embi_js_is_callable(on_r)) {
        on_r = EMPTY;
    }
    embi_js_varray *fr = reaction_new(rt, cap, on_f, RX_FULFILL);
    embi_js_varray *rr = reaction_new(rt, cap, on_r, RX_REJECT);
    embi_js_varray *st = state_of(p);
    int status = embi_js_as_int32(st->elems[PS_STATUS]);
    if (status == ST_PENDING) {
        list_append(st, PS_FHEAD, PS_FTAIL, fr);
        list_append(st, PS_RHEAD, PS_RTAIL, rr);
    } else if (status == ST_FULFILLED) {
        embi_js_enqueue_job(rt, EMBI_JS_JOB_REACTION, embi_js_from_cell(fr), st->elems[PS_RESULT],
                            UNDEF);
    } else {
        if (st->elems[PS_HANDLED] != EMBI_JS_TRUE) {
            embi_js_track_handle(rt, embi_js_from_object(p));
        }
        embi_js_enqueue_job(rt, EMBI_JS_JOB_REACTION, embi_js_from_cell(rr), st->elems[PS_RESULT],
                            UNDEF);
    }
    EMBI_JS_STORE(st, st->elems[PS_HANDLED], EMBI_JS_TRUE);
    return cap != nullptr ? cap->promise : UNDEF;
}

/* ------------------------------------------------------------------ capabilities */

bool embi_js_promise_capability_new(embi_js_runtime *rt, embi_js_value c, embi_js_pcap_t *out) {
    if (!embi_js_is_constructor(c)) {
        (void)throw_type(rt, "Promise capability constructor is not a constructor");
        return false;
    }
    if (c == rt->protos[EMBI_JS_PROTO_PROMISE_CTOR]) {
        /* %Promise% itself: the executor is internal, so it is created directly. */
        embi_js_object *p = embi_js_promise_new(rt);
        embi_js_object *res;
        embi_js_object *rej;
        create_resolving(rt, p, &res, &rej);
        out->promise = embi_js_from_object(p);
        out->resolve = embi_js_from_object(res);
        out->reject = embi_js_from_object(rej);
        return true;
    }
    embi_js_varray *st = embi_js_varray_new(rt, 3);
    EMBI_JS_STORE(st, st->elems[0], embi_js_from_int32(PF_EXECUTOR));
    EMBI_JS_STORE(st, st->elems[1], UNDEF);
    EMBI_JS_STORE(st, st->elems[2], UNDEF);
    embi_js_value executor = embi_js_from_object(pfunc_new(rt, 2, st));
    embi_js_value promise = embi_js_construct(rt, c, 1, &executor, c);
    if (promise == EXC) {
        return false;
    }
    if (!embi_js_is_callable(st->elems[1])) {
        (void)throw_type(rt, "Promise resolve function is not callable");
        return false;
    }
    if (!embi_js_is_callable(st->elems[2])) {
        (void)throw_type(rt, "Promise reject function is not callable");
        return false;
    }
    out->promise = promise;
    out->resolve = st->elems[1];
    out->reject = st->elems[2];
    return true;
}

/* PromiseResolve(C, x). */
embi_js_value embi_js_promise_resolve(embi_js_runtime *rt, embi_js_value c, embi_js_value x) {
    if (embi_js_is_promise(x)) {
        embi_js_value xc =
            embi_js_get(rt, embi_js_as_object(x), embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR));
        if (xc == EXC) {
            return EXC;
        }
        if (embi_js_same_value(xc, c)) {
            return x;
        }
    }
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, c, &cap)) {
        return EXC;
    }
    if (embi_js_call(rt, cap.resolve, UNDEF, 1, &x) == EXC) {
        return EXC;
    }
    return cap.promise;
}

/* ------------------------------------------------------------ resolving and executor calls */

static embi_js_value call_resolving(embi_js_runtime *rt, embi_js_varray *st, embi_js_value arg) {
    embi_js_varray *already = as_varray(st->elems[2]);
    if (already->elems[0] == EMBI_JS_TRUE) {
        return UNDEF;
    }
    EMBI_JS_STORE(already, already->elems[0], EMBI_JS_TRUE);
    embi_js_object *p = embi_js_as_object(st->elems[1]);
    if (embi_js_as_int32(st->elems[3]) == 0) {
        return embi_js_promise_resolve_with(rt, p, arg);
    }
    return embi_js_promise_reject_with(rt, p, arg);
}

static embi_js_value call_executor(embi_js_runtime *rt, embi_js_varray *st, int argc,
                                   const embi_js_value *argv) {
    if (st->elems[1] != UNDEF || st->elems[2] != UNDEF) {
        return throw_type(rt,
                          "Promise executor has already been invoked with non-undefined arguments");
    }
    EMBI_JS_STORE(st, st->elems[1], arg_at(argc, argv, 0));
    EMBI_JS_STORE(st, st->elems[2], arg_at(argc, argv, 1));
    return UNDEF;
}

/* Settled-object for allSettled: {status, value} or {status, reason}. */
static embi_js_value settled_object(embi_js_runtime *rt, bool fulfilled, embi_js_value v) {
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "status"),
                       str_value(rt, fulfilled ? "fulfilled" : "rejected"), ATTR_WEC);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, fulfilled ? "value" : "reason"), v, ATTR_WEC);
    return embi_js_from_object(o);
}

/* The element function of Promise.all, allSettled and any. */
static embi_js_value call_element(embi_js_runtime *rt, embi_js_varray *st, embi_js_value x) {
    embi_js_varray *already = as_varray(st->elems[6]);
    if (already->elems[0] == EMBI_JS_TRUE) {
        return UNDEF;
    }
    EMBI_JS_STORE(already, already->elems[0], EMBI_JS_TRUE);
    int mode = embi_js_as_int32(st->elems[7]);
    embi_js_value v = x;
    if (mode == EL_SETTLED_FULFILLED) {
        v = settled_object(rt, true, x);
    } else if (mode == EL_SETTLED_REJECTED) {
        v = settled_object(rt, false, x);
    }
    embi_js_object *values = embi_js_as_object(st->elems[2]);
    if (embi_js_create_data_property_or_throw(rt, values, st->elems[1], v) == EMBI_JS_THREW) {
        return EXC;
    }
    embi_js_varray *remaining = as_varray(st->elems[3]);
    int32_t left = embi_js_as_int32(remaining->elems[0]) - 1;
    EMBI_JS_STORE(remaining, remaining->elems[0], embi_js_from_int32(left));
    if (left != 0) {
        return UNDEF;
    }
    if (mode == EL_ANY) {
        embi_js_value err =
            embi_js_from_object(embi_js_aggregate_error_new(rt, embi_js_from_object(values)));
        return embi_js_call(rt, st->elems[5], UNDEF, 1, &err);
    }
    embi_js_value arr = st->elems[2];
    return embi_js_call(rt, st->elems[4], UNDEF, 1, &arr);
}

/* thenFinally and catchFinally of Promise.prototype.finally. */
static embi_js_value call_finally(embi_js_runtime *rt, embi_js_varray *st, embi_js_value x) {
    embi_js_value result = embi_js_call(rt, st->elems[1], UNDEF, 0, nullptr);
    if (result == EXC) {
        return EXC;
    }
    embi_js_value p = embi_js_promise_resolve(rt, st->elems[2], result);
    if (p == EXC) {
        return EXC;
    }
    embi_js_varray *helper = embi_js_varray_new(rt, 2);
    bool then_form = embi_js_as_int32(st->elems[3]) == 0;
    EMBI_JS_STORE(helper, helper->elems[0], embi_js_from_int32(then_form ? PF_VALUE_THUNK : PF_THROWER));
    EMBI_JS_STORE(helper, helper->elems[1], x);
    embi_js_value f = embi_js_from_object(pfunc_new(rt, 0, helper));
    return embi_js_invoke(rt, p, embi_js_name_val(rt, EMBI_JS_NAME_THEN), 1, &f);
}

static embi_js_value pfunc_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                int argc, const embi_js_value *argv) {
    (void)this_v;
    embi_js_varray *st = (embi_js_varray *)f->internal;
    embi_js_value arg = arg_at(argc, argv, 0);
    switch ((enum pfunc_kind)embi_js_as_int32(st->elems[0])) {
    case PF_RESOLVING:
        return call_resolving(rt, st, arg);
    case PF_EXECUTOR:
        return call_executor(rt, st, argc, argv);
    case PF_ELEMENT:
        return call_element(rt, st, arg);
    case PF_FINALLY:
        return call_finally(rt, st, arg);
    case PF_VALUE_THUNK:
        return st->elems[1];
    case PF_THROWER:
        return embi_js_throw_value(rt, st->elems[1]);
    }
    return UNDEF;
}

/* ------------------------------------------------------------------ the constructor */

static embi_js_value promise_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic, embi_js_value new_target) {
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (new_target == UNDEF) {
        return throw_type(rt, "Promise constructor cannot be invoked without 'new'");
    }
    embi_js_value executor = arg_at(argc, argv, 0);
    if (!embi_js_is_callable(executor)) {
        return throw_type(rt, "Promise resolver is not a function");
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_PROMISE);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *p = embi_js_object_new_class(rt, proto, &class_promise);
    p->internal = new_state(rt);
    EMBI_JS_WB_STRUCT(p);
    embi_js_object *res;
    embi_js_object *rej;
    create_resolving(rt, p, &res, &rej);
    embi_js_value args[2] = {embi_js_from_object(res), embi_js_from_object(rej)};
    if (embi_js_call(rt, executor, UNDEF, 2, args) == EXC) {
        embi_js_value err = take_exc(rt);
        (void)embi_js_call(rt, args[1], UNDEF, 1, &err);
    }
    return embi_js_from_object(p);
}

/* ------------------------------------------------------------------ then, catch, finally */

static embi_js_value proto_then(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic) {
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_promise(this_v)) {
        return throw_type(rt, "Promise.prototype.then called on a non-promise");
    }
    embi_js_object *p = embi_js_as_object(this_v);
    embi_js_value c = embi_js_species_constructor(rt, p, rt->protos[EMBI_JS_PROTO_PROMISE_CTOR]);
    if (c == EXC) {
        return EXC;
    }
    return then_with(rt, p, c, arg_at(argc, argv, 0), arg_at(argc, argv, 1));
}

static embi_js_value proto_catch(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic) {
    (void)magic;
    embi_js_value args[2] = {UNDEF, arg_at(argc, argv, 0)};
    return embi_js_invoke(r->rt, this_v, embi_js_name_val(r->rt, EMBI_JS_NAME_THEN), 2, args);
}

static embi_js_value proto_finally(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic) {
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_object_value(this_v)) {
        return throw_type(rt, "Promise.prototype.finally called on a non-object");
    }
    embi_js_value c = embi_js_species_constructor(rt, embi_js_as_object(this_v),
                                                  rt->protos[EMBI_JS_PROTO_PROMISE_CTOR]);
    if (c == EXC) {
        return EXC;
    }
    embi_js_value on_finally = arg_at(argc, argv, 0);
    embi_js_value then_f = on_finally;
    embi_js_value catch_f = on_finally;
    if (embi_js_is_callable(on_finally)) {
        embi_js_varray *st_then = embi_js_varray_new(rt, 4);
        EMBI_JS_STORE(st_then, st_then->elems[0], embi_js_from_int32(PF_FINALLY));
        EMBI_JS_STORE(st_then, st_then->elems[1], on_finally);
        EMBI_JS_STORE(st_then, st_then->elems[2], c);
        EMBI_JS_STORE(st_then, st_then->elems[3], embi_js_from_int32(0));
        then_f = embi_js_from_object(pfunc_new(rt, 1, st_then));
        embi_js_varray *st_catch = embi_js_varray_new(rt, 4);
        EMBI_JS_STORE(st_catch, st_catch->elems[0], embi_js_from_int32(PF_FINALLY));
        EMBI_JS_STORE(st_catch, st_catch->elems[1], on_finally);
        EMBI_JS_STORE(st_catch, st_catch->elems[2], c);
        EMBI_JS_STORE(st_catch, st_catch->elems[3], embi_js_from_int32(1));
        catch_f = embi_js_from_object(pfunc_new(rt, 1, st_catch));
    }
    embi_js_value args[2] = {then_f, catch_f};
    return embi_js_invoke(rt, this_v, embi_js_name_val(rt, EMBI_JS_NAME_THEN), 2, args);
}

/* ------------------------------------------------------- resolve, reject, withResolvers, try */

static embi_js_value static_resolve(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic) {
    (void)magic;
    if (!is_object_value(this_v)) {
        return throw_type(r->rt, "Promise.resolve called on a non-object");
    }
    return embi_js_promise_resolve(r->rt, this_v, arg_at(argc, argv, 0));
}

static embi_js_value static_reject(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic) {
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_object_value(this_v)) {
        return throw_type(rt, "Promise.reject called on a non-object");
    }
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, this_v, &cap)) {
        return EXC;
    }
    embi_js_value reason = arg_at(argc, argv, 0);
    if (embi_js_call(rt, cap.reject, UNDEF, 1, &reason) == EXC) {
        return EXC;
    }
    return cap.promise;
}

static embi_js_value static_with_resolvers(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic) {
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, this_v, &cap)) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "promise"), cap.promise, ATTR_WEC);
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_RESOLVE), cap.resolve, ATTR_WEC);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "reject"), cap.reject, ATTR_WEC);
    return embi_js_from_object(o);
}

/* Promise.try(callback, ...args): a throw from the callback rejects the promise. */
static embi_js_value static_try(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic) {
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_object_value(this_v)) {
        return throw_type(rt, "Promise.try called on a non-object");
    }
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, this_v, &cap)) {
        return EXC;
    }
    int nargs = argc > 1 ? argc - 1 : 0;
    const embi_js_value *args = argc > 1 ? argv + 1 : nullptr;
    embi_js_value res = embi_js_call(rt, arg_at(argc, argv, 0), UNDEF, nargs, args);
    embi_js_value r2;
    if (res == EXC) {
        embi_js_value err = take_exc(rt);
        r2 = embi_js_call(rt, cap.reject, UNDEF, 1, &err);
    } else {
        /* A promise made by the same constructor is returned as is (not wrapped). */
        if (embi_js_is_promise(res)) {
            embi_js_value ctor = embi_js_get(rt, embi_js_as_object(res),
                                             embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR));
            if (ctor == EXC) {
                return EXC;
            }
            if (ctor == this_v) {
                return res;
            }
        }
        r2 = embi_js_call(rt, cap.resolve, UNDEF, 1, &res);
    }
    if (r2 == EXC) {
        return EXC;
    }
    return cap.promise;
}

/* ------------------------------------------------------------------ combinators */

/* Rejects cap with the pending exception (IfAbruptRejectPromise). */
static embi_js_value reject_pending(embi_js_runtime *rt, const embi_js_pcap_t *cap) {
    embi_js_value err = take_exc(rt);
    if (embi_js_call(rt, cap->reject, UNDEF, 1, &err) == EXC) {
        return EXC;
    }
    return cap->promise;
}

/* GetPromiseResolve(C). */
static embi_js_value get_promise_resolve(embi_js_runtime *rt, embi_js_value c) {
    embi_js_value f =
        embi_js_get(rt, embi_js_as_object(c), embi_js_name_val(rt, EMBI_JS_NAME_RESOLVE));
    if (f == EXC) {
        return EXC;
    }
    if (!embi_js_is_callable(f)) {
        return throw_type(rt, "Promise resolve is not a function");
    }
    return f;
}

/* An element function for index, in the given mode. Shares already with its siblings. */
static embi_js_value make_element(embi_js_runtime *rt, int index, embi_js_object *values,
                                  embi_js_varray *remaining, const embi_js_pcap_t *cap,
                                  embi_js_varray *already, enum element_mode mode) {
    embi_js_varray *st = embi_js_varray_new(rt, 8);
    EMBI_JS_STORE(st, st->elems[0], embi_js_from_int32(PF_ELEMENT));
    EMBI_JS_STORE(st, st->elems[1], embi_js_from_int32(index));
    EMBI_JS_STORE(st, st->elems[2], embi_js_from_object(values));
    EMBI_JS_STORE(st, st->elems[3], embi_js_from_cell(remaining));
    EMBI_JS_STORE(st, st->elems[4], cap->resolve);
    EMBI_JS_STORE(st, st->elems[5], cap->reject);
    EMBI_JS_STORE(st, st->elems[6], embi_js_from_cell(already));
    EMBI_JS_STORE(st, st->elems[7], embi_js_from_int32((int32_t)mode));
    return embi_js_from_object(pfunc_new(rt, 1, st));
}

/* PerformPromiseAll / AllSettled / Any / Race over the iterator record rec. Returns the
 * capability's promise, or EXC with the exception pending (the caller rejects with it). */
static embi_js_value perform_combinator(embi_js_runtime *rt, enum combinator mode, embi_js_value c,
                                        const embi_js_pcap_t *cap, embi_js_value promise_resolve,
                                        embi_js_value rec) {
    embi_js_object *values = nullptr;
    if (mode != CB_RACE) {
        values = embi_js_array_create_default(rt, 0.0);
    }
    embi_js_varray *remaining = embi_js_varray_new(rt, 1);
    EMBI_JS_STORE(remaining, remaining->elems[0], embi_js_from_int32(1));
    uint32_t index = 0;
    for (;;) {
        embi_js_value next = UNDEF;
        int st = embi_js_ir_next(rt, rec, &next);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            if (mode == CB_RACE) {
                return cap->promise;
            }
            int32_t left = embi_js_as_int32(remaining->elems[0]) - 1;
            EMBI_JS_STORE(remaining, remaining->elems[0], embi_js_from_int32(left));
            if (left == 0) {
                embi_js_value r2;
                if (mode == CB_ANY) {
                    embi_js_value err = embi_js_from_object(
                        embi_js_aggregate_error_new(rt, embi_js_from_object(values)));
                    r2 = embi_js_call(rt, cap->reject, UNDEF, 1, &err);
                } else {
                    embi_js_value arr = embi_js_from_object(values);
                    r2 = embi_js_call(rt, cap->resolve, UNDEF, 1, &arr);
                }
                if (r2 == EXC) {
                    return EXC;
                }
            }
            return cap->promise;
        }
        if (mode == CB_RACE) {
            embi_js_value np = embi_js_call(rt, promise_resolve, c, 1, &next);
            if (np == EXC) {
                return EXC;
            }
            embi_js_value args[2] = {cap->resolve, cap->reject};
            if (embi_js_invoke(rt, np, embi_js_name_val(rt, EMBI_JS_NAME_THEN), 2, args) == EXC) {
                return EXC;
            }
            continue;
        }
        if (embi_js_create_data_property_or_throw(rt, values, embi_js_from_int32((int32_t)index),
                                                  UNDEF) == EMBI_JS_THREW) {
            return EXC;
        }
        embi_js_value np = embi_js_call(rt, promise_resolve, c, 1, &next);
        if (np == EXC) {
            return EXC;
        }
        embi_js_varray *already = embi_js_varray_new(rt, 1);
        EMBI_JS_STORE(already, already->elems[0], EMBI_JS_FALSE);
        embi_js_value on_f;
        embi_js_value on_r;
        switch (mode) {
        case CB_ALL:
            on_f = make_element(rt, (int)index, values, remaining, cap, already, EL_ALL);
            on_r = cap->reject;
            break;
        case CB_ALL_SETTLED:
            on_f =
                make_element(rt, (int)index, values, remaining, cap, already, EL_SETTLED_FULFILLED);
            on_r =
                make_element(rt, (int)index, values, remaining, cap, already, EL_SETTLED_REJECTED);
            break;
        case CB_ANY:
        default:
            on_f = cap->resolve;
            on_r = make_element(rt, (int)index, values, remaining, cap, already, EL_ANY);
            break;
        }
        int32_t left = embi_js_as_int32(remaining->elems[0]) + 1;
        EMBI_JS_STORE(remaining, remaining->elems[0], embi_js_from_int32(left));
        embi_js_value args[2] = {on_f, on_r};
        if (embi_js_invoke(rt, np, embi_js_name_val(rt, EMBI_JS_NAME_THEN), 2, args) == EXC) {
            return EXC;
        }
        index++;
    }
}

/* Promise.all, allSettled, any and race (magic is the combinator). */
static embi_js_value static_combinator(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic) {
    embi_js_runtime *rt = r->rt;
    if (!is_object_value(this_v)) {
        return throw_type(rt, "Promise combinator called on a non-object");
    }
    embi_js_pcap_t cap;
    if (!embi_js_promise_capability_new(rt, this_v, &cap)) {
        return EXC;
    }
    embi_js_value promise_resolve = get_promise_resolve(rt, this_v);
    if (promise_resolve == EXC) {
        return reject_pending(rt, &cap);
    }
    embi_js_value rec = embi_js_ir_open(rt, arg_at(argc, argv, 0));
    if (rec == EXC) {
        return reject_pending(rt, &cap);
    }
    embi_js_value res =
        perform_combinator(rt, (enum combinator)magic, this_v, &cap, promise_resolve, rec);
    if (res == EXC) {
        embi_js_value err = take_exc(rt);
        embi_js_ir_close_throw(rt, rec);
        if (embi_js_call(rt, cap.reject, UNDEF, 1, &err) == EXC) {
            return EXC;
        }
        return cap.promise;
    }
    return res;
}

/* ------------------------------------------------------------------ accessors and setup */

static embi_js_value species_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic) {
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

void embi_js_init_builtins_promise(embi_js_realm *r) {
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    rt->protos[EMBI_JS_PROTO_PROMISE] = embi_js_from_object(proto);
    embi_js_object *ctor = embi_js_native_constructor_new(r, "Promise", 1, promise_ctor, 0);
    rt->protos[EMBI_JS_PROTO_PROMISE_CTOR] = embi_js_from_object(ctor);

    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_method(r, ctor, "all", static_combinator, 1, CB_ALL);
    embi_js_define_method(r, ctor, "allSettled", static_combinator, 1, CB_ALL_SETTLED);
    embi_js_define_method(r, ctor, "any", static_combinator, 1, CB_ANY);
    embi_js_define_method(r, ctor, "race", static_combinator, 1, CB_RACE);
    embi_js_define_method(r, ctor, "reject", static_reject, 1, 0);
    embi_js_define_method(r, ctor, "resolve", static_resolve, 1, 0);
    embi_js_define_method(r, ctor, "withResolvers", static_with_resolvers, 0, 0);
    embi_js_define_method(r, ctor, "try", static_try, 1, 0);
    embi_js_object *getter =
        embi_js_native_function_new(r, "get [Symbol.species]", 0, species_get, 0);
    embi_js_define_accessor_raw(rt, ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES),
                                embi_js_from_object(getter), UNDEF, ATTR_C);

    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_object *then_fn = embi_js_native_function_new(r, "then", 2, proto_then, 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_THEN),
                       embi_js_from_object(then_fn), ATTR_WC);
    rt->protos[EMBI_JS_PROTO_PROMISE_THEN] = embi_js_from_object(then_fn);
    embi_js_define_method(r, proto, "catch", proto_catch, 1, 0);
    embi_js_define_method(r, proto, "finally", proto_finally, 1, 0);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       str_value(rt, "Promise"), ATTR_C);

    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Promise"), embi_js_from_object(ctor),
                       ATTR_WC);
}
