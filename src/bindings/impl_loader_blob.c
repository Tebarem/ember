/*
 * Blob, File and URL.createObjectURL / revokeObjectURL (fetch.webidl, url.webidl; W7c). A Blob wrapper owns one flat block:
 * a header, then the media type, the file name (File only) and the bytes. Slices copy their window: a Blob is immutable, so
 * the copy is invisible. The blob: URL registry (src/loader/blob.h) keeps its own reference-counted copy of the blob, so
 * revoking the URL and dropping the wrapper are independent.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"
#include "bindings/gen/url.h"

#include <SDL3/SDL_time.h>

#include <math.h>

/* WebIDL ConvertToInt for `long long` (no [EnforceRange], no [Clamp]): NaN and the infinities become 0, others are
 * truncated toward zero and reduced modulo 2^64 into the signed range. */
static int64_t to_long_long(double x)
{
    if (!isfinite(x)) {
        return 0;
    }
    x = trunc(x);
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0) {
        return (int64_t)x;
    }
    double m = fmod(x, 18446744073709551616.0); /* exact, same sign as x */
    if (m < 0) {
        m += 18446744073709551616.0;
    }
    if (m >= 18446744073709551616.0) {
        m = 0; /* a tiny negative value rounded up */
    }
    return (int64_t)(uint64_t)m;
}

typedef struct blob_rec {
    size_t size;
    uint32_t type_len;
    uint32_t name_len;
    double last_modified;
    bool is_file;
    char tail[]; /* type, name, bytes */
} blob_rec;

static size_t rec_bytes(uint32_t type_len, uint32_t name_len, size_t size)
{
    return sizeof(blob_rec) + (size_t)type_len + name_len + size;
}

static const uint8_t *rec_data(const blob_rec *r)
{
    return (const uint8_t *)r->tail + r->type_len + r->name_len;
}

static embi_js_value rec_wrap(embi_bind_ctx *c, uint16_t iface, const uint8_t *p, size_t n, embi_str type, embi_str name,
                              bool is_file, double last_modified)
{
    if (rec_bytes(type.len, name.len, n) > 0xFFFFFFF0u) {
        return embi_js_throw_range_error(c->rt, "Blob too large");
    }
    size_t bytes = rec_bytes(type.len, name.len, n);
    blob_rec *r = embi_alloc(bytes);
    r->size = n;
    r->type_len = type.len;
    r->name_len = name.len;
    r->last_modified = last_modified;
    r->is_file = is_file;
    if (type.len > 0) {
        memcpy(r->tail, type.ptr, type.len);
    }
    if (name.len > 0) {
        memcpy(r->tail + type.len, name.ptr, name.len);
    }
    if (n > 0) {
        memcpy(r->tail + type.len + name.len, p, n);
    }
    return embi_bind_other_value(c, iface, r, r, (uint32_t)bytes);
}

bool ldr_blob_view(embi_js_value v, const uint8_t **p, size_t *n, embi_str *type)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || !embi_bind_iface_is(bo->iface, EMBI_IF_Blob) || embi_bind_stale(bo)) {
        return false;
    }
    const blob_rec *r = bo->ptr;
    *p = r->size > 0 ? rec_data(r) : nullptr;
    *n = r->size;
    if (type != nullptr) {
        *type = (embi_str){r->tail, r->type_len};
    }
    return true;
}

bool ldr_file_info(embi_js_value v, embi_str *name, double *last_modified)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || !embi_bind_iface_is(bo->iface, EMBI_IF_File) || embi_bind_stale(bo)) {
        return false;
    }
    const blob_rec *r = bo->ptr;
    *name = (embi_str){r->tail + r->type_len, r->name_len};
    *last_modified = r->last_modified;
    return true;
}

embi_js_value ldr_blob_new(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str type)
{
    embi_sb t = {nullptr, 0, 0};
    embi_blob_type_normalize(type, &t);
    embi_js_value v = rec_wrap(c, EMBI_IF_Blob, p, n, (embi_str){t.data, t.len}, (embi_str){nullptr, 0}, false, 0);
    embi_sb_free(&t);
    return v;
}

embi_js_value ldr_file_new(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str type, embi_str name, double last_modified)
{
    embi_sb t = {nullptr, 0, 0};
    embi_blob_type_normalize(type, &t);
    embi_js_value v = rec_wrap(c, EMBI_IF_File, p, n, (embi_str){t.data, t.len}, name, true, last_modified);
    embi_sb_free(&t);
    return v;
}

void ldr_utf8_decode(const uint8_t *p, size_t n, embi_sb *out)
{
    embi_decoder d;
    embi_decoder_init(&d, embi_encoding_for_label(EMBI_STR("utf-8")), false, false);
    (void)embi_decode(&d, p, n, true, out);
}

/* ------------------------------------------------------------------ parts */

/* Appends the bytes of a BlobPart (a Blob, a BufferSource or a string) to out. False with an exception pending. */
static bool append_part(embi_bind_ctx *c, embi_js_value part, embi_sb *out)
{
    const uint8_t *p = nullptr;
    size_t n = 0;
    if (ldr_blob_view(part, &p, &n, nullptr) || ldr_buffer_source(part, &p, &n, nullptr)) {
        if (n > 0) {
            embi_sb_append(out, (embi_str){(const char *)p, (uint32_t)n});
        }
        return true;
    }
    return ldr_to_utf8(c, part, out);
}

/* Appends every part of a sequence value. */
bool ldr_blob_parts(embi_bind_ctx *c, embi_js_value seq, const char *what, embi_sb *out)
{
    if (embi_js_is_undefined(seq)) {
        return true;
    }
    embi_js_value *items = nullptr;
    uint32_t n = 0;
    if (!embi_js_is_object(seq) || !embi_bind_seq_open(c, seq, what, &items, &n)) {
        if (!embi_js_is_object(seq)) {
            (void)embi_js_throw_type_error(c->rt, "%s: The provided value cannot be converted to a sequence.", what);
        }
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!append_part(c, items[i], out)) {
            return false;
        }
    }
    return true;
}

/* The `type` of a BlobPropertyBag / FilePropertyBag into out (normalized). False with an exception pending. */
static bool options_type(embi_bind_ctx *c, embi_js_value options, embi_sb *type)
{
    if (embi_js_is_nullish(options)) {
        return true;
    }
    if (!embi_js_is_object(options)) {
        (void)embi_js_throw_type_error(c->rt, "The provided value is not of type 'BlobPropertyBag'.");
        return false;
    }
    embi_js_value t = ldr_get(c, options, "type");
    if (t == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (embi_js_is_undefined(t)) {
        return true;
    }
    embi_sb raw = {nullptr, 0, 0};
    bool ok = ldr_to_utf8(c, t, &raw);
    if (ok) {
        embi_blob_type_normalize((embi_str){raw.data, raw.len}, type);
    }
    embi_sb_free(&raw);
    return ok;
}

embi_js_value embi_impl_Blob_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_sb bytes = {nullptr, 0, 0};
    embi_sb type = {nullptr, 0, 0};
    embi_js_value v = EMBI_JS_EXCEPTION;
    if (ldr_blob_parts(c, a0, "Failed to construct 'Blob'", &bytes) && options_type(c, a1, &type)) {
        v = rec_wrap(c, EMBI_IF_Blob, (const uint8_t *)bytes.data, bytes.len, (embi_str){type.data, type.len},
                     (embi_str){nullptr, 0}, false, 0);
        if (v != EMBI_JS_EXCEPTION) {
            embi_bind_adopt_new_target(c, v, new_target);
        }
    }
    embi_sb_free(&bytes);
    embi_sb_free(&type);
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_File_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0, embi_dstr a1,
                                         embi_js_value a2)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_sb bytes = {nullptr, 0, 0};
    embi_sb type = {nullptr, 0, 0};
    embi_js_value v = EMBI_JS_EXCEPTION;
    double modified = 0;
    bool ok = a0 != EMBI_JS_UNDEFINED && ldr_blob_parts(c, a0, "Failed to construct 'File'", &bytes) && options_type(c, a2, &type);
    if (a0 == EMBI_JS_UNDEFINED) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'File': 2 arguments required, but only 0 present.");
    }
    if (ok) {
        embi_js_value lm = ldr_get(c, a2, "lastModified");
        if (lm == EMBI_JS_EXCEPTION) {
            ok = false;
        } else if (!embi_js_is_undefined(lm)) {
            ok = embi_js_to_number(c->rt, lm, &modified);
        } else {
            SDL_Time t = 0;
            (void)SDL_GetCurrentTime(&t);
            modified = (double)(t / 1000000);
        }
    }
    if (ok) {
        embi_str name = embi_bind_utf8(c, a1);
        /* The File Standard replaces "/" by ":" in the name. */
        embi_sb nm = {nullptr, 0, 0};
        for (uint32_t i = 0; i < name.len; i++) {
            embi_sb_append_char(&nm, name.ptr[i] == '/' ? ':' : name.ptr[i]);
        }
        v = rec_wrap(c, EMBI_IF_File, (const uint8_t *)bytes.data, bytes.len, (embi_str){type.data, type.len},
                     (embi_str){nm.data, nm.len}, true, (double)to_long_long(modified));
        embi_sb_free(&nm);
        if (v != EMBI_JS_EXCEPTION) {
            embi_bind_adopt_new_target(c, v, new_target);
        }
    }
    embi_sb_free(&bytes);
    embi_sb_free(&type);
    return embi_bind_leave(sc, v);
}

/* ------------------------------------------------------------------ members */

uint64_t embi_impl_Blob_get_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_bind_stale(self) ? 0 : ((const blob_rec *)self->ptr)->size;
}

embi_dstr embi_impl_Blob_get_type(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    const blob_rec *r = self->ptr;
    return embi_bind_tmp_utf8(c, (embi_str){r->tail, r->type_len});
}

embi_dstr embi_impl_File_get_name(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    const blob_rec *r = self->ptr;
    return embi_bind_tmp_utf8(c, (embi_str){r->tail + r->type_len, r->name_len});
}

int64_t embi_impl_File_get_lastModified(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    if (embi_bind_stale(self)) {
        return 0;
    }
    double d = ((const blob_rec *)self->ptr)->last_modified; /* an int64 that the double may have rounded up to 2^63 */
    return d >= 9223372036854775808.0 ? INT64_MAX : (int64_t)d;
}

/* A relative index of Blob.slice: undefined is `def`; otherwise ToInteger clamped to [0, size]. */
static bool slice_index(embi_bind_ctx *c, embi_js_value v, double size, double def, double *out)
{
    if (embi_js_is_undefined(v)) {
        *out = def;
        return true;
    }
    double d;
    if (!embi_js_to_integer_or_infinity(c->rt, v, &d)) {
        return false;
    }
    if (d < 0) {
        d = size + d;
        if (d < 0) {
            d = 0;
        }
    } else if (d > size) {
        d = size;
    }
    *out = d;
    return true;
}

embi_dom_err embi_impl_Blob_slice(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1,
                                  embi_js_value a2, embi_js_value *out)
{
    const blob_rec *r = embi_bind_stale(self) ? nullptr : self->ptr;
    double size = r != nullptr ? (double)r->size : 0;
    double start, end;
    if (!slice_index(c, a0, size, 0, &start) || !slice_index(c, a1, size, size, &end)) {
        return EMBI_BIND_THREW;
    }
    embi_sb type = {nullptr, 0, 0};
    if (!embi_js_is_undefined(a2)) {
        embi_sb raw = {nullptr, 0, 0};
        if (!ldr_to_utf8(c, a2, &raw)) {
            embi_sb_free(&raw);
            return EMBI_BIND_THREW;
        }
        embi_blob_type_normalize((embi_str){raw.data, raw.len}, &type);
        embi_sb_free(&raw);
    }
    size_t span = end > start ? (size_t)(end - start) : 0;
    const uint8_t *p = r != nullptr && span > 0 ? rec_data(r) + (size_t)start : nullptr;
    *out = rec_wrap(c, EMBI_IF_Blob, p, span, (embi_str){type.data, type.len}, (embi_str){nullptr, 0}, false, 0);
    embi_sb_free(&type);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_js_value embi_impl_Blob_text(embi_bind_ctx *c, embi_bind_obj *self)
{
    const blob_rec *r = embi_bind_stale(self) ? nullptr : self->ptr;
    embi_sb text = {nullptr, 0, 0};
    if (r != nullptr && r->size > 0) {
        ldr_utf8_decode(rec_data(r), r->size, &text);
    }
    embi_js_value s = embi_bind_utf8_value(c, (embi_str){text.data, text.len});
    embi_sb_free(&text);
    return ldr_promise_resolve(c, s);
}

embi_js_value embi_impl_Blob_arrayBuffer(embi_bind_ctx *c, embi_bind_obj *self)
{
    const blob_rec *r = embi_bind_stale(self) ? nullptr : self->ptr;
    embi_js_value b = ldr_new_arraybuffer(c, r != nullptr ? rec_data(r) : nullptr, r != nullptr ? r->size : 0);
    return b == EMBI_JS_EXCEPTION ? ldr_promise_reject_pending(c) : ldr_promise_resolve(c, b);
}

embi_js_value embi_impl_Blob_bytes(embi_bind_ctx *c, embi_bind_obj *self)
{
    const blob_rec *r = embi_bind_stale(self) ? nullptr : self->ptr;
    embi_js_value b = ldr_new_uint8(c, r != nullptr ? rec_data(r) : nullptr, r != nullptr ? r->size : 0);
    return b == EMBI_JS_EXCEPTION ? ldr_promise_reject_pending(c) : ldr_promise_resolve(c, b);
}

embi_dom_err embi_impl_Blob_stream(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    const blob_rec *r = embi_bind_stale(self) ? nullptr : self->ptr;
    *out = ldr_stream_from_bytes(c, r != nullptr ? rec_data(r) : nullptr, r != nullptr ? r->size : 0);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ blob: URLs */

/* The serialized origin of the document ("null" for an opaque one). */
static void doc_origin(embi_bind_ctx *c, embi_sb *out)
{
    embi_url u = {0};
    embi_str base = embi_bind_url(c);
    if (embi_url_parse(base, nullptr, &u)) {
        embi_url_origin(&u, out);
        embi_url_free(&u);
    } else {
        embi_sb_append(out, EMBI_STR("null"));
    }
}

embi_dom_err embi_impl_URL_createObjectURL(embi_bind_ctx *c, embi_js_value a0, embi_dstr *out)
{
    const uint8_t *p;
    size_t n;
    embi_str type;
    if (!ldr_blob_view(a0, &p, &n, &type)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'createObjectURL' on 'URL': Overload resolution failed.");
        return EMBI_BIND_THREW;
    }
    embi_blob_part part = {.bytes = p, .len = n};
    embi_blob *b = embi_blob_new(&part, 1, type);
    embi_sb origin = {nullptr, 0, 0};
    doc_origin(c, &origin);
    embi_sb url = {nullptr, 0, 0};
    embi_blob_url_create(ldr_blobs(c), ldr_rng(), (embi_str){origin.data, origin.len}, b, &url);
    embi_blob_release(b);
    *out = embi_bind_tmp_utf8(c, (embi_str){url.data, url.len});
    embi_sb_free(&url);
    embi_sb_free(&origin);
    return EMBI_DOM_OK;
}

void embi_impl_URL_revokeObjectURL(embi_bind_ctx *c, embi_dstr a0)
{
    embi_blob_url_revoke(ldr_blobs(c), embi_bind_utf8(c, a0));
}

/* A fetch of a blob: URL: a 200 response with the blob's bytes and type (impl_loader.h). */
bool ldr_blob_response(embi_bind_ctx *c, embi_str url, embi_response *out)
{
    embi_blob *b = embi_blob_url_resolve(ldr_blobs(c), url);
    if (b == nullptr) {
        return false;
    }
    *out = (embi_response){0};
    out->type = EMBI_RESP_BASIC;
    out->status = 200;
    embi_sb_append(&out->status_text, EMBI_STR("OK"));
    size_t n = 0;
    const uint8_t *p = embi_blob_bytes(b, &n);
    out->body.present = true;
    if (n > 0) {
        embi_sb_append(&out->body.bytes, (embi_str){(const char *)p, (uint32_t)n});
    }
    if (b->type.len > 0) {
        embi_headers_add_raw(&out->headers, EMBI_STR("content-type"), (embi_str){b->type.data, b->type.len});
    }
    char len[24];
    int l = SDL_snprintf(len, sizeof len, "%llu", (unsigned long long)n);
    embi_headers_add_raw(&out->headers, EMBI_STR("content-length"), (embi_str){len, (uint32_t)l});
    embi_sb_append(&out->url, url);
    return true;
}
