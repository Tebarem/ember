/*
 * %IteratorPrototype%, the Iterator constructor and the ES2025 iterator helpers, the iterator
 * helper and wrapper prototypes, and the generator prototypes (%GeneratorFunction.prototype%,
 * %GeneratorPrototype%) of J2c.
 *
 * The generator objects and their frames are in interp.c (see the design note there). This file
 * holds the prototypes and the methods that drive them. The array and string iterator prototypes
 * are in builtins_array.c and builtins_string.c, and they inherit from %IteratorPrototype%, which
 * this file creates first (the realm initializes it before the array and string builtins).
 *
 * Iterator helper objects keep their state in a varray: [iterator, next, callback, counter,
 * remaining, inner iterator, inner next, state, kind]. The helper steps are written as explicit
 * state machines, so no generator frame is needed for them.
 *
 * Known gap: the Iterator constructor cannot tell `new Iterator()` from a subclass construction
 * through the native convention (no new.target yet, J2d changes it). It throws for an object whose
 * prototype is %IteratorPrototype%, which is the direct case.
 */
#include "js/builtins.h"
#include "js/compiler.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/value.h"

#include <SDL3/SDL_stdinc.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define ARG(i) ((i) < argc ? argv[i] : EMBI_JS_UNDEFINED)
#define UNDEF EMBI_JS_UNDEFINED
#define EXC EMBI_JS_EXCEPTION

/* Iterator helper state slots and kinds. */
enum {
    HS_ITER,
    HS_NEXT,
    HS_FN,
    HS_COUNT,
    HS_REM,
    HS_INNER,
    HS_INNER_NEXT,
    HS_STATE,
    HS_KIND,
    HS_N
};
enum { ST_START, ST_YIELD, ST_RUNNING, ST_DONE };
enum { HK_MAP, HK_FILTER, HK_TAKE, HK_DROP, HK_FLAT, HK_CONCAT };
/* Wrapper slots (Iterator.from of an iterator that does not inherit from %Iterator%). */
enum { WS_ITER, WS_NEXT, WS_N };
/* Eager methods. */
enum { EM_REDUCE, EM_TO_ARRAY, EM_FOR_EACH, EM_SOME, EM_EVERY, EM_FIND };

static double num_of(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

static embi_js_value text_val(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

static embi_js_value iter_result(embi_js_runtime *rt, embi_js_value v, bool done)
{
    return embi_js_iter_result_new(rt, v, done);
}

/* ------------------------------------------------------------------ the protocol on values */

/* IteratorStep (want false) or IteratorStepValue (want true) on an iterator and its next method:
 * 1 with the value in *out, 0 when done, -1 when it threw. */
static int step(embi_js_runtime *rt, embi_js_value iter, embi_js_value next, bool want,
                embi_js_value *out)
{
    *out = UNDEF;
    embi_js_value res = embi_js_call(rt, next, iter, 0, nullptr);
    if (res == EXC) {
        return -1;
    }
    if (!embi_js_is_object(res)) {
        (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
        return -1;
    }
    embi_js_value d = embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_DONE));
    if (d == EXC) {
        return -1;
    }
    if (embi_js_to_boolean(d)) {
        return 0;
    }
    if (want) {
        embi_js_value v = embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_VALUE));
        if (v == EXC) {
            return -1;
        }
        *out = v;
    }
    return 1;
}

/* IteratorClose on a normal completion: the errors of return propagate. False with the error
 * pending. */
static bool close_normal(embi_js_runtime *rt, embi_js_value iter)
{
    embi_js_value m = embi_js_get_method(rt, iter, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (m == EXC) {
        return false;
    }
    if (m == UNDEF) {
        return true;
    }
    embi_js_value res = embi_js_call(rt, m, iter, 0, nullptr);
    if (res == EXC) {
        return false;
    }
    if (!embi_js_is_object(res)) {
        (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
        return false;
    }
    return true;
}

/* IteratorClose on a throw completion: the errors of return are dropped and the pending
 * exception stays pending. */
static void close_throw(embi_js_runtime *rt, embi_js_value iter)
{
    embi_js_value saved = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    embi_js_value m = embi_js_get_method(rt, iter, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (m != EXC && m != UNDEF) {
        (void)embi_js_call(rt, m, iter, 0, nullptr);
    }
    embi_js_exception_clear(rt);
    embi_js_exception_set(rt, saved);
}

/* GetIteratorDirect: next is read from the object. False with the error pending. */
static bool direct_next(embi_js_runtime *rt, embi_js_value obj, embi_js_value *next)
{
    *next = embi_js_get(rt, embi_js_as_object(obj), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
    return *next != EXC;
}

/* GetIteratorFlattenable (ES2025 27.1.2.1). With strings set, a string is iterated; otherwise
 * primitives are rejected. False with the error pending. */
static bool flattenable(embi_js_runtime *rt, embi_js_value v, bool strings, embi_js_value *iter,
                        embi_js_value *next)
{
    if (!embi_js_is_object(v) && !(strings && embi_js_is_string(v))) {
        (void)embi_js_throw_type_error(rt, "value is not an iterator or iterable object");
        return false;
    }
    embi_js_value m = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (m == EXC) {
        return false;
    }
    embi_js_value it = v;
    if (m != UNDEF) {
        it = embi_js_call(rt, m, v, 0, nullptr);
        if (it == EXC) {
            return false;
        }
    }
    if (!embi_js_is_object(it)) {
        (void)embi_js_throw_type_error(rt, "iterator is not an object");
        return false;
    }
    *next = embi_js_get(rt, embi_js_as_object(it), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
    if (*next == EXC) {
        return false;
    }
    *iter = it;
    return true;
}

/* OrdinaryHasInstance against %IteratorPrototype%: whether proto is on o's prototype chain. */
static bool chain_has(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto)
{
    for (;;) {
        embi_js_value p = UNDEF;
        if (embi_js_obj_get_prototype_of(rt, o, &p) == EMBI_JS_THREW) {
            return false;
        }
        if (!embi_js_is_object(p)) {
            return false;
        }
        if (p == proto) {
            return true;
        }
        o = embi_js_as_object(p);
    }
}

/* ------------------------------------------------------------------ helper and wrapper objects */

static void state_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, &((embi_js_varray *)o->internal)->hdr);
    }
}

static const embi_js_class class_helper = {.name = "Iterator Helper", .trace = state_trace};
static const embi_js_class class_wrap = {.name = "Object", .trace = state_trace};

static embi_js_varray *state_of(embi_js_value v, const embi_js_class *cls)
{
    if (!embi_js_is_object(v) || embi_js_as_object(v)->klass != cls) {
        return nullptr;
    }
    return embi_js_as_object(v)->internal;
}

static embi_js_value helper_new(embi_js_realm *r, embi_js_value iter, embi_js_value next,
                                embi_js_value fn, int kind, embi_js_value rem)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *st = embi_js_varray_new(rt, HS_N);
    EMBI_JS_STORE(st, st->elems[HS_ITER], iter);
    EMBI_JS_STORE(st, st->elems[HS_NEXT], next);
    EMBI_JS_STORE(st, st->elems[HS_FN], fn);
    EMBI_JS_STORE(st, st->elems[HS_COUNT], embi_js_number(0.0));
    EMBI_JS_STORE(st, st->elems[HS_REM], rem);
    EMBI_JS_STORE(st, st->elems[HS_INNER], UNDEF);
    EMBI_JS_STORE(st, st->elems[HS_INNER_NEXT], UNDEF);
    EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_START));
    EMBI_JS_STORE(st, st->elems[HS_KIND], embi_js_from_int32(kind));
    embi_js_object *o = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_ITER_HELPER],
                                                 &class_helper);
    o->internal = st;
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

/* One step of a helper: 1 with the value in *out, 0 when done, -1 when it threw. Each kind
 * follows its ES2025 abstract operation (the callbacks close the underlying iterator when they
 * throw). */
static int helper_step(embi_js_runtime *rt, embi_js_varray *st, embi_js_value *out)
{
    embi_js_value iter = st->elems[HS_ITER];
    embi_js_value next = st->elems[HS_NEXT];
    embi_js_value fn = st->elems[HS_FN];
    int kind = embi_js_as_int32(st->elems[HS_KIND]);
    embi_js_value v = UNDEF;
    switch (kind) {
    case HK_MAP:
    case HK_FILTER:
        for (;;) {
            int c = step(rt, iter, next, true, &v);
            if (c <= 0) {
                return c;
            }
            embi_js_value args[2] = {v, st->elems[HS_COUNT]};
            embi_js_value res = embi_js_call(rt, fn, UNDEF, 2, args);
            if (res == EXC) {
                close_throw(rt, iter);
                return -1;
            }
            EMBI_JS_STORE(st, st->elems[HS_COUNT], embi_js_number(num_of(args[1]) + 1.0));
            EMBI_JS_WB(st, st->elems[HS_COUNT]);
            if (kind == HK_MAP) {
                *out = res;
                return 1;
            }
            if (embi_js_to_boolean(res)) {
                *out = v;
                return 1;
            }
        }
    case HK_TAKE: {
        double rem = num_of(st->elems[HS_REM]);
        if (rem == 0) {
            return close_normal(rt, iter) ? 0 : -1;
        }
        if (rem != INFINITY) {
            EMBI_JS_STORE(st, st->elems[HS_REM], embi_js_number(rem - 1.0));
            EMBI_JS_WB(st, st->elems[HS_REM]);
        }
        return step(rt, iter, next, true, out);
    }
    case HK_DROP: {
        double rem = num_of(st->elems[HS_REM]);
        while (rem > 0) {
            if (rem != INFINITY) {
                rem -= 1.0;
                EMBI_JS_STORE(st, st->elems[HS_REM], embi_js_number(rem));
                EMBI_JS_WB(st, st->elems[HS_REM]);
            }
            int c = step(rt, iter, next, false, &v);
            if (c <= 0) {
                return c;
            }
        }
        return step(rt, iter, next, true, out);
    }
    case HK_FLAT:
        for (;;) {
            if (embi_js_is_object(st->elems[HS_INNER])) {
                int c = step(rt, st->elems[HS_INNER], st->elems[HS_INNER_NEXT], true, out);
                if (c < 0) {
                    close_throw(rt, iter);
                    return -1;
                }
                if (c == 1) {
                    return 1;
                }
                EMBI_JS_STORE(st, st->elems[HS_INNER], UNDEF);
                EMBI_JS_STORE(st, st->elems[HS_INNER_NEXT], UNDEF);
                continue;
            }
            int c = step(rt, iter, next, true, &v);
            if (c <= 0) {
                return c;
            }
            embi_js_value args[2] = {v, st->elems[HS_COUNT]};
            embi_js_value m = embi_js_call(rt, fn, UNDEF, 2, args);
            if (m == EXC) {
                close_throw(rt, iter);
                return -1;
            }
            EMBI_JS_STORE(st, st->elems[HS_COUNT], embi_js_number(num_of(args[1]) + 1.0));
            EMBI_JS_WB(st, st->elems[HS_COUNT]);
            embi_js_value ii = UNDEF;
            embi_js_value inext = UNDEF;
            if (!flattenable(rt, m, false, &ii, &inext)) {
                close_throw(rt, iter);
                return -1;
            }
            EMBI_JS_STORE(st, st->elems[HS_INNER], ii);
            EMBI_JS_WB(st, ii);
            EMBI_JS_STORE(st, st->elems[HS_INNER_NEXT], inext);
            EMBI_JS_WB(st, inext);
        }
    case HK_CONCAT:
        /* Iterator.concat (iterator-sequencing). HS_FN is the list [item, @@iterator method, ...]
         * validated by the call, HS_REM its length and HS_COUNT the next position. The iterables
         * are opened one at a time, when the previous one is exhausted. The inner iterator is the
         * only one that a return closes (HS_ITER is unused). An error from the inner iterator is
         * not closed. */
        for (;;) {
            if (embi_js_is_object(st->elems[HS_INNER])) {
                int c = step(rt, st->elems[HS_INNER], st->elems[HS_INNER_NEXT], true, out);
                if (c != 0) {
                    return c;
                }
                EMBI_JS_STORE(st, st->elems[HS_INNER], UNDEF);
                EMBI_JS_STORE(st, st->elems[HS_INNER_NEXT], UNDEF);
                continue;
            }
            embi_js_varray *list = (embi_js_varray *)embi_js_as_cell(st->elems[HS_FN]);
            uint32_t pos = (uint32_t)num_of(st->elems[HS_COUNT]);
            uint32_t len = (uint32_t)num_of(st->elems[HS_REM]);
            if (pos >= len) {
                return 0;
            }
            EMBI_JS_STORE(st, st->elems[HS_COUNT], embi_js_number((double)(pos + 2u)));
            EMBI_JS_WB(st, st->elems[HS_COUNT]);
            embi_js_value item = list->elems[pos];
            embi_js_value m = list->elems[pos + 1u];
            embi_js_value it = embi_js_call(rt, m, item, 0, nullptr);
            if (it == EXC) {
                return -1;
            }
            if (!embi_js_is_object(it)) {
                (void)embi_js_throw_type_error(rt, "iterator is not an object");
                return -1;
            }
            embi_js_value inext = UNDEF;
            if (!direct_next(rt, it, &inext)) {
                return -1;
            }
            EMBI_JS_STORE(st, st->elems[HS_INNER], it);
            EMBI_JS_WB(st, it);
            EMBI_JS_STORE(st, st->elems[HS_INNER_NEXT], inext);
            EMBI_JS_WB(st, inext);
        }
    default:
        return -1;
    }
}

/* %IteratorHelperPrototype%.next (magic 0) and .return (magic 1). */
static embi_js_value helper_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_varray *st = state_of(this_v, &class_helper);
    if (!st) {
        return embi_js_throw_type_error(rt, "Iterator Helper method called on incompatible receiver");
    }
    int state = embi_js_as_int32(st->elems[HS_STATE]);
    if (state == ST_RUNNING) {
        return embi_js_throw_type_error(rt, "Generator is already running");
    }
    if (state == ST_DONE) {
        return iter_result(rt, UNDEF, true);
    }
    if (magic == 1) {
        /* The closing runs with the helper executing: a return or next that the underlying
         * iterator makes back into this helper throws a TypeError, as for a running generator. */
        EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_RUNNING));
        bool ok = true;
        if (embi_js_is_object(st->elems[HS_INNER])) {
            if (!close_normal(rt, st->elems[HS_INNER])) {
                if (embi_js_is_object(st->elems[HS_ITER])) {
                    close_throw(rt, st->elems[HS_ITER]);
                }
                ok = false;
            }
        }
        /* Iterator.concat has no outer iterator (HS_ITER is undefined). */
        if (ok && embi_js_is_object(st->elems[HS_ITER]) && !close_normal(rt, st->elems[HS_ITER])) {
            ok = false;
        }
        EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_DONE));
        return ok ? iter_result(rt, UNDEF, true) : EXC;
    }
    EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_RUNNING));
    embi_js_value out = UNDEF;
    int c = helper_step(rt, st, &out);
    if (c < 0) {
        EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_DONE));
        return EXC;
    }
    if (c == 0) {
        EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_DONE));
        return iter_result(rt, UNDEF, true);
    }
    EMBI_JS_STORE(st, st->elems[HS_STATE], embi_js_from_int32(ST_YIELD));
    return iter_result(rt, out, false);
}

/* %WrapForValidIteratorPrototype%.next (magic 0) and .return (magic 1). */
static embi_js_value wrap_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_varray *st = state_of(this_v, &class_wrap);
    if (!st) {
        return embi_js_throw_type_error(rt, "Wrapper method called on incompatible receiver");
    }
    embi_js_value iter = st->elems[WS_ITER];
    if (magic == 0) {
        return embi_js_call(rt, st->elems[WS_NEXT], iter, 0, nullptr);
    }
    embi_js_value m = embi_js_get_method(rt, iter, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (m == EXC) {
        return EXC;
    }
    if (m == UNDEF) {
        return iter_result(rt, UNDEF, true);
    }
    return embi_js_call(rt, m, iter, 0, nullptr);
}

/* ------------------------------------------------------------------ Iterator.prototype methods */

/* map, filter, take, drop and flatMap (magic is the helper kind): validate the arguments, then
 * build the helper. A bad argument closes this iterator (ES2025: the close precedes the throw). */
static embi_js_value lazy_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Iterator helper method called on non-object");
    }
    embi_js_value fn = ARG(0);
    embi_js_value rem = UNDEF;
    if (magic == HK_TAKE || magic == HK_DROP) {
        double lim = 0;
        if (!embi_js_to_number(rt, fn, &lim)) {
            close_throw(rt, this_v);
            return EXC;
        }
        /* NaN, a finite limit above 2^53 - 1, and a negative limit are RangeErrors (ES2025). An
         * infinite limit is allowed. */
        if (isnan(lim) || (isfinite(lim) && lim > 9007199254740991.0)) {
            (void)embi_js_throw_range_error(rt, "must be positive");
            close_throw(rt, this_v);
            return EXC;
        }
        lim = trunc(lim);
        if (lim < 0) {
            (void)embi_js_throw_range_error(rt, "must be positive");
            close_throw(rt, this_v);
            return EXC;
        }
        rem = embi_js_number(lim);
        fn = UNDEF;
    } else if (!embi_js_is_callable(fn)) {
        (void)embi_js_throw_type_error(rt, "Iterator helper callback is not a function");
        close_throw(rt, this_v);
        return EXC;
    }
    embi_js_value next = UNDEF;
    if (!direct_next(rt, this_v, &next)) {
        return EXC;
    }
    return helper_new(r, this_v, next, fn, magic, rem);
}

/* reduce, toArray, forEach, some, every and find (magic is the EM_ kind). */
static embi_js_value eager_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Iterator method called on non-object");
    }
    embi_js_value fn = ARG(0);
    if (magic != EM_TO_ARRAY && !embi_js_is_callable(fn)) {
        (void)embi_js_throw_type_error(rt, "Iterator method callback is not a function");
        close_throw(rt, this_v);
        return EXC;
    }
    embi_js_value next = UNDEF;
    if (!direct_next(rt, this_v, &next)) {
        return EXC;
    }
    embi_js_value v = UNDEF;
    double counter = 0;
    if (magic == EM_TO_ARRAY) {
        embi_js_object *arr = embi_js_array_create_default(rt, 0.0);
        embi_js_value av = embi_js_from_object(arr);
        for (uint32_t n = 0;; n++) {
            int c = step(rt, this_v, next, true, &v);
            if (c < 0) {
                return EXC;
            }
            if (c == 0) {
                return av;
            }
            if (embi_js_create_data_property(rt, arr, embi_js_key_from_index(n), v) ==
                EMBI_JS_THREW) {
                close_throw(rt, this_v);
                return EXC;
            }
        }
    }
    if (magic == EM_REDUCE) {
        embi_js_value acc = UNDEF;
        if (argc >= 2) {
            acc = argv[1];
        } else {
            int c = step(rt, this_v, next, true, &acc);
            if (c < 0) {
                return EXC;
            }
            if (c == 0) {
                return embi_js_throw_type_error(rt, "Reduce of empty iterator with no initial value");
            }
            counter = 1;
        }
        for (;;) {
            int c = step(rt, this_v, next, true, &v);
            if (c < 0) {
                return EXC;
            }
            if (c == 0) {
                return acc;
            }
            embi_js_value args[3] = {acc, v, embi_js_number(counter)};
            acc = embi_js_call(rt, fn, UNDEF, 3, args);
            if (acc == EXC) {
                close_throw(rt, this_v);
                return EXC;
            }
            counter += 1.0;
        }
    }
    for (;;) {
        int c = step(rt, this_v, next, true, &v);
        if (c < 0) {
            return EXC;
        }
        if (c == 0) {
            switch (magic) {
            case EM_SOME:
                return EMBI_JS_FALSE;
            case EM_EVERY:
                return EMBI_JS_TRUE;
            default:
                return UNDEF;
            }
        }
        embi_js_value args[2] = {v, embi_js_number(counter)};
        embi_js_value res = embi_js_call(rt, fn, UNDEF, 2, args);
        if (res == EXC) {
            close_throw(rt, this_v);
            return EXC;
        }
        counter += 1.0;
        bool t = embi_js_to_boolean(res);
        if (magic == EM_FOR_EACH) {
            continue;
        }
        if ((magic == EM_SOME && t) || (magic == EM_EVERY && !t) || (magic == EM_FIND && t)) {
            if (!close_normal(rt, this_v)) {
                return EXC;
            }
            if (magic == EM_FIND) {
                return v;
            }
            return magic == EM_SOME ? EMBI_JS_TRUE : EMBI_JS_FALSE;
        }
    }
}

/* Iterator.concat(...items) (iterator-sequencing). Every item is checked before any is opened: it
 * must be an object with a callable @@iterator method, which is read once here. */
static embi_js_value iterator_concat(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_varray *list = embi_js_varray_new(rt, (uint32_t)(2 * argc));
    for (int i = 0; i < argc; i++) {
        embi_js_value item = argv[i];
        if (!embi_js_is_object(item)) {
            return embi_js_throw_type_error(rt, "Iterator.concat: argument is not an object");
        }
        embi_js_value m = embi_js_get_method(rt, item, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
        if (m == EXC) {
            return EXC;
        }
        if (!embi_js_is_callable(m)) {
            return embi_js_throw_type_error(rt, "Iterator.concat: @@iterator is not a function");
        }
        EMBI_JS_STORE(list, list->elems[2 * i], item);
        EMBI_JS_STORE(list, list->elems[2 * i + 1u], m);
    }
    embi_js_value h = helper_new(r, UNDEF, UNDEF, embi_js_from_cell(list), HK_CONCAT,
                                 embi_js_number((double)(2 * argc)));
    return h;
}

static embi_js_value iter_self(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

/* The Iterator constructor is abstract: it can be subclassed, but not constructed directly. */
static embi_js_value iterator_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic,
                                   embi_js_value new_target)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(new_target)) {
        return embi_js_throw_type_error(rt, "Constructor Iterator requires 'new'");
    }
    if (new_target == rt->protos[EMBI_JS_PROTO_ITERATOR_CTOR]) {
        return embi_js_throw_type_error(rt, "Abstract class Iterator not directly constructable");
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_ITERATOR);
    if (proto == EXC) {
        return EXC;
    }
    return embi_js_from_object(embi_js_object_new(rt, proto));
}

/* Iterator.from (ES2025 27.1.3.2.1). */
static embi_js_value iterator_from(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value iter = UNDEF;
    embi_js_value next = UNDEF;
    if (!flattenable(rt, ARG(0), true, &iter, &next)) {
        return EXC;
    }
    if (chain_has(rt, embi_js_as_object(iter), rt->protos[EMBI_JS_PROTO_ITERATOR])) {
        return iter;
    }
    embi_js_varray *st = embi_js_varray_new(rt, WS_N);
    EMBI_JS_STORE(st, st->elems[WS_ITER], iter);
    EMBI_JS_WB(st, iter);
    EMBI_JS_STORE(st, st->elems[WS_NEXT], next);
    EMBI_JS_WB(st, next);
    embi_js_object *w = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_WRAP_ITER], &class_wrap);
    w->internal = st;
    EMBI_JS_WB_STRUCT(w);
    return embi_js_from_object(w);
}

/* The constructor and @@toStringTag accessors of Iterator.prototype: the getters report the
 * intrinsic values, and the setters (SetterThatIgnoresPrototypeProperties) define an own property
 * on any object but the prototype itself. magic 0 is constructor, 1 is @@toStringTag. */
static embi_js_value proto_tag_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    if (magic == 0) {
        return rt->protos[EMBI_JS_PROTO_ITERATOR_CTOR];
    }
    return text_val(rt, "Iterator");
}

static embi_js_value proto_tag_set(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value v = ARG(0);
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Iterator.prototype setter called on non-object");
    }
    if (this_v == rt->protos[EMBI_JS_PROTO_ITERATOR]) {
        return embi_js_throw_type_error(rt, "Cannot assign to read only property of Iterator.prototype");
    }
    embi_js_object *o = embi_js_as_object(this_v);
    embi_js_key k = magic == 0 ? embi_js_key_c(rt, "constructor")
                               : embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG);
    int own = embi_js_obj_has_own_property(rt, o, k);
    if (own == EMBI_JS_THREW) {
        return EXC;
    }
    if (own == EMBI_JS_NO) {
        if (embi_js_create_data_property_or_throw(rt, o, k, v) == EMBI_JS_THREW) {
            return EXC;
        }
        return UNDEF;
    }
    if (embi_js_put(rt, o, k, v, true) == EMBI_JS_THREW) {
        return EXC;
    }
    return UNDEF;
}

/* %GeneratorPrototype%.next (0), .throw (2) and .return (1) on a generator object. */
static embi_js_value gen_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_generator(this_v)) {
        return embi_js_throw_type_error(rt, "Generator method called on incompatible receiver");
    }
    return embi_js_generator_resume(rt, embi_js_as_object(this_v), magic, ARG(0));
}

static void define_tag(embi_js_runtime *rt, embi_js_object *o, const char *tag)
{
    embi_js_define_raw(rt, o, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), text_val(rt, tag),
                       ATTR_C);
}

/* ------------------------------------------------------------------ GeneratorFunction */

static void sb_text(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void sb_bytes(embi_sb *sb, const embi_sb *piece)
{
    embi_sb_append(sb, (embi_str){piece->data, piece->len});
}

/* Compile-only check that "pre piece post" is exactly one parenthesised generator function
 * expression, so injected text can neither close the wrapper early nor add statements (the same
 * check Function makes; see builtins_function.c). */
static bool gen_piece_ok(embi_js_realm *r, const char *pre, const embi_sb *piece, const char *post)
{
    embi_sb sb = {nullptr, 0, 0};
    sb_text(&sb, pre);
    sb_bytes(&sb, piece);
    sb_text(&sb, post);
    embi_js_value v = embi_js_compile_function_only(r, (const uint8_t *)sb.data, sb.len);
    embi_sb_free(&sb);
    return v != EXC;
}

/* The UTF-8 of a ToString'd argument. False with the conversion's error pending. */
static bool gen_arg_text(embi_js_runtime *rt, embi_js_value v, embi_sb *sb)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_js_str_append_utf8(s, sb);
    return true;
}

/* The dynamic function constructors (GeneratorFunction, AsyncFunction, AsyncGeneratorFunction): the
 * Function constructor's dynamic path with the keyword kw ("function*", "async function" or
 * "async function*"; J3b). The parameters and the body are checked as pieces, and the whole text
 * as one function expression, before it is evaluated (which only creates the closure). */
static embi_js_value dyn_fn_ctor(embi_js_realm *r, int argc, const embi_js_value *argv,
                                 embi_js_value new_target, const char *kw, enum embi_js_proto proto)
{
    embi_js_runtime *rt = r->rt;
    embi_sb params = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    embi_sb src = {nullptr, 0, 0};
    embi_js_value result = EXC;
    char head[96];
    char body_pre[112];
    SDL_snprintf(head, sizeof head, "(%s anonymous(", kw);
    SDL_snprintf(body_pre, sizeof body_pre, "%s\n) {\n", head);

    for (int i = 0; i < argc - 1; i++) {
        if (i > 0) {
            embi_sb_append_char(&params, ',');
        }
        if (!gen_arg_text(rt, argv[i], &params)) {
            goto done;
        }
    }
    if (argc > 0 && !gen_arg_text(rt, argv[argc - 1], &body)) {
        goto done;
    }
    if (!gen_piece_ok(r, head, &params, "\n) {\n})") || !gen_piece_ok(r, body_pre, &body, "\n})")) {
        goto done;
    }
    sb_text(&src, head);
    sb_bytes(&src, &params);
    sb_text(&src, "\n) {\n");
    sb_bytes(&src, &body);
    sb_text(&src, "\n})");
    if (!gen_piece_ok(r, "", &src, "")) {
        goto done;
    }
    embi_js_value f = embi_js_eval(r, src.data, src.len, "anonymous", EMBI_JS_EVAL_DYNFN);
    if (f == EXC) {
        goto done;
    }
    embi_js_define_raw(rt, embi_js_as_object(f), embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "anonymous", 9)), ATTR_C);
    embi_js_value gp = embi_js_proto_from_ctor(rt, new_target, proto);
    if (gp == EXC || embi_js_obj_set_prototype_of(rt, embi_js_as_object(f), gp) == EMBI_JS_THREW) {
        goto done;
    }
    result = f;

done:
    embi_sb_free(&params);
    embi_sb_free(&body);
    embi_sb_free(&src);
    return result;
}

/* GeneratorFunction(p1, ..., body) (ES2025 27.3.1.1). */
static embi_js_value generator_function_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic,
                                             embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    return dyn_fn_ctor(r, argc, argv, new_target, "function*", EMBI_JS_PROTO_GENERATOR_FUNCTION);
}

/* AsyncFunction(p1, ..., body) (ECMA-262 27.7.1.1). */
static embi_js_value async_function_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic,
                                         embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    return dyn_fn_ctor(r, argc, argv, new_target, "async function", EMBI_JS_PROTO_ASYNC_FUNCTION);
}

/* AsyncGeneratorFunction(p1, ..., body) (ECMA-262 27.4.1.1). */
static embi_js_value async_generator_function_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                                   const embi_js_value *argv, int magic,
                                                   embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    return dyn_fn_ctor(r, argc, argv, new_target, "async function*",
                       EMBI_JS_PROTO_ASYNC_GENERATOR_FUNCTION);
}

/* ------------------------------------------------------------------ async generators and
 * AsyncFromSyncIterator (J3b) */

/* AsyncGenerator.prototype.next (0), throw (1) and return (2): the request machinery is in
 * interp.c (embi_js_async_gen_request). */
static embi_js_value agen_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    return embi_js_async_gen_request(r->rt, this_v, magic, ARG(0));
}

/* %AsyncFromSyncIteratorPrototype%: the wrapper object's internal slot is its sync iterator record
 * (a varray [iterator, next method, flags]). */
static void afs_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal != nullptr) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_async_from_sync = {
    .name = "AsyncFromSyncIterator",
    .trace = afs_trace,
};

enum afs_kind {
    AFS_UNWRAP,     /* the value of a settled result: {value, done} */
    AFS_CLOSE_THROW, /* a rejected value: the sync iterator is closed, then the reason rethrown */
};

static embi_js_value afs_fn_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)this_v;
    const embi_js_varray *st = (const embi_js_varray *)f->internal;
    int kind = embi_js_as_int32(st->elems[0]);
    embi_js_value v = argc > 0 ? argv[0] : UNDEF;
    if (kind == AFS_UNWRAP) {
        return embi_js_iter_result_new(rt, v, embi_js_as_int32(st->elems[1]) != 0);
    }
    embi_js_ir_close_throw(rt, st->elems[2]);
    return embi_js_throw_value(rt, v);
}

static void afs_fn_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal != nullptr) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_afs_fn = {
    .name = "Function",
    .call = afs_fn_call,
    .trace = afs_fn_trace,
};

/* A reaction closure of AsyncFromSyncIteratorContinuation. */
static embi_js_value afs_fn_new(embi_js_runtime *rt, enum afs_kind kind, bool done,
                                embi_js_value rec)
{
    embi_js_varray *st = embi_js_varray_new(rt, 3);
    EMBI_JS_STORE(st, st->elems[0], embi_js_from_int32((int32_t)kind));
    EMBI_JS_STORE(st, st->elems[1], embi_js_from_int32(done ? 1 : 0));
    EMBI_JS_STORE(st, st->elems[2], rec);
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_afs_fn);
    f->internal = st;
    EMBI_JS_WB_STRUCT(f);
    return embi_js_from_object(f);
}

/* Rejects promise with the pending exception (taken). */
static embi_js_value afs_reject_pending(embi_js_runtime *rt, embi_js_object *promise)
{
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    (void)embi_js_promise_reject_with(rt, promise, ex);
    return embi_js_from_object(promise);
}

/* AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord,
 * closeOnRejection): the result is an object. Returns the promise. */
static embi_js_value afs_continue(embi_js_runtime *rt, embi_js_value result, embi_js_value rec,
                                  bool close_on_reject, embi_js_object *promise)
{
    embi_js_pcap_t cap = {embi_js_from_object(promise), UNDEF, UNDEF};
    embi_js_value dv = embi_js_get(rt, embi_js_as_object(result), embi_js_iter_key(rt, EMBI_JS_IK_DONE));
    if (dv == EXC) {
        return afs_reject_pending(rt, promise);
    }
    bool done = embi_js_to_boolean(dv);
    embi_js_value value = embi_js_get(rt, embi_js_as_object(result), embi_js_iter_key(rt, EMBI_JS_IK_VALUE));
    if (value == EXC) {
        return afs_reject_pending(rt, promise);
    }
    embi_js_value wrapper = embi_js_promise_resolve(rt, rt->protos[EMBI_JS_PROTO_PROMISE_CTOR], value);
    if (wrapper == EXC) {
        if (!done && close_on_reject) {
            embi_js_ir_close_throw(rt, rec);
        }
        return afs_reject_pending(rt, promise);
    }
    embi_js_value on_ok = afs_fn_new(rt, AFS_UNWRAP, done, rec);
    embi_js_value on_err = (done || !close_on_reject) ? UNDEF : afs_fn_new(rt, AFS_CLOSE_THROW, false, rec);
    (void)embi_js_promise_then(rt, embi_js_as_object(wrapper), on_ok, on_err, &cap);
    return cap.promise;
}

/* %AsyncFromSyncIteratorPrototype%.next (0), return (1) and throw (2) (ECMA-262 27.1.6.2). */
static embi_js_value afs_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *promise = embi_js_promise_new(rt);
    if (!embi_js_is_object(this_v) || embi_js_as_object(this_v)->klass != &class_async_from_sync) {
        (void)embi_js_throw_type_error(rt, "not an async-from-sync iterator");
        return afs_reject_pending(rt, promise);
    }
    embi_js_value rec = embi_js_from_cell((embi_js_cell *)embi_js_as_object(this_v)->internal);
    const embi_js_varray *sr = (const embi_js_varray *)embi_js_as_cell(rec);
    embi_js_value iter = sr->elems[0];
    if (magic == 0) {
        embi_js_value res = embi_js_call(rt, sr->elems[1], iter, argc > 0 ? 1 : 0, argv);
        if (res == EXC) {
            return afs_reject_pending(rt, promise);
        }
        if (!embi_js_is_object(res)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            return afs_reject_pending(rt, promise);
        }
        return afs_continue(rt, res, rec, true, promise);
    }
    if (magic == 1) {
        embi_js_value rf = embi_js_get_method(rt, iter, embi_js_key_c(rt, "return"));
        if (rf == EXC) {
            return afs_reject_pending(rt, promise);
        }
        if (rf == UNDEF) {
            embi_js_value res = embi_js_iter_result_new(rt, ARG(0), true);
            (void)embi_js_promise_resolve_with(rt, promise, res);
            return embi_js_from_object(promise);
        }
        embi_js_value res = embi_js_call(rt, rf, iter, argc > 0 ? 1 : 0, argv);
        if (res == EXC) {
            return afs_reject_pending(rt, promise);
        }
        if (!embi_js_is_object(res)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            return afs_reject_pending(rt, promise);
        }
        return afs_continue(rt, res, rec, false, promise);
    }
    embi_js_value tf = embi_js_get_method(rt, iter, embi_js_key_c(rt, "throw"));
    if (tf == EXC) {
        return afs_reject_pending(rt, promise);
    }
    if (tf == UNDEF) {
        /* No throw method: the iterator is closed first, then the request rejects. */
        if (!embi_js_ir_close(rt, rec)) {
            return afs_reject_pending(rt, promise);
        }
        (void)embi_js_throw_type_error(rt, "The iterator does not provide a 'throw' method");
        return afs_reject_pending(rt, promise);
    }
    embi_js_value res = embi_js_call(rt, tf, iter, argc > 0 ? 1 : 0, argv);
    if (res == EXC) {
        return afs_reject_pending(rt, promise);
    }
    if (!embi_js_is_object(res)) {
        (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
        return afs_reject_pending(rt, promise);
    }
    return afs_continue(rt, res, rec, true, promise);
}

embi_js_value embi_js_async_from_sync_create(embi_js_runtime *rt, embi_js_value sync_rec)
{
    embi_js_object *o = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_ASYNC_FROM_SYNC],
                                                 &class_async_from_sync);
    o->internal = embi_js_as_cell(sync_rec);
    EMBI_JS_WB(o, sync_rec);
    return embi_js_from_object(o);
}

void embi_js_init_builtins_iterator(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];
    embi_js_value fn_proto = rt->protos[EMBI_JS_PROTO_FUNCTION];

    /* %IteratorPrototype%: [Symbol.iterator]() returns this. */
    embi_js_object *iter_proto = embi_js_object_new(rt, obj_proto);
    rt->protos[EMBI_JS_PROTO_ITERATOR] = embi_js_from_object(iter_proto);
    embi_js_object *self_fn = embi_js_native_function_new(r, "[Symbol.iterator]", 0, iter_self, 0);
    embi_js_define_raw(rt, iter_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(self_fn), ATTR_WC);

    /* The Iterator constructor and its prototype property. */
    embi_js_object *ctor = embi_js_native_constructor_new(r, "Iterator", 0, iterator_ctor, 0);
    rt->protos[EMBI_JS_PROTO_ITERATOR_CTOR] = embi_js_from_object(ctor);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(iter_proto), 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Iterator"), embi_js_from_object(ctor),
                       ATTR_WC);
    embi_js_define_method(r, ctor, "from", iterator_from, 1, 0);
    embi_js_define_method(r, ctor, "concat", iterator_concat, 0, 0);

    /* Iterator.prototype.constructor and @@toStringTag are accessors (ES2025). */
    embi_js_object *g_ctor = embi_js_native_function_new(r, "get constructor", 0, proto_tag_get, 0);
    embi_js_object *s_ctor = embi_js_native_function_new(r, "set constructor", 1, proto_tag_set, 0);
    embi_js_define_accessor_raw(rt, iter_proto, embi_js_key_c(rt, "constructor"),
                                embi_js_from_object(g_ctor), embi_js_from_object(s_ctor), ATTR_C);
    embi_js_object *g_tag = embi_js_native_function_new(r, "get [Symbol.toStringTag]", 0,
                                                        proto_tag_get, 1);
    embi_js_object *s_tag = embi_js_native_function_new(r, "set [Symbol.toStringTag]", 1,
                                                        proto_tag_set, 1);
    embi_js_define_accessor_raw(rt, iter_proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                                embi_js_from_object(g_tag), embi_js_from_object(s_tag), ATTR_C);

    embi_js_define_method(r, iter_proto, "map", lazy_method, 1, HK_MAP);
    embi_js_define_method(r, iter_proto, "filter", lazy_method, 1, HK_FILTER);
    embi_js_define_method(r, iter_proto, "take", lazy_method, 1, HK_TAKE);
    embi_js_define_method(r, iter_proto, "drop", lazy_method, 1, HK_DROP);
    embi_js_define_method(r, iter_proto, "flatMap", lazy_method, 1, HK_FLAT);
    embi_js_define_method(r, iter_proto, "reduce", eager_method, 1, EM_REDUCE);
    embi_js_define_method(r, iter_proto, "toArray", eager_method, 0, EM_TO_ARRAY);
    embi_js_define_method(r, iter_proto, "forEach", eager_method, 1, EM_FOR_EACH);
    embi_js_define_method(r, iter_proto, "some", eager_method, 1, EM_SOME);
    embi_js_define_method(r, iter_proto, "every", eager_method, 1, EM_EVERY);
    embi_js_define_method(r, iter_proto, "find", eager_method, 1, EM_FIND);

    /* %IteratorHelperPrototype% */
    embi_js_object *helper_proto = embi_js_object_new(rt, embi_js_from_object(iter_proto));
    rt->protos[EMBI_JS_PROTO_ITER_HELPER] = embi_js_from_object(helper_proto);
    embi_js_define_method(r, helper_proto, "next", helper_method, 0, 0);
    embi_js_define_method(r, helper_proto, "return", helper_method, 0, 1);
    define_tag(rt, helper_proto, "Iterator Helper");

    /* %WrapForValidIteratorPrototype% */
    embi_js_object *wrap_proto = embi_js_object_new(rt, embi_js_from_object(iter_proto));
    rt->protos[EMBI_JS_PROTO_WRAP_ITER] = embi_js_from_object(wrap_proto);
    embi_js_define_method(r, wrap_proto, "next", wrap_method, 0, 0);
    embi_js_define_method(r, wrap_proto, "return", wrap_method, 0, 1);

    /* %GeneratorFunction.prototype% and %GeneratorPrototype% */
    embi_js_object *gen_proto = embi_js_object_new(rt, embi_js_from_object(iter_proto));
    rt->protos[EMBI_JS_PROTO_GENERATOR] = embi_js_from_object(gen_proto);
    embi_js_object *gen_fn_proto = embi_js_object_new(rt, fn_proto);
    rt->protos[EMBI_JS_PROTO_GENERATOR_FUNCTION] = embi_js_from_object(gen_fn_proto);
    embi_js_define_raw(rt, gen_fn_proto, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(gen_proto), ATTR_C);
    define_tag(rt, gen_fn_proto, "GeneratorFunction");
    embi_js_define_raw(rt, gen_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(gen_fn_proto), ATTR_C);

    /* The GeneratorFunction constructor: its prototype is the Function constructor (set up before
     * this file runs), and it is not a global. */
    embi_js_object *gf_ctor =
        embi_js_native_constructor_new(r, "GeneratorFunction", 1, generator_function_ctor, 0);
    embi_js_value fn_ctor = embi_js_get(rt, r->global, embi_js_key_c(rt, "Function"));
    if (embi_js_is_object(fn_ctor)) {
        (void)embi_js_obj_set_prototype_of(rt, gf_ctor, fn_ctor);
    }
    embi_js_define_raw(rt, gf_ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(gen_fn_proto), 0);
    embi_js_define_raw(rt, gen_fn_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(gf_ctor), ATTR_C);
    embi_js_define_method(r, gen_proto, "next", gen_method, 1, 0);
    embi_js_define_method(r, gen_proto, "return", gen_method, 1, 2);
    embi_js_define_method(r, gen_proto, "throw", gen_method, 1, 1);
    define_tag(rt, gen_proto, "Generator");
    /* J3b: %AsyncIteratorPrototype%, %AsyncFromSyncIteratorPrototype%, %AsyncGeneratorFunction%
     * and %AsyncFunction% (ECMA-262 27.1.4, 27.1.6, 27.4, 27.7). */
    embi_js_object *async_iter_proto = embi_js_object_new(rt, obj_proto);
    rt->protos[EMBI_JS_PROTO_ASYNC_ITERATOR] = embi_js_from_object(async_iter_proto);
    embi_js_object *async_self =
        embi_js_native_function_new(r, "[Symbol.asyncIterator]", 0, iter_self, 0);
    embi_js_define_raw(rt, async_iter_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ASYNC_ITERATOR),
                       embi_js_from_object(async_self), ATTR_WC);

    embi_js_object *afs_proto = embi_js_object_new(rt, embi_js_from_object(async_iter_proto));
    rt->protos[EMBI_JS_PROTO_ASYNC_FROM_SYNC] = embi_js_from_object(afs_proto);
    embi_js_define_method(r, afs_proto, "next", afs_method, 1, 0);
    embi_js_define_method(r, afs_proto, "return", afs_method, 1, 1);
    embi_js_define_method(r, afs_proto, "throw", afs_method, 1, 2);

    embi_js_object *ag_proto = embi_js_object_new(rt, embi_js_from_object(async_iter_proto));
    rt->protos[EMBI_JS_PROTO_ASYNC_GENERATOR] = embi_js_from_object(ag_proto);
    embi_js_object *agf_proto = embi_js_object_new(rt, fn_proto);
    rt->protos[EMBI_JS_PROTO_ASYNC_GENERATOR_FUNCTION] = embi_js_from_object(agf_proto);
    embi_js_define_raw(rt, agf_proto, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(ag_proto), ATTR_C);
    define_tag(rt, agf_proto, "AsyncGeneratorFunction");
    embi_js_define_raw(rt, ag_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(agf_proto), ATTR_C);
    embi_js_object *agf_ctor =
        embi_js_native_constructor_new(r, "AsyncGeneratorFunction", 1, async_generator_function_ctor, 0);
    if (embi_js_is_object(fn_ctor)) {
        (void)embi_js_obj_set_prototype_of(rt, agf_ctor, fn_ctor);
    }
    embi_js_define_raw(rt, agf_ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(agf_proto), 0);
    embi_js_define_raw(rt, agf_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(agf_ctor), ATTR_C);
    embi_js_define_method(r, ag_proto, "next", agen_method, 1, 0);
    embi_js_define_method(r, ag_proto, "return", agen_method, 1, 2);
    embi_js_define_method(r, ag_proto, "throw", agen_method, 1, 1);
    define_tag(rt, ag_proto, "AsyncGenerator");

    embi_js_object *af_proto = embi_js_object_new(rt, fn_proto);
    rt->protos[EMBI_JS_PROTO_ASYNC_FUNCTION] = embi_js_from_object(af_proto);
    define_tag(rt, af_proto, "AsyncFunction");
    embi_js_object *af_ctor =
        embi_js_native_constructor_new(r, "AsyncFunction", 1, async_function_ctor, 0);
    if (embi_js_is_object(fn_ctor)) {
        (void)embi_js_obj_set_prototype_of(rt, af_ctor, fn_ctor);
    }
    embi_js_define_raw(rt, af_ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(af_proto), 0);
    embi_js_define_raw(rt, af_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(af_ctor), ATTR_C);
    (void)obj_proto;
}
