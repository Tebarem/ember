/*
 * The JS-written part of the Streams API (W7f): WritableStream, TransformStream, TextEncoderStream, TextDecoderStream, the
 * pipes and the transfer of a ReadableStream through a MessagePort. The source is src/bindings/js/streams.js, embedded
 * at build time and evaluated once per realm, the first time any of it is used. The names are global accessors until then
 * (installed when the context is made): the first read evaluates the source and replaces the accessor by an ordinary data
 * property, the way an interface object is; an assignment replaces it as well.
 *
 * The exports are kept in a persistent handle of the loader state. The source gets a small host object: the interfaces it
 * builds on (captured when it is evaluated) and a few native helpers that read the state of a native ReadableStream.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"

#ifdef EMBER_LZ_ASSETS
#include "ember_lz/js/streams.js.lz.h"
#define k_streams_js embi_lz_js_streams_js
#else
static const unsigned char k_streams_js[] = {
#embed "js/streams.js"
    , 0
};
#endif

/* The globals defined by the source, in the order of the lazy accessors' magic numbers. */
static const char *const k_names[] = {"WritableStream",
                                      "WritableStreamDefaultWriter",
                                      "WritableStreamDefaultController",
                                      "TransformStream",
                                      "TransformStreamDefaultController",
                                      "TextEncoderStream",
                                      "TextDecoderStream"};
#define N_NAMES ((int)(sizeof k_names / sizeof k_names[0]))

/* ------------------------------------------------------------------ host helpers */

enum { H_STATE, H_ERROR, H_SHOULD_PULL, H_CAN_ENQUEUE, H_DISTURB, H_IS_RS };

static embi_js_value host_native(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_stream *s = ldr_stream_core(v);
    if (s == nullptr) {
        if (magic == H_IS_RS) {
            return EMBI_JS_FALSE;
        }
        return embi_js_throw_type_error(c->rt, "Illegal invocation: not a ReadableStream");
    }
    switch (magic) {
    case H_STATE: {
        const char *n = ldr_stream_state_name(v);
        return embi_bind_utf8_value(c, (embi_str){n, (uint32_t)SDL_strlen(n)});
    }
    case H_ERROR:
        return ldr_stream_stored_error(c, v);
    case H_SHOULD_PULL:
        return embi_js_from_bool(embi_stream_should_pull(s));
    case H_CAN_ENQUEUE:
        return embi_js_from_bool(embi_stream_can_enqueue(s));
    case H_DISTURB:
        s->disturbed = true;
        return EMBI_JS_UNDEFINED;
    default:
        return EMBI_JS_TRUE;
    }
}

static void host_set(embi_bind_ctx *c, embi_js_value host, const char *name, embi_js_native fn, int magic, int length)
{
    embi_js_value f = ldr_native(c, name, length, fn, magic);
    if (f != EMBI_JS_EXCEPTION) {
        embi_bind_object_set_c(c, host, name, f);
    }
}

/* ------------------------------------------------------------------ loading */

/* The exports object of the source, evaluating it first when needed. EMBI_JS_EXCEPTION with the exception pending. */
static embi_js_value prelude_exports(embi_bind_ctx *c)
{
    uint32_t *slot = ldr_prelude_slot(c);
    if (*slot != 0) {
        return embi_js_persistent_get(c->rt, *slot - 1u);
    }
    embi_js_runtime *rt = c->rt;
    embi_js_value global = embi_js_from_object(c->realm->global);
    embi_js_value host = embi_bind_pin(c, embi_bind_object_new(c));
    static const char *const imports[] = {"ReadableStream", "TextEncoder", "TextDecoder", "AbortSignal",
                                          "AbortController", "DOMException", "MessageChannel"};
    for (size_t i = 0; i < sizeof imports / sizeof imports[0]; i++) {
        embi_js_value v = ldr_get(c, global, imports[i]);
        if (v == EMBI_JS_EXCEPTION) {
            return v;
        }
        embi_bind_object_set_c(c, host, imports[i], v);
    }
    host_set(c, host, "rsState", host_native, H_STATE, 1);
    host_set(c, host, "rsError", host_native, H_ERROR, 1);
    host_set(c, host, "rsShouldPull", host_native, H_SHOULD_PULL, 1);
    host_set(c, host, "rsCanEnqueue", host_native, H_CAN_ENQUEUE, 1);
    host_set(c, host, "disturb", host_native, H_DISTURB, 1);
    host_set(c, host, "isReadableStream", host_native, H_IS_RS, 1);
    embi_js_value fn = embi_bind_eval_asset(c, k_streams_js, sizeof k_streams_js, "streams.js");
    if (fn == EMBI_JS_EXCEPTION) {
        return fn;
    }
    embi_js_value ex = embi_js_call(rt, fn, EMBI_JS_UNDEFINED, 1, &host);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    *slot = embi_js_persistent_new(rt, ex) + 1u;
    return ex;
}

embi_js_value ldr_prelude_get(embi_bind_ctx *c, const char *name)
{
    embi_js_value ex = prelude_exports(c);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    return ldr_get(c, ex, name);
}

embi_js_value ldr_prelude_call(embi_bind_ctx *c, const char *name, int argc, const embi_js_value *argv)
{
    embi_js_value fn = ldr_prelude_get(c, name);
    if (fn == EMBI_JS_EXCEPTION) {
        return fn;
    }
    return embi_js_call(c->rt, fn, EMBI_JS_UNDEFINED, argc, argv);
}

/* ------------------------------------------------------------------ lazy globals */

static void define_data(embi_bind_ctx *c, const char *name, embi_js_value v)
{
    embi_js_runtime *rt = c->rt;
    /* Replaces the accessor: delete it, then define the data property (writable, configurable, not enumerable). */
    embi_js_key k = embi_js_key_c(rt, name);
    (void)embi_js_obj_delete(rt, c->realm->global, k);
    embi_js_define_raw(rt, c->realm->global, k, v, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

static embi_js_value lazy_get(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_value v = ldr_prelude_get(c, k_names[magic]);
    if (v == EMBI_JS_EXCEPTION) {
        return v;
    }
    define_data(c, k_names[magic], v);
    return v;
}

static embi_js_value lazy_set(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    define_data(c, k_names[magic], argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

void ldr_install_lazy_globals(embi_bind_ctx *c)
{
    for (int i = 0; i < N_NAMES; i++) {
        embi_js_define_getset(c->realm, c->realm->global, k_names[i], lazy_get, lazy_set, i, EMBI_JS_ATTR_CONFIGURABLE);
    }
    ldr_xhr_install_lazy(c); /* XMLHttpRequest and ProgressEvent (impl_xhr.c) */
}

/* ------------------------------------------------------------------ ReadableStream.pipeTo, pipeThrough */

embi_js_value embi_impl_ReadableStream_pipeTo(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1)
{
    if (embi_bind_stale(self)) {
        return ldr_promise_type_error(c, "Illegal invocation");
    }
    embi_js_value args[3] = {embi_bind_obj_value(self), a0, a1};
    embi_js_value r = ldr_prelude_call(c, "pipeTo", 3, args);
    if (r == EMBI_JS_EXCEPTION) {
        return ldr_promise_reject_pending(c);
    }
    return r;
}

embi_dom_err embi_impl_ReadableStream_pipeThrough(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1,
                                                  embi_js_value *out)
{
    if (embi_bind_stale(self)) {
        *out = EMBI_JS_UNDEFINED;
        return EMBI_DOM_OK;
    }
    embi_js_value args[3] = {embi_bind_obj_value(self), a0, a1};
    embi_js_value r = ldr_prelude_call(c, "pipeThrough", 3, args);
    if (r == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    *out = r;
    return EMBI_DOM_OK;
}

void ldr_prelude_release(embi_bind_ctx *c, uint32_t slot)
{
    if (slot != 0) {
        embi_js_persistent_free(c->rt, slot - 1u);
    }
}
