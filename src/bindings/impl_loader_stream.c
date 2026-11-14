/*
 * ReadableStream, its default reader, its controller and its async iterator (fetch.webidl, W7c; Streams Standard,
 * minimal: default streams, no byte streams, no tee and no pipes).
 *
 * The state machine is src/loader/stream.c (embi_stream, a flat struct). Everything that holds a JS value or grows lives in
 * JS: the wrapper's ref slot is an array of RS_N slots (the chunk queue as value, size pairs, the pending read promises,
 * the underlying source and its functions, the stored error, the reader and the closed promise of the current reader). The
 * wrapper's block is a flat rs_rec, so the finalizer frees everything.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"

#include <math.h>

enum {
    RS_QUEUE,  /* chunk, size, chunk, size ... from the record's qh */
    RS_READS,  /* pending read promises from the record's rh */
    RS_SOURCE, /* the underlying source object (or undefined) */
    RS_READER, /* the reader wrapper holding the lock, or undefined */
    RS_ERROR,  /* the stored error */
    RS_CLOSED, /* the closed promise of the current reader */
    RS_PULL,
    RS_CANCEL,
    RS_CTRL,   /* the controller wrapper */
    RS_SIZEFN, /* the strategy's size function (or undefined) */
    RS_N
};

typedef struct rs_rec {
    embi_stream s;
    uint32_t qh; /* index of the oldest queued element in RS_QUEUE */
    uint32_t rh; /* index of the oldest pending read in RS_READS */
    /* Hooks for a push stream (a network body): on_read(bytes) when a queued chunk is read, on_cancel when the reader
     * cancels. Both optional; hook_user is passed to them. */
    void (*on_read)(void *user, size_t bytes);
    void (*on_cancel)(void *user);
    void *hook_user;
} rs_rec;

/* Reader aux flags. */
#define RD_RELEASED 1u
#define RD_AUTO 2u /* an async iterator's reader: the lock is released when the stream closes or errors */
/* Iterator aux flag. */
#define IT_PREVENT_CANCEL 1u

static rs_rec *rs_of(embi_bind_obj *bo)
{
    return bo->ptr;
}

static embi_js_value slot(embi_bind_ctx *c, embi_bind_obj *bo, int i)
{
    return embi_bind_array_get(c, bo->ref, (uint32_t)i);
}

static void slot_set(embi_bind_ctx *c, embi_bind_obj *bo, int i, embi_js_value v)
{
    embi_bind_array_set(c, bo->ref, (uint32_t)i, v);
}

static embi_bind_obj *stream_bo(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    return (bo != nullptr && bo->iface == EMBI_IF_ReadableStream && !embi_bind_stale(bo)) ? bo : nullptr;
}

embi_stream *ldr_stream_core(embi_js_value v)
{
    embi_bind_obj *bo = stream_bo(v);
    return bo != nullptr ? &rs_of(bo)->s : nullptr;
}

/* The state of a ReadableStream wrapper as the standard names it, or nullptr when v is not one (W7f: the JS side of the
 * pipes reads it, and the stored error, synchronously). */
const char *ldr_stream_state_name(embi_js_value v)
{
    embi_stream *s = ldr_stream_core(v);
    if (s == nullptr) {
        return nullptr;
    }
    return s->state == EMBI_STREAM_READABLE ? "readable" : (s->state == EMBI_STREAM_CLOSED ? "closed" : "errored");
}

embi_js_value ldr_stream_stored_error(embi_bind_ctx *c, embi_js_value v)
{
    embi_bind_obj *bo = stream_bo(v);
    return bo != nullptr ? slot(c, bo, RS_ERROR) : EMBI_JS_UNDEFINED;
}

/* Marks a promise handled so that a rejection nobody reads is not reported (the closed promise is handled by spec). */
static embi_js_value noop_reaction(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)rl;
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return EMBI_JS_UNDEFINED;
}

/* The reaction must be callable: with no handlers and no result capability the rejection would rethrow inside the job
 * and be reported as an uncaught error. */
static void mark_handled(embi_bind_ctx *c, embi_js_value p)
{
    embi_js_value noop = ldr_native(c, "", 1, noop_reaction, 0);
    (void)embi_js_promise_then(c->rt, embi_js_as_object(p), EMBI_JS_UNDEFINED, noop, nullptr);
}

static void arr_reset(embi_bind_ctx *c, embi_js_value arr)
{
    (void)embi_js_put(c->rt, embi_js_as_object(arr), embi_js_key_c(c->rt, "length"), embi_js_from_int32(0), false);
}

/* ------------------------------------------------------------------ creation */

static embi_bind_obj *stream_new(embi_bind_ctx *c, double hwm, embi_js_value *out)
{
    (void)ldr_state_of(c);
    rs_rec *r = embi_alloc(sizeof *r);
    *r = (rs_rec){0};
    embi_stream_init(&r->s, hwm);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_ReadableStream, r, r, sizeof *r);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_js_value st = embi_bind_array_new(c, RS_N);
    embi_bind_set_ref(bo, st);
    embi_bind_array_set(c, st, RS_QUEUE, embi_bind_array_new(c, 0));
    embi_bind_array_set(c, st, RS_READS, embi_bind_array_new(c, 0));
    embi_js_value ctrl = embi_bind_view_new(c, EMBI_IF_ReadableStreamDefaultController, bo, 0, nullptr, 0);
    embi_bind_array_set(c, st, RS_CTRL, ctrl);
    *out = v;
    return bo;
}

/* ------------------------------------------------------------------ settling */

static void settle_read(embi_bind_ctx *c, embi_js_value promise, embi_js_value value, bool done)
{
    (void)embi_js_promise_resolve_with(c->rt, embi_js_as_object(promise), ldr_iter_result(c, value, done));
}

/* Takes the oldest pending read promise off the queue. */
static embi_js_value reads_pop(embi_bind_ctx *c, embi_bind_obj *bo)
{
    rs_rec *r = rs_of(bo);
    embi_js_value arr = slot(c, bo, RS_READS);
    embi_js_value p = embi_bind_array_get(c, arr, r->rh++);
    if (r->rh >= embi_bind_array_len(c, arr)) {
        arr_reset(c, arr);
        r->rh = 0;
    }
    return p;
}

static void unlock_for_auto_reader(embi_bind_ctx *c, embi_bind_obj *bo)
{
    embi_js_value reader = slot(c, bo, RS_READER);
    embi_bind_obj *rb = embi_bind_obj_of(reader);
    if (rb != nullptr && (rb->aux & RD_AUTO) != 0 && (rb->aux & RD_RELEASED) == 0) {
        rb->aux |= RD_RELEASED;
        rs_of(bo)->s.locked = false;
        slot_set(c, bo, RS_READER, EMBI_JS_UNDEFINED);
    }
}

/* The effects of the stream becoming closed: pending reads end, the closed promise resolves. */
static void on_closed(embi_bind_ctx *c, embi_bind_obj *bo)
{
    rs_rec *r = rs_of(bo);
    uint32_t n = embi_stream_take_reads(&r->s);
    for (uint32_t i = 0; i < n; i++) {
        settle_read(c, reads_pop(c, bo), EMBI_JS_UNDEFINED, true);
    }
    embi_js_value cp = slot(c, bo, RS_CLOSED);
    if (embi_js_is_object(cp)) {
        (void)embi_js_promise_resolve_with(c->rt, embi_js_as_object(cp), EMBI_JS_UNDEFINED);
    }
    unlock_for_auto_reader(c, bo);
}

static void error_stream(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value e)
{
    rs_rec *r = rs_of(bo);
    if (!embi_stream_error(&r->s)) {
        return;
    }
    slot_set(c, bo, RS_ERROR, e);
    arr_reset(c, slot(c, bo, RS_QUEUE));
    r->qh = 0;
    uint32_t n = embi_stream_take_reads(&r->s);
    for (uint32_t i = 0; i < n; i++) {
        (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(reads_pop(c, bo)), e);
    }
    embi_js_value cp = slot(c, bo, RS_CLOSED);
    if (embi_js_is_object(cp)) {
        (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(cp), e);
    }
    unlock_for_auto_reader(c, bo);
}

/* ------------------------------------------------------------------ pulling */

static embi_js_value pull_ok(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);
static embi_js_value pull_err(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic);

/* Subscribes ok and err (natives with the persistent id of `holder`) to the result of a source function: PromiseResolve
 * of r, then. Returns false with an exception pending when the subscription failed. */
static bool after(embi_bind_ctx *c, embi_js_value result, embi_js_native ok, embi_js_native err, uint32_t pid)
{
    embi_js_value p = result;
    if (!embi_js_is_promise(result)) {
        embi_js_object *po = embi_js_promise_new(c->rt);
        p = embi_js_from_object(po);
        (void)embi_js_promise_resolve_with(c->rt, po, result);
    }
    embi_js_value f_ok = ldr_native(c, "", 1, ok, (int)pid);
    embi_js_value f_err = ldr_native(c, "", 1, err, (int)pid);
    (void)embi_js_promise_then(c->rt, embi_js_as_object(p), f_ok, f_err, nullptr);
    return true;
}

static void call_pull_if_needed(embi_bind_ctx *c, embi_bind_obj *bo)
{
    rs_rec *r = rs_of(bo);
    if (!embi_stream_should_pull(&r->s)) {
        return;
    }
    embi_js_value pull = slot(c, bo, RS_PULL);
    if (!embi_js_is_callable(pull)) {
        return;
    }
    if (r->s.pulling) {
        r->s.pull_again = true;
        return;
    }
    r->s.pulling = true;
    embi_js_value ctrl = slot(c, bo, RS_CTRL);
    embi_js_value res = embi_js_call(c->rt, pull, slot(c, bo, RS_SOURCE), 1, &ctrl);
    if (res == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        r->s.pulling = false;
        error_stream(c, bo, ex);
        return;
    }
    uint32_t pid = embi_js_persistent_new(c->rt, embi_bind_obj_value(bo));
    if (!after(c, res, pull_ok, pull_err, pid)) {
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        embi_js_persistent_free(c->rt, pid);
        r->s.pulling = false;
        error_stream(c, bo, ex);
    }
}

static embi_js_value pull_ok(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value sv = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    embi_bind_obj *bo = stream_bo(sv);
    if (bo != nullptr) {
        rs_rec *r = rs_of(bo);
        r->s.pulling = false;
        if (r->s.pull_again) {
            r->s.pull_again = false;
            call_pull_if_needed(c, bo);
        }
    }
    return EMBI_JS_UNDEFINED;
}

static embi_js_value pull_err(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value sv = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    embi_bind_obj *bo = stream_bo(sv);
    if (bo != nullptr) {
        rs_of(bo)->s.pulling = false;
        error_stream(c, bo, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    }
    return EMBI_JS_UNDEFINED;
}

static embi_js_value start_ok(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value sv = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    embi_bind_obj *bo = stream_bo(sv);
    if (bo != nullptr) {
        rs_of(bo)->s.started = true;
        call_pull_if_needed(c, bo);
    }
    return EMBI_JS_UNDEFINED;
}

static embi_js_value start_err(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value sv = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    embi_bind_obj *bo = stream_bo(sv);
    if (bo != nullptr) {
        error_stream(c, bo, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    }
    return EMBI_JS_UNDEFINED;
}

/* ------------------------------------------------------------------ constructor */

embi_js_value embi_impl_ReadableStream_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0,
                                                   embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_runtime *rt = c->rt;
    embi_js_value source = embi_js_is_nullish(a0) ? EMBI_JS_UNDEFINED : a0;
    if (!embi_js_is_undefined(source) && !embi_js_is_object(source)) {
        (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': The provided value is not of type 'UnderlyingSource'.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value type = ldr_get(c, source, "type");
    embi_js_value start = ldr_get(c, source, "start");
    embi_js_value pull = ldr_get(c, source, "pull");
    embi_js_value cancel = ldr_get(c, source, "cancel");
    if (type == EMBI_JS_EXCEPTION || start == EMBI_JS_EXCEPTION || pull == EMBI_JS_EXCEPTION || cancel == EMBI_JS_EXCEPTION) {
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (!embi_js_is_undefined(type)) {
        embi_sb t = {nullptr, 0, 0};
        bool ok = ldr_to_utf8(c, type, &t);
        bool is_default = ok && embi_str_eq((embi_str){t.data, t.len}, EMBI_STR("default"));
        embi_sb_free(&t);
        if (!ok) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        if (!is_default) {
            (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': Byte streams are not supported.");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
    }
    double hwm = 1.0;
    embi_js_value sizefn = EMBI_JS_UNDEFINED;
    if (!embi_js_is_nullish(a1)) {
        embi_js_value h = ldr_get(c, a1, "highWaterMark");
        embi_js_value sz = ldr_get(c, a1, "size");
        if (h == EMBI_JS_EXCEPTION || sz == EMBI_JS_EXCEPTION) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        if (!embi_js_is_undefined(h)) {
            if (!embi_js_to_number(rt, h, &hwm)) {
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            if (hwm != hwm || hwm < 0) {
                (void)embi_js_throw_range_error(rt, "Failed to construct 'ReadableStream': Invalid highWaterMark");
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
        }
        if (!embi_js_is_undefined(sz)) {
            if (!embi_js_is_callable(sz)) {
                (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': The strategy's size is not a function.");
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            sizefn = sz;
        }
    }
    if (!embi_js_is_undefined(start) && !embi_js_is_callable(start)) {
        (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': The underlying source's start is not a function.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (!embi_js_is_undefined(pull) && !embi_js_is_callable(pull)) {
        (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': The underlying source's pull is not a function.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (!embi_js_is_undefined(cancel) && !embi_js_is_callable(cancel)) {
        (void)embi_js_throw_type_error(rt, "Failed to construct 'ReadableStream': The underlying source's cancel is not a function.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value sv;
    embi_bind_obj *bo = stream_new(c, hwm, &sv);
    embi_bind_adopt_new_target(c, sv, new_target);
    slot_set(c, bo, RS_SOURCE, source);
    slot_set(c, bo, RS_PULL, pull);
    slot_set(c, bo, RS_CANCEL, cancel);
    slot_set(c, bo, RS_SIZEFN, sizefn);
    embi_js_value res = EMBI_JS_UNDEFINED;
    if (embi_js_is_callable(start)) {
        embi_js_value ctrl = slot(c, bo, RS_CTRL);
        res = embi_js_call(rt, start, source, 1, &ctrl);
        if (res == EMBI_JS_EXCEPTION) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
    }
    uint32_t pid = embi_js_persistent_new(rt, sv);
    if (!after(c, res, start_ok, start_err, pid)) {
        embi_js_persistent_free(rt, pid);
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    return embi_bind_leave(sc, sv);
}

embi_js_value ldr_stream_from_bytes(embi_bind_ctx *c, const uint8_t *p, size_t n)
{
    embi_js_value sv;
    embi_bind_obj *bo = stream_new(c, 1.0, &sv);
    rs_rec *r = rs_of(bo);
    r->s.started = true;
    if (n > 0) {
        embi_js_value chunk = ldr_new_uint8(c, p, n);
        if (chunk == EMBI_JS_EXCEPTION) {
            return chunk;
        }
        (void)embi_stream_enqueue(&r->s, 1.0);
        embi_js_value q = slot(c, bo, RS_QUEUE);
        embi_bind_array_set(c, q, 0, chunk);
        embi_bind_array_set(c, q, 1, embi_js_from_double(1.0));
        (void)embi_stream_close(&r->s);
    } else {
        (void)embi_stream_close(&r->s);
    }
    return sv;
}

/* ------------------------------------------------------------------ push streams (N1b: a network response body) */

embi_js_value ldr_stream_push_new(embi_bind_ctx *c)
{
    embi_js_value sv;
    embi_bind_obj *bo = stream_new(c, 1.0, &sv);
    rs_of(bo)->s.started = true; /* no source: nothing to start or pull */
    return sv;
}

/* Returns the bytes queued (0 when the chunk went straight to a pending read, or was refused). */
size_t ldr_stream_push_chunk(embi_bind_ctx *c, embi_js_value sv, const uint8_t *p, size_t n)
{
    embi_bind_obj *bo = stream_bo(sv);
    if (bo == nullptr) {
        return 0;
    }
    rs_rec *r = rs_of(bo);
    if (!embi_stream_can_enqueue(&r->s)) {
        return 0;
    }
    embi_js_value chunk = ldr_new_uint8(c, p, n);
    if (chunk == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        error_stream(c, bo, ex);
        return 0;
    }
    switch (embi_stream_enqueue(&r->s, 1.0)) {
    case EMBI_STREAM_ENQ_DELIVERED:
        settle_read(c, reads_pop(c, bo), chunk, false);
        return 0;
    case EMBI_STREAM_ENQ_QUEUED: {
        embi_js_value q = slot(c, bo, RS_QUEUE);
        uint32_t len = embi_bind_array_len(c, q);
        embi_bind_array_set(c, q, len, chunk);
        embi_bind_array_set(c, q, len + 1u, embi_js_number(1.0));
        return n;
    }
    case EMBI_STREAM_ENQ_INVALID:
    default:
        return 0;
    }
}

void ldr_stream_set_hooks(embi_js_value sv, void (*on_read)(void *user, size_t bytes), void (*on_cancel)(void *user),
                          void *user)
{
    embi_bind_obj *bo = stream_bo(sv);
    if (bo == nullptr) {
        return;
    }
    rs_rec *r = rs_of(bo);
    r->on_read = on_read;
    r->on_cancel = on_cancel;
    r->hook_user = user;
}

void ldr_stream_push_close(embi_bind_ctx *c, embi_js_value sv)
{
    embi_bind_obj *bo = stream_bo(sv);
    if (bo == nullptr) {
        return;
    }
    if (embi_stream_close(&rs_of(bo)->s) == EMBI_STREAM_CLOSE_NOW) {
        on_closed(c, bo);
    }
}

void ldr_stream_push_error(embi_bind_ctx *c, embi_js_value sv, embi_js_value reason)
{
    embi_bind_obj *bo = stream_bo(sv);
    if (bo != nullptr) {
        error_stream(c, bo, reason);
    }
}

void ldr_stream_consume(embi_js_value v)
{
    embi_bind_obj *bo = stream_bo(v);
    if (bo == nullptr) {
        return;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_stream_consume_all(&rs_of(bo)->s);
    rs_of(bo)->s.locked = true; /* the consumer holds the reader for good (Fetch: consume body) */
    arr_reset(c, slot(c, bo, RS_QUEUE));
    rs_of(bo)->qh = 0;
}

/* ------------------------------------------------------------------ reading */

/* ReadableStreamDefaultReaderRead on the stream: returns the promise of the result. */
static embi_js_value stream_read(embi_bind_ctx *c, embi_bind_obj *bo)
{
    rs_rec *r = rs_of(bo);
    embi_js_value q = slot(c, bo, RS_QUEUE);
    double head_size = 1.0;
    embi_js_value chunk = EMBI_JS_UNDEFINED;
    if (embi_stream_queued(&r->s) > 0) {
        chunk = embi_bind_array_get(c, q, r->qh);
        embi_js_value sz = embi_bind_array_get(c, q, r->qh + 1u);
        head_size = embi_js_is_number(sz) ? embi_js_as_double(sz) : 1.0;
    }
    bool closed_now = false;
    switch (embi_stream_read(&r->s, head_size, &closed_now)) {
    case EMBI_STREAM_READ_CHUNK: {
        r->qh += 2;
        if (r->qh >= embi_bind_array_len(c, q)) {
            arr_reset(c, q);
            r->qh = 0;
        }
        if (r->on_read != nullptr) {
            const uint8_t *bp = nullptr;
            size_t bn = 0;
            bool detached = false;
            if (ldr_buffer_source(chunk, &bp, &bn, &detached)) {
                r->on_read(r->hook_user, bn);
            }
        }
        embi_js_value p = ldr_promise_resolve(c, ldr_iter_result(c, chunk, false));
        if (closed_now) {
            on_closed(c, bo);
        } else {
            call_pull_if_needed(c, bo);
        }
        return p;
    }
    case EMBI_STREAM_READ_PENDING: {
        embi_js_object *po = embi_js_promise_new(c->rt);
        embi_js_value p = embi_js_from_object(po);
        embi_js_value reads = slot(c, bo, RS_READS);
        embi_bind_array_set(c, reads, embi_bind_array_len(c, reads), p);
        call_pull_if_needed(c, bo);
        return p;
    }
    case EMBI_STREAM_READ_CLOSED: {
        embi_js_value p = ldr_promise_resolve(c, ldr_iter_result(c, EMBI_JS_UNDEFINED, true));
        unlock_for_auto_reader(c, bo);
        return p;
    }
    case EMBI_STREAM_READ_ERRORED:
    default: {
        embi_js_value p = ldr_promise_reject(c, slot(c, bo, RS_ERROR));
        unlock_for_auto_reader(c, bo);
        return p;
    }
    }
}

/* ------------------------------------------------------------------ cancel */

static embi_js_value cancel_ok(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value p = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    (void)embi_js_promise_resolve_with(c->rt, embi_js_as_object(p), EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

static embi_js_value cancel_err(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value p = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(p), argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

static embi_js_value stream_cancel(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value reason)
{
    rs_rec *r = rs_of(bo);
    r->s.disturbed = true;
    if (r->s.state == EMBI_STREAM_CLOSED) {
        return ldr_promise_resolve(c, EMBI_JS_UNDEFINED);
    }
    if (r->s.state == EMBI_STREAM_ERRORED) {
        return ldr_promise_reject(c, slot(c, bo, RS_ERROR));
    }
    (void)embi_stream_cancel(&r->s);
    arr_reset(c, slot(c, bo, RS_QUEUE));
    r->qh = 0;
    if (r->on_cancel != nullptr) {
        r->on_cancel(r->hook_user);
    }
    on_closed(c, bo);
    embi_js_object *po = embi_js_promise_new(c->rt);
    embi_js_value p = embi_js_from_object(po);
    embi_js_value cancel = slot(c, bo, RS_CANCEL);
    if (!embi_js_is_callable(cancel)) {
        (void)embi_js_promise_resolve_with(c->rt, po, EMBI_JS_UNDEFINED);
        return p;
    }
    embi_js_value res = embi_js_call(c->rt, cancel, slot(c, bo, RS_SOURCE), 1, &reason);
    if (res == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        (void)embi_js_promise_reject_with(c->rt, po, ex);
        return p;
    }
    uint32_t pid = embi_js_persistent_new(c->rt, p);
    if (!after(c, res, cancel_ok, cancel_err, pid)) {
        embi_js_persistent_free(c->rt, pid);
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        (void)embi_js_promise_reject_with(c->rt, po, ex);
    }
    return p;
}

/* ------------------------------------------------------------------ readers */

/* Locks the stream with a new reader. Returns the reader wrapper, or EMBI_JS_EXCEPTION (TypeError: locked). */
static embi_js_value acquire_reader(embi_bind_ctx *c, embi_bind_obj *bo, uint32_t aux, const char *what)
{
    rs_rec *r = rs_of(bo);
    if (r->s.locked) {
        (void)embi_js_throw_type_error(c->rt, "%s: ReadableStream is locked", what);
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value reader = embi_bind_view_new(c, EMBI_IF_ReadableStreamDefaultReader, bo, aux, nullptr, 0);
    r->s.locked = true;
    slot_set(c, bo, RS_READER, reader);
    embi_js_object *po = embi_js_promise_new(c->rt);
    embi_js_value cp = embi_js_from_object(po);
    mark_handled(c, cp);
    if (r->s.state == EMBI_STREAM_CLOSED) {
        (void)embi_js_promise_resolve_with(c->rt, po, EMBI_JS_UNDEFINED);
    } else if (r->s.state == EMBI_STREAM_ERRORED) {
        (void)embi_js_promise_reject_with(c->rt, po, slot(c, bo, RS_ERROR));
    }
    slot_set(c, bo, RS_CLOSED, cp);
    return reader;
}

static void release_reader(embi_bind_ctx *c, embi_bind_obj *rb, embi_bind_obj *bo)
{
    rs_rec *r = rs_of(bo);
    rb->aux |= RD_RELEASED;
    r->s.locked = false;
    slot_set(c, bo, RS_READER, EMBI_JS_UNDEFINED);
    embi_js_value err = ldr_make_type_error(c, "Reader was released");
    uint32_t n = embi_stream_take_reads(&r->s);
    for (uint32_t i = 0; i < n; i++) {
        (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(reads_pop(c, bo)), err);
    }
    if (r->s.state == EMBI_STREAM_READABLE) {
        embi_js_value cp = slot(c, bo, RS_CLOSED);
        if (embi_js_is_object(cp)) {
            (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(cp), err);
        }
    }
}

/* The stream of a live reader, or nullptr after releaseLock (the caller rejects). */
static embi_bind_obj *reader_stream(embi_bind_obj *rb)
{
    if (embi_bind_stale(rb) || (rb->aux & RD_RELEASED) != 0) {
        return nullptr;
    }
    return rb->owner;
}

embi_js_value embi_impl_ReadableStreamDefaultReader_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *bo = stream_bo(a0);
    if (bo == nullptr) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'ReadableStreamDefaultReader': parameter 1 is not of type 'ReadableStream'.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value reader = acquire_reader(c, bo, 0, "Failed to construct 'ReadableStreamDefaultReader'");
    if (reader != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, reader, new_target);
    }
    return embi_bind_leave(sc, reader);
}

embi_js_value embi_impl_ReadableStreamDefaultReader_read(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_obj *bo = reader_stream(self);
    if (bo == nullptr) {
        return ldr_promise_type_error(c, "This readable stream reader has been released and cannot be used to read from its previous owner stream");
    }
    return stream_read(c, bo);
}

embi_dom_err embi_impl_ReadableStreamDefaultReader_releaseLock(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_obj *bo = reader_stream(self);
    if (bo != nullptr) {
        release_reader(c, self, bo);
    }
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_ReadableStreamDefaultReader_cancel(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_bind_obj *bo = reader_stream(self);
    if (bo == nullptr) {
        return ldr_promise_type_error(c, "This readable stream reader has been released and cannot be used to cancel its previous owner stream");
    }
    return stream_cancel(c, bo, a0);
}

embi_js_value embi_impl_ReadableStreamDefaultReader_get_closed(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self) || self->owner == nullptr) {
        return ldr_promise_type_error(c, "Illegal invocation");
    }
    if ((self->aux & RD_RELEASED) != 0) {
        embi_js_value p = ldr_promise_type_error(c, "Reader was released");
        mark_handled(c, p);
        return p;
    }
    return slot(c, self->owner, RS_CLOSED);
}

/* ------------------------------------------------------------------ stream members */

bool embi_impl_ReadableStream_get_locked(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return !embi_bind_stale(self) && rs_of(self)->s.locked;
}

embi_js_value embi_impl_ReadableStream_cancel(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    if (embi_bind_stale(self)) {
        return ldr_promise_resolve(c, EMBI_JS_UNDEFINED);
    }
    if (rs_of(self)->s.locked) {
        return ldr_promise_type_error(c, "Failed to execute 'cancel' on 'ReadableStream': Cannot cancel a locked stream");
    }
    return stream_cancel(c, self, a0);
}

embi_dom_err embi_impl_ReadableStream_getReader(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value *out)
{
    if (embi_bind_stale(self)) {
        *out = EMBI_JS_UNDEFINED;
        return EMBI_DOM_OK;
    }
    embi_js_value mode = ldr_get(c, a0, "mode");
    if (mode == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    if (!embi_js_is_undefined(mode)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'getReader' on 'ReadableStream': BYOB readers are not supported.");
        return EMBI_BIND_THREW;
    }
    *out = acquire_reader(c, self, 0, "Failed to execute 'getReader' on 'ReadableStream'");
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_ReadableStream_values(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value *out)
{
    if (embi_bind_stale(self)) {
        *out = EMBI_JS_UNDEFINED;
        return EMBI_DOM_OK;
    }
    embi_js_value prevent = ldr_get(c, a0, "preventCancel");
    if (prevent == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    embi_js_value reader = acquire_reader(c, self, RD_AUTO, "Failed to execute 'values' on 'ReadableStream'");
    if (reader == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    embi_js_value it = embi_bind_other_value(c, EMBI_IF_ReadableStreamAsyncIterator, c, nullptr, 0);
    embi_bind_obj *ib = embi_bind_obj_of(it);
    ib->aux = embi_js_to_boolean(prevent) ? IT_PREVENT_CANCEL : 0;
    embi_bind_set_ref(ib, reader);
    *out = it;
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ async iterator */

embi_js_value embi_impl_ReadableStreamAsyncIterator_next(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_obj *rb = embi_bind_obj_of(self->ref);
    embi_bind_obj *bo = rb != nullptr ? reader_stream(rb) : nullptr;
    if (bo == nullptr) {
        return ldr_promise_resolve(c, ldr_iter_result(c, EMBI_JS_UNDEFINED, true));
    }
    return stream_read(c, bo);
}

embi_js_value embi_impl_ReadableStreamAsyncIterator_return(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_bind_obj *rb = embi_bind_obj_of(self->ref);
    embi_bind_obj *bo = rb != nullptr ? reader_stream(rb) : nullptr;
    if (bo != nullptr) {
        if ((self->aux & IT_PREVENT_CANCEL) == 0) {
            embi_js_value p = stream_cancel(c, bo, a0);
            mark_handled(c, p);
        }
        if (reader_stream(rb) != nullptr) {
            release_reader(c, rb, bo);
        }
    }
    return ldr_promise_resolve(c, ldr_iter_result(c, a0, true));
}

/* ------------------------------------------------------------------ controller */

embi_js_value embi_impl_ReadableStreamDefaultController_get_desiredSize(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    if (embi_bind_stale(self)) {
        return EMBI_JS_NULL;
    }
    double d;
    return embi_stream_desired_size(&rs_of(self->owner)->s, &d) ? embi_js_number(d) : EMBI_JS_NULL;
}

embi_dom_err embi_impl_ReadableStreamDefaultController_close(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_bind_obj *bo = self->owner;
    switch (embi_stream_close(&rs_of(bo)->s)) {
    case EMBI_STREAM_CLOSE_INVALID:
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'close' on 'ReadableStreamDefaultController': The stream is not in a state that permits close");
        return EMBI_BIND_THREW;
    case EMBI_STREAM_CLOSE_NOW:
        on_closed(c, bo);
        break;
    case EMBI_STREAM_CLOSE_DEFERRED:
        break;
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_ReadableStreamDefaultController_enqueue(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_bind_obj *bo = self->owner;
    rs_rec *r = rs_of(bo);
    if (!embi_stream_can_enqueue(&r->s)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'enqueue' on 'ReadableStreamDefaultController': Cannot enqueue a chunk into a readable stream that is closed or has been requested to be closed");
        return EMBI_BIND_THREW;
    }
    double size = 1.0;
    embi_js_value sizefn = slot(c, bo, RS_SIZEFN);
    if (embi_js_is_callable(sizefn)) {
        embi_js_value res = embi_js_call(c->rt, sizefn, EMBI_JS_UNDEFINED, 1, &a0);
        bool ok = res != EMBI_JS_EXCEPTION && embi_js_to_number(c->rt, res, &size);
        if (ok && (size != size || size < 0 || isinf(size))) {
            (void)embi_js_throw_range_error(c->rt, "Failed to execute 'enqueue' on 'ReadableStreamDefaultController': Invalid chunk size");
            ok = false;
        }
        if (!ok) {
            embi_js_value ex = embi_js_exception_get(c->rt);
            embi_js_exception_clear(c->rt);
            error_stream(c, bo, ex);
            (void)embi_js_throw_value(c->rt, ex);
            return EMBI_BIND_THREW;
        }
    }
    switch (embi_stream_enqueue(&r->s, size)) {
    case EMBI_STREAM_ENQ_DELIVERED:
        settle_read(c, reads_pop(c, bo), a0, false);
        break;
    case EMBI_STREAM_ENQ_QUEUED: {
        embi_js_value q = slot(c, bo, RS_QUEUE);
        uint32_t n = embi_bind_array_len(c, q);
        embi_bind_array_set(c, q, n, a0);
        embi_bind_array_set(c, q, n + 1u, embi_js_number(size));
        break;
    }
    case EMBI_STREAM_ENQ_INVALID:
        break;
    }
    call_pull_if_needed(c, bo);
    return EMBI_DOM_OK;
}

void embi_impl_ReadableStreamDefaultController_error(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    if (!embi_bind_stale(self)) {
        error_stream(c, self->owner, a0);
    }
}

/* ------------------------------------------------------------------ reading a whole stream (bodies) */

/* The drain state is a JS array: [result promise, reader wrapper, chunks array]. */
enum { DR_PROMISE, DR_READER, DR_CHUNKS };

static void drain_step(embi_bind_ctx *c, uint32_t pid);

static void drain_fail(embi_bind_ctx *c, uint32_t pid, embi_js_value d, embi_js_value reason)
{
    embi_js_value p = embi_bind_array_get(c, d, DR_PROMISE);
    embi_js_persistent_free(c->rt, pid);
    (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(p), reason);
}

static embi_js_value drain_ok(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    uint32_t pid = (uint32_t)magic;
    embi_js_value d = embi_js_persistent_get(c->rt, pid);
    embi_js_value res = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_js_value done = ldr_get(c, res, "done");
    embi_js_value chunks = embi_bind_array_get(c, d, DR_CHUNKS);
    if (done != EMBI_JS_EXCEPTION && embi_js_to_boolean(done)) {
        size_t total = 0;
        uint32_t n = embi_bind_array_len(c, chunks);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *p;
            size_t len;
            (void)ldr_buffer_source(embi_bind_array_get(c, chunks, i), &p, &len, nullptr);
            total += len;
        }
        embi_js_value out = ldr_new_uint8(c, nullptr, total);
        if (out != EMBI_JS_EXCEPTION && total > 0) {
            uint8_t *w = embi_js_view_base(embi_js_view_of(embi_js_as_object(out)));
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *p;
                size_t len;
                (void)ldr_buffer_source(embi_bind_array_get(c, chunks, i), &p, &len, nullptr);
                if (len > 0) {
                    memcpy(w, p, len);
                    w += len;
                }
            }
        }
        embi_js_value promise = embi_bind_array_get(c, d, DR_PROMISE);
        embi_js_persistent_free(c->rt, pid);
        if (out == EMBI_JS_EXCEPTION) {
            embi_js_value ex = embi_js_exception_get(c->rt);
            embi_js_exception_clear(c->rt);
            (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(promise), ex);
        } else {
            (void)embi_js_promise_resolve_with(c->rt, embi_js_as_object(promise), out);
        }
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value value = ldr_get(c, res, "value");
    const uint8_t *p;
    size_t len;
    if (value == EMBI_JS_EXCEPTION || !ldr_buffer_source(value, &p, &len, nullptr)) {
        if (value == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
        }
        drain_fail(c, pid, d, ldr_make_type_error(c, "Failed to read the body: a chunk is not a Uint8Array"));
        return EMBI_JS_UNDEFINED;
    }
    embi_bind_array_set(c, chunks, embi_bind_array_len(c, chunks), value);
    drain_step(c, pid);
    return EMBI_JS_UNDEFINED;
}

static embi_js_value drain_err(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value d = embi_js_persistent_get(c->rt, (uint32_t)magic);
    drain_fail(c, (uint32_t)magic, d, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

static void drain_step(embi_bind_ctx *c, uint32_t pid)
{
    embi_js_value d = embi_js_persistent_get(c->rt, pid);
    embi_bind_obj *rb = embi_bind_obj_of(embi_bind_array_get(c, d, DR_READER));
    embi_bind_obj *bo = rb != nullptr ? reader_stream(rb) : nullptr;
    if (bo == nullptr) {
        drain_fail(c, pid, d, ldr_make_type_error(c, "The body stream was released"));
        return;
    }
    embi_js_value p = stream_read(c, bo);
    embi_js_value f_ok = ldr_native(c, "", 1, drain_ok, (int)pid);
    embi_js_value f_err = ldr_native(c, "", 1, drain_err, (int)pid);
    (void)embi_js_promise_then(c->rt, embi_js_as_object(p), f_ok, f_err, nullptr);
}

embi_js_value ldr_stream_read_all(embi_bind_ctx *c, embi_js_value stream)
{
    embi_bind_obj *bo = stream_bo(stream);
    if (bo == nullptr) {
        return ldr_promise_type_error(c, "The body is not a ReadableStream");
    }
    embi_js_value reader = acquire_reader(c, bo, 0, "Failed to read the body");
    if (reader == EMBI_JS_EXCEPTION) {
        return ldr_promise_reject_pending(c);
    }
    embi_js_object *po = embi_js_promise_new(c->rt);
    embi_js_value p = embi_js_from_object(po);
    embi_js_value d = embi_bind_array_new(c, 3);
    embi_bind_array_set(c, d, DR_PROMISE, p);
    embi_bind_array_set(c, d, DR_READER, reader);
    embi_bind_array_set(c, d, DR_CHUNKS, embi_bind_array_new(c, 0));
    uint32_t pid = embi_js_persistent_new(c->rt, d);
    drain_step(c, pid);
    return p;
}
