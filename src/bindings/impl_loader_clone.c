/*
 * structuredClone (storage.webidl, W7c, HTML 2.7.4). The serializer and the deserializer are the loader's
 * (src/loader/clone.h): the value is serialized, the transfer list is detached, and the bytes are read back as a
 * new value of the realm. A DataCloneError or a deserialization error stays pending on the runtime.
 *
 * The serializer throws a plain Error named DataCloneError (the loader has no DOM); this layer turns it into the
 * DOMException of the standard (name DataCloneError, code 25). Blob and File are serializable platform objects: the
 * host hooks below write their bytes, type and, for a File, name and lastModified.
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"
#include "bindings/gen/window.h"

#include "loader/clone.h"

/* Blob and File payload: u8 is_file, u32 type_len, u32 name_len, f64 last_modified, u64 size, type, name, bytes. */
static void put(embi_sb *out, const void *p, size_t n)
{
    if (n > 0) {
        embi_sb_append(out, (embi_str){(const char *)p, (uint32_t)n});
    }
}

static int host_serialize(embi_js_realm *r, embi_js_value v, embi_sb *payload)
{
    (void)r;
    const uint8_t *p = nullptr;
    size_t n = 0;
    embi_str type = {nullptr, 0};
    if (!ldr_blob_view(v, &p, &n, &type)) {
        return 0;
    }
    embi_str name = {nullptr, 0};
    double lm = 0;
    uint8_t is_file = ldr_file_info(v, &name, &lm) ? 1 : 0;
    if (n > UINT32_MAX - 64u) {
        return 0;
    }
    uint32_t tl = type.len, nl = is_file ? name.len : 0;
    uint64_t size = n;
    put(payload, &is_file, 1);
    put(payload, &tl, 4);
    put(payload, &nl, 4);
    put(payload, &lm, 8);
    put(payload, &size, 8);
    put(payload, type.ptr, tl);
    put(payload, name.ptr, nl);
    put(payload, p, n);
    return 1;
}

static bool host_deserialize(embi_js_realm *rl, embi_str payload, embi_js_value *out)
{
    const char *q = payload.ptr;
    size_t left = payload.len;
    uint8_t is_file = 0;
    uint32_t tl = 0, nl = 0;
    double lm = 0;
    uint64_t size = 0;
    if (left < 25) {
        return false;
    }
    memcpy(&is_file, q, 1);
    memcpy(&tl, q + 1, 4);
    memcpy(&nl, q + 5, 4);
    memcpy(&lm, q + 9, 8);
    memcpy(&size, q + 17, 8);
    q += 25;
    left -= 25;
    if (tl > left || nl > left - tl || size != left - tl - nl) {
        return false;
    }
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_str type = {q, tl}, name = {q + tl, nl};
    const uint8_t *bytes = (const uint8_t *)q + tl + nl;
    *out = is_file ? ldr_file_new(c, bytes, (size_t)size, type, name, lm) : ldr_blob_new(c, bytes, (size_t)size, type);
    return *out != EMBI_JS_EXCEPTION;
}

static const embi_clone_host k_host = {host_serialize, host_deserialize, ldr_xfer_prepare, ldr_xfer_commit,
                                       ldr_xfer_rollback, ldr_xfer_receive};

void ldr_clone_install(void)
{
    embi_clone_set_host(&k_host);
}

/* Replaces the pending plain DataCloneError of the serializer by the DOMException of that name. */
void ldr_clone_rethrow_dom(embi_bind_ctx *c)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return;
    }
    embi_js_value name = ldr_get(c, ex, "name");
    embi_js_value msg = name == EMBI_JS_EXCEPTION ? name : ldr_get(c, ex, "message");
    if (name == EMBI_JS_EXCEPTION || msg == EMBI_JS_EXCEPTION) {
        return;
    }
    embi_sb n = {nullptr, 0, 0}, m = {nullptr, 0, 0};
    if (embi_js_is_string(name) && embi_js_is_string(msg) && ldr_to_utf8(c, name, &n) && ldr_to_utf8(c, msg, &m) &&
        n.len == 14 && memcmp(n.data, "DataCloneError", 14) == 0) {
        embi_js_exception_clear(rt);
        embi_dstr nd = {n.data, n.len, false}, md = {m.data, m.len, false};
        (void)embi_js_throw_value(rt, embi_bind_dom_exception(c, 25, nd, md));
    }
    embi_sb_free(&n);
    embi_sb_free(&m);
}

embi_dom_err embi_impl_Window_structuredClone(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                              embi_dict_StructuredSerializeOptions a1, embi_js_value *out)
{
    (void)self;
    ldr_clone_install();
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_realm *r = embi_bind_realm(c);
    embi_sb bytes = {nullptr, 0, 0};
    const embi_js_value *transfer = (const embi_js_value *)a1.transfer.data;
    if (!embi_clone_serialize(r, a0, transfer, a1.transfer.len, &bytes)) {
        embi_sb_free(&bytes);
        ldr_clone_rethrow_dom(c);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_BIND_THREW;
    }
    embi_js_value v = EMBI_JS_UNDEFINED;
    bool ok = embi_clone_deserialize(r, (embi_str){bytes.data, bytes.len}, &v);
    embi_sb_free(&bytes);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    if (!ok) {
        return EMBI_BIND_THREW;
    }
    *out = v;
    return EMBI_DOM_OK;
}
