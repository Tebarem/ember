/*
 * crypto.getRandomValues and crypto.randomUUID (crypto.webidl, W7c). The random source is the loader's ChaCha20
 * generator (src/loader/random.h), one per thread, seeded on first use. It is documented as not for key generation.
 */
#include "bindings/impl_loader.h"
#include "bindings/gen/crypto.h"
#include "bindings/gen/window.h"

#include "js/typedarray.h"
#include "loader/random.h"

#include <string.h>

/* The quota of one getRandomValues call (Web Crypto: 65536 bytes). */
#define CRYPTO_MAX_BYTES 65536u

#define g_rng (*ldr_rng())

/* True for the integer element kinds getRandomValues accepts (not the float ones). */
static bool integer_kind(enum embi_js_ta_kind k)
{
    switch (k) {
    case EMBI_JS_TA_INT8:
    case EMBI_JS_TA_UINT8:
    case EMBI_JS_TA_UINT8C:
    case EMBI_JS_TA_INT16:
    case EMBI_JS_TA_UINT16:
    case EMBI_JS_TA_INT32:
    case EMBI_JS_TA_UINT32:
    case EMBI_JS_TA_BIGINT64:
    case EMBI_JS_TA_BIGUINT64:
        return true;
    default:
        return false;
    }
}

embi_dom_err embi_impl_Crypto_getRandomValues(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                              embi_js_value *out)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    if (!embi_js_is_object(a0) || !embi_js_is_typedarray_obj(embi_js_as_object(a0))) {
        (void)embi_bind_throw_named(c, "TypeMismatchError",
                                    "Failed to execute 'getRandomValues' on 'Crypto': The provided ArrayBufferView is not an integer array type.");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    embi_js_varray *vw = embi_js_view_of(embi_js_as_object(a0));
    enum embi_js_ta_kind kind = (enum embi_js_ta_kind)embi_js_bin_num(vw->elems[EMBI_JS_VIEW_KIND]);
    if (!integer_kind(kind)) {
        (void)embi_bind_throw_named(c, "TypeMismatchError",
                                    "Failed to execute 'getRandomValues' on 'Crypto': The provided ArrayBufferView is not an integer array type.");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    {
        /* A detached buffer, or a view that runs out of its buffer, is a TypeError before anything is written. */
        const uint8_t *bp;
        size_t bn;
        bool detached = false;
        (void)ldr_buffer_source(a0, &bp, &bn, &detached);
        size_t probe = 0;
        if (detached || !embi_js_view_len(vw, embi_js_bin_size(kind), &probe)) {
            (void)embi_js_throw_type_error(c->rt, "Failed to execute 'getRandomValues' on 'Crypto': The ArrayBufferView's buffer is detached or out of bounds.");
            (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return EMBI_BIND_THREW;
        }
    }
    size_t esz = embi_js_bin_size(kind);
    size_t count = 0;
    if (!embi_js_view_len(vw, esz, &count)) {
        count = 0;
    }
    size_t bytes = count * esz;
    if (bytes > CRYPTO_MAX_BYTES) {
        (void)embi_bind_throw_named(c, "QuotaExceededError",
                                    "Failed to execute 'getRandomValues' on 'Crypto': The ArrayBufferView's byte length exceeds the number of bytes of entropy available via this API (65536).");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    if (bytes > 0) {
        embi_rng_bytes(&g_rng, embi_js_view_base(vw), bytes);
    }
    *out = a0;
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return EMBI_DOM_OK;
}

embi_dstr embi_impl_Crypto_randomUUID(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    char uuid[37];
    embi_rng_uuid(&g_rng, uuid);
    return embi_bind_tmp_utf8(c, (embi_str){uuid, 36});
}

embi_js_value embi_impl_Window_get_crypto(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_Crypto, c, nullptr, 0);
    return embi_bind_leave(sc, v);
}
