/*
 * fetch() (fetch.webidl, W7c; Fetch Standard minus a network). The operation is src/loader/fetch.c (redirects, modes, abort);
 * the hops go through the environment's transport (loader_host.h), which the embedding layer binds to the runtime's resource
 * handlers and the view's event queue, so the operation's completion callback runs on the context's thread.
 *
 * A fetch in flight is a job: the operation, and a persistent JS array [promise, signal] that keeps the promise alive. An
 * AbortSignal is observed through a one-shot "abort" listener whose function carries the job id; when the listener runs
 * the job is looked up again, so a listener that outlives its fetch does nothing. blob: URLs are answered from the blob
 * registry, and data: URLs from the data URL decoder when the environment has no transport. A finished operation is freed
 * in a microtask, never inside its own completion callback.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/window.h"

struct ldr_job {
    ldr_job *next, *prev;
    ldr_state *st;
    embi_bind_ctx *c;
    uint32_t id;
    embi_fetch_op *op;
    uint32_t pid; /* persistent: [promise, signal, abort listener, body stream]; 0 once released */
    bool has_pid;
    bool finished;
    bool body_open; /* a network body is still arriving through the sink (the record holds its stream) */
    size_t queued;  /* body bytes queued in the stream, not yet read by the reader */
    bool held;      /* the network is paused for this body (queued reached BODY_HOLD_BYTES, not yet BODY_RELEASE_BYTES) */
};

static void job_unlink(ldr_job *j)
{
    ldr_job **head = ldr_jobs_head(j->st);
    if (j->prev != nullptr) {
        j->prev->next = j->next;
    } else {
        *head = j->next;
    }
    if (j->next != nullptr) {
        j->next->prev = j->prev;
    }
    j->next = j->prev = nullptr;
}

static void job_destroy(ldr_job *j)
{
    if (j->has_pid) {
        embi_js_persistent_free(j->c->rt, j->pid);
        j->has_pid = false;
    }
    if (j->op != nullptr) {
        embi_fetch_op_free(j->op);
    }
    embi_free(j, sizeof *j);
}

static ldr_job *job_find(ldr_state *st, uint32_t id)
{
    for (ldr_job *j = *ldr_jobs_head(st); j != nullptr; j = j->next) {
        if (j->id == id) {
            return j;
        }
    }
    return nullptr;
}

void ldr_jobs_abort_all(ldr_state *st)
{
    ldr_job *j;
    while ((j = *ldr_jobs_head(st)) != nullptr) {
        job_unlink(j);
        job_destroy(j);
    }
}

/* Frees a finished job (queued as a microtask by the completion callback). */
static embi_js_value job_reap(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    if (c == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    ldr_state *st = ldr_state_of(c);
    ldr_job *j = job_find(st, (uint32_t)magic);
    if (j != nullptr) {
        job_unlink(j);
        job_destroy(j);
    }
    return EMBI_JS_UNDEFINED;
}

static void schedule_reap(embi_bind_ctx *c, ldr_job *j)
{
    embi_js_value f = ldr_native(c, "", 0, job_reap, (int)j->id);
    if (f != EMBI_JS_EXCEPTION) {
        embi_js_enqueue_microtask(c->rt, f, EMBI_JS_UNDEFINED);
    }
}

/* ------------------------------------------------------------------ completion */

/* Removes the job's abort listener from its signal (the record's third slot holds the listener), so a signal that is
 * reused for many fetches does not collect one closure per fetch. */
static void unwatch_signal(embi_bind_ctx *c, embi_js_value rec)
{
    embi_js_value signal = embi_bind_array_get(c, rec, 1);
    embi_js_value f = embi_bind_array_get(c, rec, 2);
    if (!embi_js_is_object(signal) || !embi_js_is_object(f)) {
        return;
    }
    embi_js_value args[2] = {embi_bind_utf8_value(c, EMBI_STR("abort")), f};
    embi_js_value r = embi_js_invoke(c->rt, signal, embi_js_key_c(c->rt, "removeEventListener"), 2, args);
    if (r == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    }
}

/* ------------------------------------------------------------------ streamed bodies (network responses) */

/* The stream of a job whose body is arriving, or undefined. */
static embi_js_value job_stream(ldr_job *j)
{
    embi_js_value rec = embi_js_persistent_get(j->c->rt, j->pid);
    return embi_bind_array_get(j->c, rec, 3);
}

static bool job_stream_open(ldr_job *j)
{
    embi_js_value st = job_stream(j);
    const char *state = ldr_stream_state_name(st);
    return state != nullptr && strcmp(state, "readable") == 0;
}

/* The body is over (ended, errored, cancelled or aborted): the abort listener goes, the record is released and the job is
 * reaped, so the operation (and with it the hop, if one is left) is freed. */
static void body_finish(ldr_job *j)
{
    if (!j->body_open) {
        return;
    }
    embi_bind_ctx *c = j->c;
    j->body_open = false;
    j->queued = 0;
    j->held = false;
    if (j->has_pid) {
        ldr_stream_set_hooks(job_stream(j), nullptr, nullptr, nullptr);
        unwatch_signal(c, embi_js_persistent_get(c->rt, j->pid));
        embi_js_persistent_free(c->rt, j->pid);
        j->has_pid = false;
    }
    schedule_reap(c, j);
}

/* Backpressure for a network body: the network is paused while more than BODY_HOLD_BYTES of it wait in the stream, and
 * resumed when the reader has taken it down to BODY_RELEASE_BYTES. */
enum { BODY_HOLD_BYTES = 1 << 20, BODY_RELEASE_BYTES = 256 << 10 };

static void body_on_read(void *user, size_t bytes)
{
    ldr_job *j = user;
    j->queued = bytes < j->queued ? j->queued - bytes : 0;
    if (j->held && j->queued <= BODY_RELEASE_BYTES) {
        j->held = false;
        embi_fetch_op_hold(j->op, false);
    }
}

/* The reader cancelled: the rest of the body is not wanted. */
static void body_on_cancel(void *user)
{
    ldr_job *j = user;
    embi_fetch_op_abort(j->op);
    body_finish(j);
}

static void sink_chunk(void *user, const uint8_t *p, size_t n)
{
    ldr_job *j = user;
    if (!j->body_open || !j->has_pid) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(j->c);
    if (job_stream_open(j)) {
        j->queued += ldr_stream_push_chunk(j->c, job_stream(j), p, n);
        if (!j->held && j->queued >= BODY_HOLD_BYTES) {
            j->held = true;
            embi_fetch_op_hold(j->op, true);
        }
    } else {
        /* The reader cancelled the body: the rest of it is not wanted. */
        embi_fetch_op_abort(j->op);
        body_finish(j);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void sink_end(void *user)
{
    ldr_job *j = user;
    if (!j->body_open || !j->has_pid) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(j->c);
    if (job_stream_open(j)) {
        ldr_stream_push_close(j->c, job_stream(j));
    }
    body_finish(j);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void sink_error(void *user, embi_str message)
{
    ldr_job *j = user;
    if (!j->body_open || !j->has_pid) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(j->c);
    if (job_stream_open(j)) {
        char msg[512];
        SDL_snprintf(msg, sizeof msg, "network error%s%.*s", message.len > 0 ? ": " : "", (int)message.len, message.ptr);
        ldr_stream_push_error(j->c, job_stream(j), ldr_make_type_error(j->c, msg));
    }
    body_finish(j);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static const embi_fetch_sink k_body_sink_proto = {nullptr, sink_chunk, sink_end, sink_error};

static void fetch_done(void *user, embi_fetch_op *op, bool ok)
{
    ldr_job *j = user;
    embi_bind_ctx *c = j->c;
    if (j->finished || !j->has_pid) {
        return;
    }
    j->finished = true;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value rec = embi_js_persistent_get(c->rt, j->pid);
    embi_js_value promise = embi_bind_array_get(c, rec, 0);
    embi_js_object *po = embi_js_as_object(promise);
    bool streaming = ok && embi_fetch_op_streaming(op);
    if (!streaming) {
        unwatch_signal(c, rec); /* a streamed body keeps the listener until its end (an abort then errors the body) */
    }
    bool body = false;
    if (ok) {
        embi_response r;
        embi_fetch_op_take(op, &r);
        body = streaming && r.body.present;
        if (body) {
            r.body.present = false; /* the stream is attached below */
        }
        embi_js_value resp = ldr_response_wrap(c, &r);
        if (resp == EMBI_JS_EXCEPTION) {
            embi_js_value ex = embi_js_exception_get(c->rt);
            embi_js_exception_clear(c->rt);
            (void)embi_js_promise_reject_with(c->rt, po, ex);
            body = false;
        } else {
            if (body) {
                embi_js_value st = ldr_stream_push_new(c);
                embi_bind_array_set(c, rec, 3, st);
                ldr_response_attach_stream(c, resp, st);
                ldr_stream_set_hooks(st, body_on_read, body_on_cancel, j);
            }
            (void)embi_js_promise_resolve_with(c->rt, po, resp);
        }
    } else {
        embi_str why = embi_fetch_op_error(op);
        char msg[160];
        SDL_snprintf(msg, sizeof msg, "Failed to fetch%s%.*s", why.len > 0 ? ": " : "", (int)why.len, why.ptr);
        (void)embi_js_promise_reject_with(c->rt, po, ldr_make_type_error(c, msg));
    }
    j->op = op; /* freed by the reaper, not here */
    if (body) {
        j->body_open = true; /* the record (and the job) live until the body is over */
    } else {
        embi_js_persistent_free(c->rt, j->pid);
        j->has_pid = false;
        /* A streamed response without a body (a null body status) is over: the reaper aborts what is left of the hop. */
        schedule_reap(c, j);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ abort */

static embi_js_value on_abort(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    if (c == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    ldr_state *st = ldr_state_of(c);
    ldr_job *j = job_find(st, (uint32_t)magic);
    if (j == nullptr || !j->has_pid) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value rec = embi_js_persistent_get(c->rt, j->pid);
    embi_js_value signal = embi_bind_array_get(c, rec, 1);
    if (j->finished) {
        /* The response is out and its body is arriving: the abort errors the body, and the operation stops. */
        if (j->body_open) {
            embi_js_value reason = ldr_get(c, signal, "reason");
            if (reason == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(c->rt);
                reason = EMBI_JS_UNDEFINED;
            }
            embi_fetch_op_abort(j->op);
            if (job_stream_open(j)) {
                ldr_stream_push_error(c, job_stream(j), reason);
            }
            body_finish(j);
        }
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value promise = embi_bind_array_get(c, rec, 0);
    embi_js_value reason = ldr_get(c, signal, "reason");
    if (reason == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        reason = EMBI_JS_UNDEFINED;
    }
    j->finished = true;
    embi_fetch_op *op = j->op;
    j->op = nullptr;
    if (op != nullptr) {
        embi_fetch_op_abort(op);
    }
    (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(promise), reason);
    embi_js_persistent_free(c->rt, j->pid);
    j->has_pid = false;
    j->op = op;
    schedule_reap(c, j);
    return EMBI_JS_UNDEFINED;
}

/* Registers the one-shot abort listener of a job on the signal. */
static void watch_signal(embi_bind_ctx *c, embi_js_value signal, ldr_job *j)
{
    embi_js_value f = ldr_native(c, "", 0, on_abort, (int)j->id);
    if (f == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return;
    }
    if (j->has_pid) {
        embi_bind_array_set(c, embi_js_persistent_get(c->rt, j->pid), 2, f); /* for unwatch_signal */
    }
    embi_js_value opts = embi_bind_object_new(c);
    embi_bind_object_set_c(c, opts, "once", EMBI_JS_TRUE);
    embi_js_value args[3] = {embi_bind_utf8_value(c, EMBI_STR("abort")), f, opts};
    embi_js_value r = embi_js_invoke(c->rt, signal, embi_js_key_c(c->rt, "addEventListener"), 3, args);
    if (r == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    }
}

/* ------------------------------------------------------------------ fetch */

static embi_str strip_fragment(embi_str url)
{
    for (uint32_t i = 0; i < url.len; i++) {
        if (url.ptr[i] == '#') {
            return (embi_str){url.ptr, i};
        }
    }
    return url;
}

static bool scheme_is(embi_str url, const char *name)
{
    size_t n = strlen(name);
    return url.len > n && url.ptr[n] == ':' && SDL_strncasecmp(url.ptr, name, n) == 0;
}

/* A data: URL answered by the loader's decoder (used when the environment has no transport). */
static bool data_response(embi_str url, embi_response *out)
{
    embi_sb mime = {nullptr, 0, 0}, body = {nullptr, 0, 0};
    if (!embi_data_url(url, &mime, &body)) {
        embi_sb_free(&mime);
        embi_sb_free(&body);
        return false;
    }
    *out = (embi_response){0};
    out->type = EMBI_RESP_BASIC;
    out->status = 200;
    embi_sb_append(&out->status_text, EMBI_STR("OK"));
    out->body.present = true;
    out->body.bytes = body;
    if (mime.len > 0) {
        embi_headers_add_raw(&out->headers, EMBI_STR("content-type"), (embi_str){mime.data, mime.len});
    }
    embi_sb_free(&mime);
    embi_sb_append(&out->url, url);
    return true;
}

static embi_js_value fetch_reject_type(embi_bind_ctx *c, const char *msg)
{
    return ldr_promise_type_error(c, msg);
}

embi_js_value embi_impl_Window_fetch(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    ldr_state *st = ldr_state_of(c);
    embi_js_value req = ldr_request_make(c, a0, a1);
    if (req == EMBI_JS_EXCEPTION) {
        return embi_bind_leave(sc, ldr_promise_reject_pending(c));
    }
    ldr_request_view rv = {0};
    if (!ldr_request_view_of(req, &rv)) {
        return embi_bind_leave(sc, fetch_reject_type(c, "Failed to fetch"));
    }
    embi_js_value result;
    embi_str url = strip_fragment(rv.url);
    bool aborted = false;
    embi_js_value reason = EMBI_JS_UNDEFINED;
    if (embi_js_is_object(rv.signal)) {
        embi_js_value ab = ldr_get(c, rv.signal, "aborted");
        if (ab != EMBI_JS_EXCEPTION && embi_js_to_boolean(ab)) {
            aborted = true;
            reason = ldr_get(c, rv.signal, "reason");
            if (reason == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(c->rt);
                reason = EMBI_JS_UNDEFINED;
            }
        } else if (ab == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
        }
    }
    if (aborted) {
        result = ldr_promise_reject(c, reason);
    } else if (rv.body_used) {
        result = fetch_reject_type(c, "Failed to execute 'fetch' on 'Window': Cannot construct a Request with a Request object that has already been used.");
    } else if (rv.body_from_stream) {
        result = fetch_reject_type(c, "Failed to execute 'fetch' on 'Window': Request bodies that are streams are not supported.");
    } else if (scheme_is(url, "blob") || (scheme_is(url, "data") && ldr_env(c)->transport.start == nullptr)) {
        embi_response r;
        bool get = embi_str_eq(rv.method, EMBI_STR("GET"));
        bool found = get && (scheme_is(url, "blob") ? ldr_blob_response(c, url, &r) : data_response(url, &r));
        if (!found) {
            result = fetch_reject_type(c, "Failed to fetch");
        } else {
            embi_js_value resp = ldr_response_wrap(c, &r);
            result = resp == EMBI_JS_EXCEPTION ? ldr_promise_reject_pending(c) : ldr_promise_resolve(c, resp);
        }
    } else if (ldr_env(c)->transport.start == nullptr) {
        result = fetch_reject_type(c, "Failed to fetch: no resource handler");
    } else {
        embi_js_object *po = embi_js_promise_new(c->rt);
        result = embi_js_from_object(po);
        ldr_job *j = embi_alloc(sizeof *j);
        *j = (ldr_job){0};
        j->st = st;
        j->c = c;
        j->id = ldr_next_job_id(st);
        embi_js_value rec = embi_bind_array_new(c, 4);
        embi_bind_array_set(c, rec, 0, result);
        embi_bind_array_set(c, rec, 1, rv.signal);
        j->pid = embi_js_persistent_new(c->rt, rec);
        j->has_pid = true;
        ldr_job **head = ldr_jobs_head(st);
        j->next = *head;
        if (*head != nullptr) {
            (*head)->prev = j;
        }
        *head = j;
        uint32_t n = rv.headers.list.len;
        embi_hdr_view *hv = n > 0 ? embi_alloc((size_t)n * sizeof *hv) : nullptr;
        for (uint32_t i = 0; i < n; i++) {
            const embi_kv *kv = &rv.headers.list.data[i];
            hv[i] = (embi_hdr_view){{kv->name.data, kv->name.len}, {kv->value.data, kv->value.len}};
        }
        bool has_body = rv.has_body;
        embi_fetch_init in = {0};
        in.method = rv.method;
        in.url = url;
        in.headers = hv;
        in.nheaders = n;
        in.body = has_body ? rv.body : nullptr;
        in.body_len = rv.body_len;
        in.redirect = rv.redirect;
        embi_fetch_sink sink = k_body_sink_proto;
        sink.user = j;
        in.creds = rv.creds;
        in.sink = &sink; /* the operation copies it; a streamed body goes to the job */
        const embi_loader_env *env = ldr_env(c);
        embi_fetch_op *op = embi_fetch_op_new(&env->transport, &in, fetch_done, j);
        if (n > 0) {
            embi_free(hv, (size_t)n * sizeof *hv);
        }
        if (op == nullptr) {
            /* The URL did not parse (cannot happen: the Request parsed it). */
            if (!j->finished) {
                job_unlink(j);
                embi_js_persistent_free(c->rt, j->pid);
                j->has_pid = false;
                embi_free(j, sizeof *j);
                (void)embi_js_promise_reject_with(c->rt, po, ldr_make_type_error(c, "Failed to fetch"));
            }
        } else {
            if (j->op == nullptr) {
                j->op = op;
            }
            if (!j->finished && embi_js_is_object(rv.signal)) {
                watch_signal(c, rv.signal, j);
            }
        }
    }
    ldr_request_mark_used(req);
    embi_headers_free(&rv.headers);
    return embi_bind_leave(sc, result);
}
