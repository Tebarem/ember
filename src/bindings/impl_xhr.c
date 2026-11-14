/*
 * XMLHttpRequest, XMLHttpRequestUpload, XMLHttpRequestEventTarget and ProgressEvent (N1b). The interfaces are
 * written in JS, like the Streams classes (impl_loader_prelude.c): the source is src/bindings/js/xhr.js, embedded at build time
 * and evaluated once per realm, the first time one of the names is read. The names are global accessors until then, and a read
 * replaces the accessor by an ordinary data property, the way an interface object is. The transport is fetch, so an app://
 * handler and a network request (http and https) take the same path, and a streamed body reports progress as it arrives.
 *
 * Synchronous requests are refused (open with async false throws InvalidAccessError and logs why), and responseType "document"
 * throws NotSupportedError. The host object the source gets holds the globals it builds on (captured at evaluation).
 */
#include "bindings/impl_loader.h"

#ifdef EMBER_LZ_ASSETS
#include "ember_lz/js/xhr.js.lz.h"
#define k_xhr_js embi_lz_js_xhr_js
#else
static const unsigned char k_xhr_js[] = {
#embed "js/xhr.js"
    , 0
};
#endif

/* The globals the source defines, in the order of the lazy accessors' magic numbers. */
static const char *const k_xhr_names[] = {"XMLHttpRequest", "XMLHttpRequestUpload", "XMLHttpRequestEventTarget",
                                          "ProgressEvent"};
#define XHR_N ((int)(sizeof k_xhr_names / sizeof k_xhr_names[0]))

/* The globals the source builds on. Their data values are captured when the lazy accessors are installed (ldr_xhr_install_lazy),
 * so a later replacement of one of them by the page does not change XMLHttpRequest. A global that is still an accessor at
 * that time (installed lazily by another module) is read at the first use instead. */
static const char *const k_xhr_imports[] = {"EventTarget",  "Event",   "DOMException",  "AbortController", "fetch",
                                            "TextDecoder",  "TextEncoder", "Blob",    "FormData",        "URLSearchParams",
                                            "setTimeout",   "clearTimeout", "console"};
#define XHR_IMPORTS ((int)(sizeof k_xhr_imports / sizeof k_xhr_imports[0]))

/* True when o has an own data property name; its value in out. */
static bool own_data_get(embi_bind_ctx *c, embi_js_object *o, const char *name, embi_js_value *out)
{
    embi_js_pdesc d;
    if (embi_js_obj_get_own_property(c->rt, o, embi_js_key_c(c->rt, name), &d) != EMBI_JS_YES) {
        return false;
    }
    if (embi_js_pd_is_accessor(&d) || (d.has & EMBI_JS_PD_VALUE) == 0) {
        return false;
    }
    *out = d.value;
    return true;
}

/* The host object: the globals the source needs, as of now (creates it on first call). */
static embi_js_value xhr_host(embi_bind_ctx *c)
{
    uint32_t *hs = ldr_xhr_host_slot(c);
    if (*hs == 0) {
        /* Rooted by its persistent handle at once (no scope is needed here: install runs outside one). */
        embi_js_value host = embi_bind_object_new(c);
        *hs = embi_js_persistent_new(c->rt, host) + 1u;
        for (int i = 0; i < XHR_IMPORTS; i++) {
            embi_js_value v;
            if (own_data_get(c, c->realm->global, k_xhr_imports[i], &v)) {
                embi_bind_object_set_c(c, host, k_xhr_imports[i], v);
            }
        }
    }
    return embi_js_persistent_get(c->rt, *hs - 1u);
}

/* The exports object of the source, evaluated on first use. EMBI_JS_EXCEPTION with the exception pending on failure. */
static embi_js_value xhr_exports(embi_bind_ctx *c)
{
    uint32_t *slot = ldr_xhr_slot(c);
    if (*slot != 0) {
        return embi_js_persistent_get(c->rt, *slot - 1u);
    }
    embi_js_value host = xhr_host(c);
    embi_js_value global = embi_js_from_object(c->realm->global);
    for (int i = 0; i < XHR_IMPORTS; i++) {
        embi_js_value have;
        if (own_data_get(c, embi_js_as_object(host), k_xhr_imports[i], &have)) {
            continue;
        }
        embi_js_value v = ldr_get(c, global, k_xhr_imports[i]);
        if (v == EMBI_JS_EXCEPTION) {
            return v;
        }
        embi_bind_object_set_c(c, host, k_xhr_imports[i], v);
    }
    embi_js_value fn = embi_bind_eval_asset(c, k_xhr_js, sizeof k_xhr_js, "xhr.js");
    if (fn == EMBI_JS_EXCEPTION) {
        return fn;
    }
    embi_js_value ex = embi_js_call(c->rt, fn, EMBI_JS_UNDEFINED, 1, &host);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    *slot = embi_js_persistent_new(c->rt, ex) + 1u;
    return ex;
}

static void xhr_define_data(embi_bind_ctx *c, const char *name, embi_js_value v)
{
    embi_js_key k = embi_js_key_c(c->rt, name);
    (void)embi_js_obj_delete(c->rt, c->realm->global, k);
    embi_js_define_raw(c->rt, c->realm->global, k, v, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

static embi_js_value xhr_lazy_get(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_value ex = xhr_exports(c);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    embi_js_value v = ldr_get(c, ex, k_xhr_names[magic]);
    if (v == EMBI_JS_EXCEPTION) {
        return v;
    }
    xhr_define_data(c, k_xhr_names[magic], v);
    return v;
}

static embi_js_value xhr_lazy_set(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    xhr_define_data(c, k_xhr_names[magic], argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

void ldr_xhr_install_lazy(embi_bind_ctx *c)
{
    (void)xhr_host(c); /* captures the globals now */
    for (int i = 0; i < XHR_N; i++) {
        embi_js_define_getset(c->realm, c->realm->global, k_xhr_names[i], xhr_lazy_get, xhr_lazy_set, i,
                              EMBI_JS_ATTR_CONFIGURABLE);
    }
}

void ldr_xhr_release(embi_bind_ctx *c, uint32_t slot)
{
    if (slot != 0) {
        embi_js_persistent_free(c->rt, slot - 1u);
    }
}
