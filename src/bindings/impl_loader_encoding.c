/*
 * TextEncoder, TextDecoder, atob and btoa (encoding.webidl, W7c). The decoder state is the loader's embi_decoder
 * (src/loader/loader.h), so streaming decode keeps its pending bytes between calls. The bytes of a BufferSource
 * argument are read from the ArrayBuffer, typed array or DataView record.
 */
#include "bindings/impl.h"
#include "bindings/gen/encoding.h"
#include "bindings/gen/window.h"

#include "js/typedarray.h"
#include "loader/loader.h"

#include <string.h>

/* The record of a TextDecoder: the streaming decoder, which holds no heap memory. */
typedef struct embi_tdec {
    embi_decoder d;
} embi_tdec;

static embi_js_value new_uint8(embi_bind_ctx *c, const uint8_t *p, size_t n)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value ctor = embi_js_binary_intrinsic(rt, (enum embi_js_bin_intr)(EMBI_JS_BI_TA_CTOR_BASE + EMBI_JS_TA_UINT8));
    embi_js_value len = embi_js_from_double((double)n);
    embi_js_value arr = embi_js_construct(rt, ctor, 1, &len, ctor);
    if (arr == EMBI_JS_EXCEPTION || n == 0) {
        return arr;
    }
    embi_js_varray *v = embi_js_view_of(embi_js_as_object(arr));
    memcpy(embi_js_view_base(v), p, n);
    return arr;
}

/* The bytes of a BufferSource value. Returns false when v is none of ArrayBuffer, a view or undefined. A view that
 * is out of bounds (a detached buffer) is empty. */
static bool binary_bytes(embi_js_value v, const uint8_t **p, size_t *n)
{
    *p = nullptr;
    *n = 0;
    if (embi_js_is_undefined(v)) {
        return true;
    }
    if (!embi_js_is_object(v)) {
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    if (embi_js_is_buffer_obj(o)) {
        embi_js_abuf *b = embi_js_abuf_of(o);
        if ((b->flags & EMBI_JS_ABUF_DETACHED) == 0 && b->len > 0) {
            *p = b->data;
            *n = b->len;
        }
        return true;
    }
    if (embi_js_is_typedarray_obj(o) || embi_js_is_dataview_obj(o)) {
        embi_js_varray *vw = embi_js_view_of(o);
        size_t esz = 1;
        if (embi_js_is_typedarray_obj(o)) {
            esz = embi_js_bin_size((enum embi_js_ta_kind)embi_js_bin_num(vw->elems[EMBI_JS_VIEW_KIND]));
        }
        size_t count = 0;
        if (embi_js_view_len(vw, esz, &count) && count > 0) {
            *n = count * esz;
            *p = embi_js_view_base(vw);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ TextEncoder */

embi_js_value embi_impl_TextEncoder_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_TextEncoder, c, nullptr, 0);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_dstr embi_impl_TextEncoder_get_encoding(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, EMBI_STR("utf-8"));
}

embi_js_value embi_impl_TextEncoder_encode(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str s = embi_bind_utf8(c, a0);
    embi_js_value r = new_uint8(c, (const uint8_t *)s.ptr, s.len);
    return embi_bind_leave(sc, r);
}

/* The writable bytes of a Uint8Array (encodeInto's destination). False for any other value; a detached or
 * out-of-bounds array is empty. */
static bool writable_u8(embi_js_value v, uint8_t **p, size_t *n)
{
    *p = nullptr;
    *n = 0;
    if (!embi_js_is_object(v) || !embi_js_is_typedarray_obj(embi_js_as_object(v))) {
        return false;
    }
    embi_js_varray *vw = embi_js_view_of(embi_js_as_object(v));
    if (embi_js_bin_num(vw->elems[EMBI_JS_VIEW_KIND]) != EMBI_JS_TA_UINT8) {
        return false;
    }
    size_t count = 0;
    if (embi_js_view_len(vw, 1, &count) && count > 0) {
        *n = count;
        *p = embi_js_view_base(vw);
    }
    return true;
}

/* TextEncoder.encodeInto(source, dest): writes the UTF-8 of whole code points while they fit in dest, and returns
 * { read, written } in UTF-16 code units and bytes. A supplementary character is never split. */
embi_js_value embi_impl_TextEncoder_encodeInto(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1)
{
    (void)self;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str s = embi_bind_utf8(c, a0);
    uint8_t *dst = nullptr;
    size_t cap = 0;
    if (!writable_u8(a1, &dst, &cap)) {
        (void)embi_js_throw_type_error(c->rt, "TextEncoder.encodeInto: dest must be a Uint8Array");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    size_t read = 0, written = 0;
    const unsigned char *p = (const unsigned char *)s.ptr;
    size_t i = 0;
    while (i < s.len) {
        uint32_t lead = p[i];
        size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (i + len > s.len) {
            break;
        }
        size_t units = len == 4 ? 2 : 1;
        if (written + len > cap) {
            break;
        }
        if (dst != nullptr) {
            memcpy(dst + written, p + i, len);
        }
        written += len;
        read += units;
        i += len;
    }
    embi_js_value obj = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, obj, "read", embi_js_from_double((double)read));
    embi_bind_object_set_c(c, obj, "written", embi_js_from_double((double)written));
    return embi_bind_leave(sc, obj);
}

/* ------------------------------------------------------------------ TextDecoder */

static embi_tdec *tdec_of(embi_bind_obj *self)
{
    return self->ptr;
}

embi_js_value embi_impl_TextDecoder_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0,
                                                embi_dict_TextDecoderOptions a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str label = embi_bind_utf8(c, a0);
    const embi_encoding *enc = embi_encoding_for_label(label);
    /* The replacement encoding (its labels: csiso2022kr, hz-gb-2312, ...) is not one a TextDecoder can be made for. */
    if (enc == nullptr || enc->kind == EMBI_ENC_UNSUPPORTED || enc->kind == EMBI_ENC_REPLACEMENT) {
        (void)embi_js_throw_range_error(c->rt, "Failed to construct 'TextDecoder': The \"%.*s\" encoding is not supported",
                                        (int)label.len, label.ptr);
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_tdec *t = embi_alloc(sizeof *t);
    embi_decoder_init(&t->d, enc, a1.fatal, a1.ignoreBOM);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_TextDecoder, t, t, sizeof *t);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

embi_dstr embi_impl_TextDecoder_get_encoding(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return embi_bind_tmp_utf8(c, EMBI_STR("utf-8"));
    }
    const char *name = tdec_of(self)->d.enc->name;
    return embi_bind_tmp_utf8(c, (embi_str){name, (uint32_t)strlen(name)});
}

bool embi_impl_TextDecoder_get_fatal(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return !embi_bind_stale(self) && tdec_of(self)->d.fatal;
}

bool embi_impl_TextDecoder_get_ignoreBOM(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return !embi_bind_stale(self) && tdec_of(self)->d.ignore_bom;
}

embi_dom_err embi_impl_TextDecoder_decode(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                          embi_dict_TextDecodeOptions a1, embi_dstr *out)
{
    if (embi_bind_stale(self)) {
        *out = embi_bind_tmp_utf8(c, EMBI_STR(""));
        return EMBI_DOM_OK;
    }
    /* No scope of our own: the string returned in out lives until the caller's scope ends. */
    embi_tdec *t = tdec_of(self);
    const uint8_t *p = nullptr;
    size_t n = 0;
    if (!binary_bytes(a0, &p, &n)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'decode' on 'TextDecoder': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
        return EMBI_BIND_THREW;
    }
    embi_sb text = {nullptr, 0, 0};
    bool ok = embi_decode(&t->d, p, n, !a1.stream, &text);
    if (!ok) {
        embi_sb_free(&text);
        /* A fatal error leaves a new decoder behind: the bytes that followed it are dropped, so a later decode() starts clean. */
        embi_decoder_init(&t->d, t->d.enc, t->d.fatal, t->d.ignore_bom);
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid for encoding %s",
                                       t->d.enc->name);
        return EMBI_BIND_THREW;
    }
    *out = embi_bind_tmp_utf8(c, (embi_str){text.data, text.len});
    embi_sb_free(&text);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ atob and btoa (Window) */

embi_dom_err embi_impl_Window_atob(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr *out)
{
    (void)self;
    embi_sb bytes = {nullptr, 0, 0};
    if (!embi_base64_decode(embi_bind_utf8(c, a0), &bytes)) {
        embi_sb_free(&bytes);
        (void)embi_bind_throw_named(c, "InvalidCharacterError",
                                    "Failed to execute 'atob' on 'Window': The string to be decoded is not correctly encoded.");
        return EMBI_BIND_THREW;
    }
    /* A binary string: one code point per byte (U+0000 to U+00FF), kept as UTF-8. */
    embi_sb latin = {nullptr, 0, 0};
    for (uint32_t i = 0; i < bytes.len; i++) {
        embi_sb_append_cp(&latin, (unsigned char)bytes.data[i]);
    }
    *out = embi_bind_tmp_utf8(c, (embi_str){latin.data, latin.len});
    embi_sb_free(&latin);
    embi_sb_free(&bytes);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Window_btoa(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr *out)
{
    (void)self;
    embi_str in = embi_bind_utf8(c, a0);
    embi_sb bytes = {nullptr, 0, 0};
    const char *p = in.ptr;
    const char *end = embi_str_end(in);
    while (p != nullptr && p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (cp > 0xffu) {
            embi_sb_free(&bytes);
            (void)embi_bind_throw_named(c, "InvalidCharacterError",
                                        "Failed to execute 'btoa' on 'Window': The string to be encoded contains characters outside of the Latin1 range.");
            return EMBI_BIND_THREW;
        }
        embi_sb_append_char(&bytes, (char)(unsigned char)cp);
    }
    embi_sb b64 = {nullptr, 0, 0};
    embi_base64_encode((const unsigned char *)bytes.data, bytes.len, &b64);
    *out = embi_bind_tmp_utf8(c, (embi_str){b64.data, b64.len});
    embi_sb_free(&b64);
    embi_sb_free(&bytes);
    return EMBI_DOM_OK;
}
