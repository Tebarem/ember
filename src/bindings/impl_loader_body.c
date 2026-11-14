/*
 * Request, Response and the Body mixin (fetch.webidl, W7c; Fetch Standard sections 5.1 to 5.5).
 *
 * A Request or Response wrapper owns one flat block of immutable data (strings inline) plus a few mutable flags. What is
 * mutable or heap-sized lives in the wrapper's ref slot, a JS array: the Headers wrapper, the body bytes (a Uint8Array),
 * the body stream (a ReadableStream, made lazily for a byte body, or the body itself for a Response built from a stream),
 * and for a Request the AbortSignal. The finalizer therefore frees one block and nothing leaks.
 *
 * Deviations: Request.signal returns the init signal itself (the standard makes a dependent signal); the referrer is
 * always "about:client"; stream bodies cannot be cloned (no tee).
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"

enum { B_HEADERS, B_BODY, B_STREAM, B_SIGNAL, B_N };

typedef struct req_rec {
    bool used;
    bool has_body;
    bool keepalive;
    uint8_t redirect;
    uint8_t mode;
    uint8_t credentials;
    uint8_t cache;
    uint8_t policy;
    uint32_t method_len, url_len, integrity_len;
    char tail[]; /* method, url, integrity */
} req_rec;

typedef struct resp_rec {
    bool used;
    bool has_body;
    bool redirected;
    uint8_t type;
    uint16_t status;
    uint32_t url_len, text_len;
    char tail[]; /* url, status text */
} resp_rec;

static const char *const k_redirect[] = {"follow", "error", "manual"};
static const char *const k_mode[] = {"cors", "no-cors", "same-origin", "navigate"};

/* The guard of a request's headers (Fetch 5.4, "Request" constructor): request-no-cors for mode no-cors. */
static embi_guard req_guard(uint8_t mode)
{
    return mode == 1 ? EMBI_GUARD_REQUEST_NO_CORS : EMBI_GUARD_REQUEST;
}
static const char *const k_credentials[] = {"same-origin", "omit", "include"};
static const char *const k_cache[] = {"default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached"};
static const char *const k_policy[] = {"",
                                       "no-referrer",
                                       "no-referrer-when-downgrade",
                                       "same-origin",
                                       "origin",
                                       "strict-origin",
                                       "origin-when-cross-origin",
                                       "strict-origin-when-cross-origin",
                                       "unsafe-url"};
static const char *const k_priority[] = {"high", "low", "auto"};
static const char *const k_duplex[] = {"half"};
static const char *const k_resp_type[] = {"default", "basic", "error", "opaqueredirect", "cors"};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static embi_js_value slot(embi_bind_ctx *c, embi_bind_obj *bo, int i)
{
    return embi_bind_array_get(c, bo->ref, (uint32_t)i);
}

static void slot_set(embi_bind_ctx *c, embi_bind_obj *bo, int i, embi_js_value v)
{
    embi_bind_array_set(c, bo->ref, (uint32_t)i, v);
}

static embi_dstr cstr(embi_bind_ctx *c, const char *s)
{
    return embi_bind_tmp_utf8(c, (embi_str){s, (uint32_t)strlen(s)});
}

static int enum_index(const char *const *names, int n, embi_str s)
{
    for (int i = 0; i < n; i++) {
        if (embi_str_eq(s, (embi_str){names[i], (uint32_t)strlen(names[i])})) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ body access shared by Request and Response */

typedef struct body_ref {
    bool *used;
    bool *has_body;
} body_ref;

static bool body_ref_of(embi_bind_obj *bo, body_ref *out)
{
    if (embi_bind_stale(bo)) {
        return false;
    }
    if (bo->iface == EMBI_IF_Request) {
        req_rec *r = bo->ptr;
        *out = (body_ref){&r->used, &r->has_body};
        return true;
    }
    resp_rec *r = bo->ptr;
    *out = (body_ref){&r->used, &r->has_body};
    return true;
}

static bool stream_disturbed_or_locked(embi_js_value stream)
{
    embi_stream *s = ldr_stream_core(stream);
    return s != nullptr && (s->disturbed || s->locked);
}

static bool body_used(embi_bind_ctx *c, embi_bind_obj *bo)
{
    body_ref b;
    if (!body_ref_of(bo, &b)) {
        return false;
    }
    if (*b.used) {
        return true;
    }
    embi_stream *s = ldr_stream_core(slot(c, bo, B_STREAM));
    return s != nullptr && s->disturbed;
}

/* The Content-Type header value of the headers wrapper of bo. */
static void content_type_of(embi_bind_ctx *c, embi_bind_obj *bo, embi_sb *out)
{
    embi_headers h;
    if (ldr_headers_load(slot(c, bo, B_HEADERS), &h)) {
        (void)embi_headers_get(&h, EMBI_STR("content-type"), out);
        embi_headers_free(&h);
    }
}

static embi_js_value body_getter(embi_bind_ctx *c, embi_bind_obj *bo)
{
    body_ref b;
    if (!body_ref_of(bo, &b) || !*b.has_body) {
        return EMBI_JS_NULL;
    }
    embi_js_value st = slot(c, bo, B_STREAM);
    if (embi_js_is_object(st)) {
        return st;
    }
    const uint8_t *p = nullptr;
    size_t n = 0;
    (void)ldr_buffer_source(slot(c, bo, B_BODY), &p, &n, nullptr);
    embi_js_value s = ldr_stream_from_bytes(c, p, n);
    if (s == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_NULL;
    }
    slot_set(c, bo, B_STREAM, s);
    if (*b.used) {
        ldr_stream_consume(s); /* a body read after it was consumed is a locked, disturbed stream */
    }
    return s;
}

/* The value a consumer kind makes of the body bytes. EMBI_JS_EXCEPTION with the exception pending on failure. */
static embi_js_value convert(embi_bind_ctx *c, int kind, const uint8_t *p, size_t n, embi_str ctype)
{
    switch (kind) {
    case LDR_BODY_TEXT:
    case LDR_BODY_JSON: {
        embi_sb text = {nullptr, 0, 0};
        if (n > 0) {
            ldr_utf8_decode(p, n, &text);
        }
        embi_js_value s = embi_bind_utf8_value(c, (embi_str){text.data, text.len});
        embi_sb_free(&text);
        if (kind == LDR_BODY_TEXT) {
            return s;
        }
        return embi_js_json_parse_text(c->rt, embi_js_as_string(s));
    }
    case LDR_BODY_ARRAYBUFFER:
        return ldr_new_arraybuffer(c, p, n);
    case LDR_BODY_BYTES:
        return ldr_new_uint8(c, p, n);
    case LDR_BODY_BLOB:
        return ldr_blob_new(c, p, n, ctype);
    case LDR_BODY_FORMDATA:
    default: {
        embi_js_value fd = ldr_formdata_parse(c, p, n, ctype);
        if (fd == EMBI_JS_NULL) {
            return embi_js_throw_type_error(c->rt, "Failed to fetch: the body could not be parsed as form data.");
        }
        return fd;
    }
    }
}

static embi_js_value settle_with(embi_bind_ctx *c, embi_js_value promise, embi_js_value v)
{
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(c->rt);
        embi_js_exception_clear(c->rt);
        (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(promise), ex);
    } else {
        (void)embi_js_promise_resolve_with(c->rt, embi_js_as_object(promise), v);
    }
    return promise;
}

/* The state of an asynchronous consume: [result promise, content type string, kind]. */
static embi_js_value consume_ok(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value st = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    embi_js_value promise = embi_bind_array_get(c, st, 0);
    embi_js_value ctype = embi_bind_array_get(c, st, 1);
    embi_js_value kind = embi_bind_array_get(c, st, 2);
    embi_sb ct = {nullptr, 0, 0};
    if (embi_js_is_string(ctype)) {
        (void)ldr_to_utf8(c, ctype, &ct);
    }
    const uint8_t *p = nullptr;
    size_t n = 0;
    (void)ldr_buffer_source(argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, &p, &n, nullptr);
    embi_bind_scope sc = embi_bind_enter(c);
    (void)settle_with(c, promise, convert(c, embi_js_as_int32(kind), p, n, (embi_str){ct.data, ct.len}));
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    embi_sb_free(&ct);
    return EMBI_JS_UNDEFINED;
}

static embi_js_value consume_err(embi_js_realm *rl, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(rl);
    embi_js_value st = embi_js_persistent_get(c->rt, (uint32_t)magic);
    embi_js_persistent_free(c->rt, (uint32_t)magic);
    (void)embi_js_promise_reject_with(c->rt, embi_js_as_object(embi_bind_array_get(c, st, 0)),
                                      argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

/* Body.text(), json(), ... of a Request or Response wrapper. */
static embi_js_value consume(embi_bind_ctx *c, embi_bind_obj *bo, int kind)
{
    body_ref b;
    if (!body_ref_of(bo, &b)) {
        return ldr_promise_type_error(c, "Illegal invocation");
    }
    embi_js_value stream = slot(c, bo, B_STREAM);
    if (*b.used || (*b.has_body && stream_disturbed_or_locked(stream))) {
        return ldr_promise_type_error(c, "Body has already been consumed.");
    }
    embi_sb ct = {nullptr, 0, 0};
    content_type_of(c, bo, &ct);
    embi_js_value result;
    embi_js_value bytes = slot(c, bo, B_BODY);
    if (*b.has_body && embi_js_is_undefined(bytes) && embi_js_is_object(stream)) {
        /* A body made from a stream: read it to the end, then convert. */
        *b.used = true;
        embi_js_object *po = embi_js_promise_new(c->rt);
        result = embi_js_from_object(po);
        embi_js_value st = embi_bind_array_new(c, 3);
        embi_bind_array_set(c, st, 0, result);
        embi_bind_array_set(c, st, 1, embi_bind_utf8_value(c, (embi_str){ct.data, ct.len}));
        embi_bind_array_set(c, st, 2, embi_js_from_int32(kind));
        uint32_t pid = embi_js_persistent_new(c->rt, st);
        embi_js_value drain = ldr_stream_read_all(c, stream);
        embi_js_value f_ok = ldr_native(c, "", 1, consume_ok, (int)pid);
        embi_js_value f_err = ldr_native(c, "", 1, consume_err, (int)pid);
        (void)embi_js_promise_then(c->rt, embi_js_as_object(drain), f_ok, f_err, nullptr);
        embi_sb_free(&ct);
        return result;
    }
    *b.used = *b.has_body ? true : *b.used;
    if (embi_js_is_object(stream)) {
        ldr_stream_consume(stream);
    }
    const uint8_t *p = nullptr;
    size_t n = 0;
    (void)ldr_buffer_source(bytes, &p, &n, nullptr);
    embi_js_object *po = embi_js_promise_new(c->rt);
    result = embi_js_from_object(po);
    (void)settle_with(c, result, convert(c, kind, p, n, (embi_str){ct.data, ct.len}));
    embi_sb_free(&ct);
    return result;
}

/* ------------------------------------------------------------------ body init */

int ldr_body_init(embi_bind_ctx *c, embi_js_value v, embi_sb *bytes, embi_sb *ctype)
{
    if (embi_js_is_nullish(v)) {
        return 0;
    }
    const uint8_t *p;
    size_t n;
    embi_str type;
    if (ldr_blob_view(v, &p, &n, &type)) {
        if (n > 0) {
            embi_sb_append(bytes, (embi_str){(const char *)p, (uint32_t)n});
        }
        if (type.len > 0) {
            embi_sb_append(ctype, type);
        }
        return 1;
    }
    bool detached = false;
    if (ldr_buffer_source(v, &p, &n, &detached)) {
        if (n > 0) {
            embi_sb_append(bytes, (embi_str){(const char *)p, (uint32_t)n});
        }
        return 1;
    }
    if (ldr_is_formdata(v)) {
        return ldr_formdata_multipart(c, v, bytes, ctype) ? 1 : -1;
    }
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo != nullptr && bo->iface == EMBI_IF_ReadableStream) {
        return 2;
    }
    if (bo != nullptr && bo->iface == EMBI_IF_URLSearchParams) {
        embi_js_value s = embi_js_invoke(c->rt, v, embi_js_key_c(c->rt, "toString"), 0, nullptr);
        if (s == EMBI_JS_EXCEPTION || !ldr_to_utf8(c, s, bytes)) {
            return -1;
        }
        embi_sb_append(ctype, EMBI_STR("application/x-www-form-urlencoded;charset=UTF-8"));
        return 1;
    }
    if (!ldr_to_utf8(c, v, bytes)) {
        return -1;
    }
    embi_sb_append(ctype, EMBI_STR("text/plain;charset=UTF-8"));
    return 1;
}

/* Stores a body init into the ref array of a new Request or Response wrapper. kind: 0 none, 1 bytes, 2 stream. */
static bool set_body(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value init, bool *has_body, embi_sb *ctype)
{
    embi_sb bytes = {nullptr, 0, 0};
    int k = ldr_body_init(c, init, &bytes, ctype);
    if (k < 0) {
        embi_sb_free(&bytes);
        return false;
    }
    *has_body = k != 0;
    if (k == 1) {
        embi_js_value u8 = ldr_new_uint8(c, (const uint8_t *)bytes.data, bytes.len);
        if (u8 == EMBI_JS_EXCEPTION) {
            embi_sb_free(&bytes);
            return false;
        }
        slot_set(c, bo, B_BODY, u8);
    } else if (k == 2) {
        if (stream_disturbed_or_locked(init)) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct body: the ReadableStream is locked or disturbed.");
            embi_sb_free(&bytes);
            return false;
        }
        slot_set(c, bo, B_STREAM, init);
    }
    embi_sb_free(&bytes);
    return true;
}

/* ------------------------------------------------------------------ Request */

static embi_js_value req_wrap(embi_bind_ctx *c, embi_str method, embi_str url, embi_str integrity, const req_rec *proto,
                              embi_js_value headers, embi_js_value signal, embi_bind_obj **bo_out)
{
    size_t bytes = sizeof(req_rec) + method.len + url.len + integrity.len;
    req_rec *r = embi_alloc(bytes);
    *r = proto != nullptr ? *proto : (req_rec){0};
    r->method_len = method.len;
    r->url_len = url.len;
    r->integrity_len = integrity.len;
    memcpy(r->tail, method.ptr, method.len);
    memcpy(r->tail + method.len, url.ptr, url.len);
    if (integrity.len > 0) {
        memcpy(r->tail + method.len + url.len, integrity.ptr, integrity.len);
    }
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_Request, r, r, (uint32_t)bytes);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_bind_set_ref(bo, embi_bind_array_new(c, B_N));
    slot_set(c, bo, B_HEADERS, headers);
    slot_set(c, bo, B_SIGNAL, signal);
    *bo_out = bo;
    return v;
}

static embi_str req_method(const req_rec *r)
{
    return (embi_str){r->tail, r->method_len};
}

static embi_str req_url(const req_rec *r)
{
    return (embi_str){r->tail + r->method_len, r->url_len};
}

static embi_str req_integrity(const req_rec *r)
{
    return (embi_str){r->tail + r->method_len + r->url_len, r->integrity_len};
}

/* Parses a request URL against the document URL: false with a TypeError pending. */
static bool parse_request_url(embi_bind_ctx *c, embi_str input, embi_sb *href)
{
    embi_url base = {0};
    bool has_base = embi_url_parse(embi_bind_url(c), nullptr, &base);
    embi_url u = {0};
    bool ok = embi_url_parse(input, has_base ? &base : nullptr, &u);
    if (has_base) {
        embi_url_free(&base);
    }
    if (!ok) {
        (void)embi_js_throw_type_error(c->rt, "Failed to parse URL from %.*s", (int)input.len, input.ptr);
        return false;
    }
    embi_sb user = {nullptr, 0, 0}, pass = {nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_USERNAME, &user);
    embi_url_get(&u, EMBI_URL_PASSWORD, &pass);
    bool creds = user.len > 0 || pass.len > 0;
    embi_sb_free(&user);
    embi_sb_free(&pass);
    if (creds) {
        (void)embi_js_throw_type_error(c->rt, "Request cannot be constructed from a URL that includes credentials: %.*s",
                                       (int)input.len, input.ptr);
        embi_url_free(&u);
        return false;
    }
    embi_sb_append(href, (embi_str){u.href, u.href_len});
    embi_url_free(&u);
    return true;
}

static bool enum_member(embi_bind_ctx *c, embi_js_value init, const char *name, const char *const *names, int n, int *out,
                        bool *present)
{
    *present = false;
    embi_js_value v = ldr_get(c, init, name);
    if (v == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (embi_js_is_undefined(v)) {
        return true;
    }
    embi_sb s = {nullptr, 0, 0};
    if (!ldr_to_utf8(c, v, &s)) {
        return false;
    }
    int idx = enum_index(names, n, (embi_str){s.data, s.len});
    if (idx < 0) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': The provided value '%.*s' is not a valid enum value of type Request%c%s.",
                                       (int)s.len, s.data, 'A' - 'A' + ' ', name);
        embi_sb_free(&s);
        return false;
    }
    embi_sb_free(&s);
    *out = idx;
    *present = true;
    return true;
}

embi_js_value ldr_request_make(embi_bind_ctx *c, embi_js_value input, embi_js_value init)
{
    (void)ldr_state_of(c);
    embi_bind_obj *in_bo = embi_bind_obj_of(input);
    bool from_req = in_bo != nullptr && in_bo->iface == EMBI_IF_Request && !embi_bind_stale(in_bo);
    if (!embi_js_is_nullish(init) && !embi_js_is_object(init)) {
        return embi_js_throw_type_error(c->rt, "Failed to construct 'Request': The provided value is not of type 'RequestInit'.");
    }
    req_rec proto = {0};
    embi_sb method = {nullptr, 0, 0}, url = {nullptr, 0, 0}, integrity = {nullptr, 0, 0};
    embi_headers hdrs = {0};
    hdrs.guard = EMBI_GUARD_REQUEST;
    embi_js_value signal = EMBI_JS_UNDEFINED;
    embi_js_value result = EMBI_JS_EXCEPTION;
    embi_js_value init_body = EMBI_JS_UNDEFINED;
    bool body_from_input = false;
    bool ok = true;
    if (from_req) {
        const req_rec *ir = in_bo->ptr;
        proto = *ir;
        embi_sb_append(&method, req_method(ir));
        embi_sb_append(&url, req_url(ir));
        embi_sb_append(&integrity, req_integrity(ir));
        signal = slot(c, in_bo, B_SIGNAL);
        embi_headers src;
        if (ldr_headers_load(slot(c, in_bo, B_HEADERS), &src)) {
            ldr_headers_copy(&src, &hdrs);
            embi_headers_free(&src);
        }
        proto.used = false;
        proto.has_body = false;
        body_from_input = ir->has_body;
    } else {
        embi_sb in = {nullptr, 0, 0};
        ok = ldr_to_utf8(c, input, &in) && parse_request_url(c, (embi_str){in.data, in.len}, &url);
        embi_sb_free(&in);
        embi_sb_append(&method, EMBI_STR("GET"));
    }
    bool method_given = false, headers_given = false, duplex_given = false;
    if (ok && embi_js_is_object(init)) {
        embi_js_value m = ldr_get(c, init, "method");
        if (m == EMBI_JS_EXCEPTION) {
            ok = false;
        } else if (!embi_js_is_undefined(m)) {
            embi_sb raw = {nullptr, 0, 0};
            embi_sb norm = {nullptr, 0, 0};
            ok = ldr_to_utf8(c, m, &raw);
            if (ok && !embi_method_normalize((embi_str){raw.data, raw.len}, &norm)) {
                (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': '%.*s' is not a valid HTTP method.",
                                               (int)raw.len, raw.data);
                ok = false;
            }
            if (ok) {
                embi_sb_clear(&method);
                embi_sb_append(&method, (embi_str){norm.data, norm.len});
                method_given = true;
            }
            embi_sb_free(&raw);
            embi_sb_free(&norm);
        }
        (void)method_given;
        int idx;
        bool present;
        if (ok && enum_member(c, init, "redirect", k_redirect, COUNT(k_redirect), &idx, &present)) {
            if (present) {
                proto.redirect = (uint8_t)idx;
            }
        } else {
            ok = false;
        }
        if (ok && enum_member(c, init, "mode", k_mode, COUNT(k_mode), &idx, &present)) {
            if (present) {
                if (idx == 3) {
                    (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': Cannot construct a Request with a RequestInit whose mode member is set as 'navigate'.");
                    ok = false;
                }
                proto.mode = (uint8_t)idx;
            }
        } else {
            ok = false;
        }
        if (ok && enum_member(c, init, "credentials", k_credentials, COUNT(k_credentials), &idx, &present)) {
            if (present) {
                proto.credentials = (uint8_t)idx;
            }
        } else {
            ok = false;
        }
        if (ok && enum_member(c, init, "cache", k_cache, COUNT(k_cache), &idx, &present)) {
            if (present) {
                proto.cache = (uint8_t)idx;
            }
        } else {
            ok = false;
        }
        if (ok && enum_member(c, init, "referrerPolicy", k_policy, COUNT(k_policy), &idx, &present)) {
            if (present) {
                proto.policy = (uint8_t)idx;
            }
        } else {
            ok = false;
        }
        if (ok) {
            /* priority and duplex are validated, not stored (the duplex getter is always "half"). */
            if (!enum_member(c, init, "priority", k_priority, COUNT(k_priority), &idx, &present)) {
                ok = false;
            } else if (!enum_member(c, init, "duplex", k_duplex, COUNT(k_duplex), &idx, &duplex_given)) {
                ok = false;
            }
        }
        if (ok) {
            /* window must be null or absent; an invalid referrer URL is a TypeError. */
            embi_js_value wv = ldr_get(c, init, "window");
            embi_js_value rv = wv == EMBI_JS_EXCEPTION ? wv : ldr_get(c, init, "referrer");
            if (wv == EMBI_JS_EXCEPTION || rv == EMBI_JS_EXCEPTION) {
                ok = false;
            } else if (!embi_js_is_nullish(wv)) {
                (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': 'window' must be null.");
                ok = false;
            } else if (!embi_js_is_undefined(rv)) {
                embi_sb rs = {nullptr, 0, 0};
                ok = ldr_to_utf8(c, rv, &rs);
                if (ok && rs.len > 0) {
                    embi_sb href = {nullptr, 0, 0};
                    ok = parse_request_url(c, (embi_str){rs.data, rs.len}, &href);
                    embi_sb_free(&href);
                }
                embi_sb_free(&rs);
            }
        }
        if (ok) {
            embi_js_value ig = ldr_get(c, init, "integrity");
            embi_js_value ka = ldr_get(c, init, "keepalive");
            if (ig == EMBI_JS_EXCEPTION || ka == EMBI_JS_EXCEPTION) {
                ok = false;
            } else {
                if (!embi_js_is_undefined(ig)) {
                    embi_sb_clear(&integrity);
                    ok = ldr_to_utf8(c, ig, &integrity);
                }
                if (!embi_js_is_undefined(ka)) {
                    proto.keepalive = embi_js_to_boolean(ka);
                }
            }
        }
        if (ok) {
            embi_js_value sg = ldr_get(c, init, "signal");
            if (sg == EMBI_JS_EXCEPTION) {
                ok = false;
            } else if (!embi_js_is_undefined(sg)) {
                if (!embi_js_is_nullish(sg) && embi_bind_obj_of(sg) == nullptr) {
                    (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': Failed to read the 'signal' property from 'RequestInit': Failed to convert value to 'AbortSignal'.");
                    ok = false;
                } else {
                    signal = embi_js_is_null(sg) ? EMBI_JS_UNDEFINED : sg;
                }
            }
        }
        if (ok) {
            embi_js_value hv = ldr_get(c, init, "headers");
            if (hv == EMBI_JS_EXCEPTION) {
                ok = false;
            } else if (!embi_js_is_undefined(hv)) {
                headers_given = true;
                embi_headers_free(&hdrs);
                hdrs = (embi_headers){0};
                hdrs.guard = req_guard(proto.mode);
                ok = ldr_headers_fill(c, &hdrs, hv, "Failed to construct 'Request'");
            }
        }
        if (ok) {
            init_body = ldr_get(c, init, "body");
            if (init_body == EMBI_JS_EXCEPTION) {
                ok = false;
            }
        }
        if (ok) {
            bool simple = (method.len == 3 && (memcmp(method.data, "GET", 3) == 0)) ||
                          (method.len == 4 && (memcmp(method.data, "HEAD", 4) == 0 || memcmp(method.data, "POST", 4) == 0));
            if (proto.mode == 1 && !simple) {
                (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': '%.*s' is unsupported in no-cors mode.",
                                               (int)method.len, method.data);
                ok = false;
            } else if (proto.cache == 5 && proto.mode != 2) {
                (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': 'only-if-cached' can be set only with 'same-origin' mode.");
                ok = false;
            } else {
                embi_bind_obj *ibo = embi_bind_obj_of(init_body);
                if (ibo != nullptr && ibo->iface == EMBI_IF_ReadableStream && !duplex_given) {
                    (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': The `duplex` member must be specified for a request with a streaming body.");
                    ok = false;
                }
            }
        }
    }
    if (ok && from_req && !headers_given && proto.mode == 1) {
        /* A request made no-cors from another request: its headers pass the request-no-cors guard (Fetch 5.4 step 33). */
        embi_headers filtered = {0};
        filtered.guard = EMBI_GUARD_REQUEST_NO_CORS;
        for (uint32_t i = 0; i < hdrs.list.len; i++) {
            (void)embi_headers_append(&filtered, (embi_str){hdrs.list.data[i].name.data, hdrs.list.data[i].name.len},
                                      (embi_str){hdrs.list.data[i].value.data, hdrs.list.data[i].value.len});
        }
        embi_headers_free(&hdrs);
        hdrs = filtered;
    }
    (void)headers_given;
    embi_sb ctype = {nullptr, 0, 0};
    embi_js_value hw = EMBI_JS_UNDEFINED;
    embi_bind_obj *bo = nullptr;
    if (ok) {
        bool no_body_method = (method.len == 3 && memcmp(method.data, "GET", 3) == 0) ||
                              (method.len == 4 && memcmp(method.data, "HEAD", 4) == 0);
        bool has_init_body = !embi_js_is_nullish(init_body);
        if ((has_init_body || (!has_init_body && body_from_input && !embi_js_is_object(init))) && no_body_method &&
            (has_init_body || body_from_input)) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
            ok = false;
        }
        if (ok && !has_init_body && body_from_input && embi_js_is_object(init) && no_body_method) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
            ok = false;
        }
        if (ok && !has_init_body && body_from_input &&
            (body_used(c, in_bo) || stream_disturbed_or_locked(slot(c, in_bo, B_STREAM)))) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used.");
            ok = false;
        }
        if (ok) {
            hw = ldr_headers_new(c, &hdrs, req_guard(proto.mode));
            embi_js_value v = req_wrap(c, (embi_str){method.data, method.len}, (embi_str){url.data, url.len},
                                       (embi_str){integrity.data, integrity.len}, &proto, hw, signal, &bo);
            if (has_init_body) {
                ok = set_body(c, bo, init_body, &((req_rec *)bo->ptr)->has_body, &ctype);
                if (ok && ctype.len > 0) {
                    embi_headers h;
                    if (ldr_headers_load(hw, &h)) {
                        if (!embi_headers_has(&h, EMBI_STR("content-type"))) {
                            (void)embi_headers_append(&h, EMBI_STR("content-type"), (embi_str){ctype.data, ctype.len});
                            ldr_headers_store(hw, &h);
                        }
                        embi_headers_free(&h);
                    }
                }
            } else if (body_from_input) {
                /* The body moves to the new request: the bytes are shared, a stream body is taken over. */
                ((req_rec *)bo->ptr)->has_body = true;
                slot_set(c, bo, B_BODY, slot(c, in_bo, B_BODY));
                embi_js_value ins = slot(c, in_bo, B_STREAM);
                if (embi_js_is_undefined(slot(c, in_bo, B_BODY))) {
                    slot_set(c, bo, B_STREAM, ins);
                }
                ((req_rec *)in_bo->ptr)->used = true;
            }
            if (ok) {
                result = v;
            }
        }
    }
    embi_sb_free(&ctype);
    embi_sb_free(&method);
    embi_sb_free(&url);
    embi_sb_free(&integrity);
    embi_headers_free(&hdrs);
    return result;
}

embi_js_value embi_impl_Request_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = ldr_request_make(c, a0, a1);
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

bool ldr_request_view_of(embi_js_value v, ldr_request_view *out)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || bo->iface != EMBI_IF_Request || embi_bind_stale(bo)) {
        return false;
    }
    embi_bind_ctx *c = bo->ctx;
    const req_rec *r = bo->ptr;
    *out = (ldr_request_view){0};
    out->method = req_method(r);
    out->url = req_url(r);
    out->redirect = (embi_redirect)r->redirect;
    /* credentials: 0 same-origin, 1 omit, 2 include (k_credentials) */
    out->creds = r->credentials == 1 ? EMBI_FETCH_CREDS_OMIT
                 : r->credentials == 2 ? EMBI_FETCH_CREDS_INCLUDE
                                       : EMBI_FETCH_CREDS_SAME_ORIGIN;
    out->signal = slot(c, bo, B_SIGNAL);
    out->body_used = body_used(c, bo);
    embi_js_value bytes = slot(c, bo, B_BODY);
    out->has_body = r->has_body;
    out->body_from_stream = r->has_body && embi_js_is_undefined(bytes);
    (void)ldr_buffer_source(bytes, &out->body, &out->body_len, nullptr);
    if (!ldr_headers_load(slot(c, bo, B_HEADERS), &out->headers)) {
        out->headers = (embi_headers){0};
    }
    return true;
}

void ldr_request_mark_used(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo != nullptr && bo->iface == EMBI_IF_Request && !embi_bind_stale(bo)) {
        ((req_rec *)bo->ptr)->used = true;
    }
}

#define REQ_STR(name, expr)                                                                 \
    embi_dstr embi_impl_Request_get_##name(embi_bind_ctx *c, embi_bind_obj *self)           \
    {                                                                                       \
        if (embi_bind_stale(self)) {                                                        \
            return cstr(c, "");                                                             \
        }                                                                                   \
        const req_rec *r = self->ptr;                                                       \
        (void)r;                                                                            \
        return expr;                                                                        \
    }

REQ_STR(method, embi_bind_tmp_utf8(c, req_method(r)))
REQ_STR(url, embi_bind_tmp_utf8(c, req_url(r)))
REQ_STR(destination, cstr(c, ""))
REQ_STR(referrer, cstr(c, "about:client"))
REQ_STR(referrerPolicy, cstr(c, k_policy[r->policy]))
REQ_STR(mode, cstr(c, k_mode[r->mode]))
REQ_STR(credentials, cstr(c, k_credentials[r->credentials]))
REQ_STR(cache, cstr(c, k_cache[r->cache]))
REQ_STR(redirect, cstr(c, k_redirect[r->redirect]))
REQ_STR(integrity, embi_bind_tmp_utf8(c, req_integrity(r)))
REQ_STR(duplex, cstr(c, "half"))

bool embi_impl_Request_get_keepalive(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return !embi_bind_stale(self) && ((const req_rec *)self->ptr)->keepalive;
}

embi_js_value embi_impl_Request_get_headers(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_stale(self) ? EMBI_JS_UNDEFINED : slot(c, self, B_HEADERS);
}

embi_js_value embi_impl_Request_get_signal(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value s = slot(c, self, B_SIGNAL);
    if (embi_js_is_object(s)) {
        return s;
    }
    /* No signal given: a signal that never aborts, from a fresh AbortController. */
    embi_js_value ctor = ldr_get(c, embi_js_from_object(c->realm->global), "AbortController");
    if (ctor == EMBI_JS_EXCEPTION || !embi_js_is_callable(ctor)) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_NULL;
    }
    embi_js_value ac = embi_js_construct(c->rt, ctor, 0, nullptr, ctor);
    if (ac == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_NULL;
    }
    embi_js_value sig = ldr_get(c, ac, "signal");
    if (sig == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_NULL;
    }
    slot_set(c, self, B_SIGNAL, sig);
    return sig;
}

embi_js_value embi_impl_Request_get_body(embi_bind_ctx *c, embi_bind_obj *self)
{
    return body_getter(c, self);
}

bool embi_impl_Request_get_bodyUsed(embi_bind_ctx *c, embi_bind_obj *self)
{
    return body_used(c, self);
}

embi_dom_err embi_impl_Request_clone(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    if (embi_bind_stale(self)) {
        *out = EMBI_JS_UNDEFINED;
        return EMBI_DOM_OK;
    }
    req_rec *r = self->ptr;
    if (body_used(c, self) || (r->has_body && embi_js_is_undefined(slot(c, self, B_BODY)))) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'clone' on 'Request': Request body is already used or is a stream.");
        return EMBI_BIND_THREW;
    }
    embi_headers h;
    embi_js_value hw = EMBI_JS_UNDEFINED;
    if (ldr_headers_load(slot(c, self, B_HEADERS), &h)) {
        hw = ldr_headers_new(c, &h, req_guard(r->mode));
        embi_headers_free(&h);
    }
    embi_bind_obj *bo;
    *out = req_wrap(c, req_method(r), req_url(r), req_integrity(r), r, hw, slot(c, self, B_SIGNAL), &bo);
    slot_set(c, bo, B_BODY, slot(c, self, B_BODY));
    return EMBI_DOM_OK;
}

#define REQ_CONSUME(fn, kind)                                                               \
    embi_js_value embi_impl_Request_##fn(embi_bind_ctx *c, embi_bind_obj *self)             \
    {                                                                                       \
        return consume(c, self, kind);                                                      \
    }

REQ_CONSUME(arrayBuffer, LDR_BODY_ARRAYBUFFER)
REQ_CONSUME(blob, LDR_BODY_BLOB)
REQ_CONSUME(bytes, LDR_BODY_BYTES)
REQ_CONSUME(formData, LDR_BODY_FORMDATA)
REQ_CONSUME(json, LDR_BODY_JSON)
REQ_CONSUME(text, LDR_BODY_TEXT)

/* ------------------------------------------------------------------ Response */

static embi_js_value resp_wrap(embi_bind_ctx *c, uint8_t type, uint16_t status, embi_str url, embi_str text, bool redirected,
                               embi_js_value headers, embi_bind_obj **bo_out)
{
    size_t bytes = sizeof(resp_rec) + url.len + text.len;
    resp_rec *r = embi_alloc(bytes);
    *r = (resp_rec){0};
    r->type = type;
    r->status = status;
    r->url_len = url.len;
    r->text_len = text.len;
    r->redirected = redirected;
    if (url.len > 0) {
        memcpy(r->tail, url.ptr, url.len);
    }
    if (text.len > 0) {
        memcpy(r->tail + url.len, text.ptr, text.len);
    }
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_Response, r, r, (uint32_t)bytes);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_bind_set_ref(bo, embi_bind_array_new(c, B_N));
    slot_set(c, bo, B_HEADERS, headers);
    *bo_out = bo;
    return v;
}

embi_js_value ldr_response_wrap(embi_bind_ctx *c, embi_response *resp)
{
    (void)ldr_state_of(c);
    embi_headers shown = {0};
    (void)embi_headers_copy_filtered(&resp->headers, &shown, resp->type == EMBI_RESP_BASIC || resp->type == EMBI_RESP_CORS);
    embi_js_value hw = ldr_headers_new(c, &shown, EMBI_GUARD_IMMUTABLE);
    embi_headers_free(&shown);
    embi_bind_obj *bo;
    embi_js_value v = resp_wrap(c, (uint8_t)resp->type, resp->status, (embi_str){resp->url.data, resp->url.len},
                                (embi_str){resp->status_text.data, resp->status_text.len}, resp->redirected, hw, &bo);
    ((resp_rec *)bo->ptr)->has_body = resp->body.present;
    if (resp->body.present) {
        embi_js_value u8 = ldr_new_uint8(c, (const uint8_t *)resp->body.bytes.data, resp->body.bytes.len);
        if (u8 == EMBI_JS_EXCEPTION) {
            embi_response_free(resp);
            return u8;
        }
        slot_set(c, bo, B_BODY, u8);
    }
    embi_response_free(resp);
    return v;
}

void ldr_response_attach_stream(embi_bind_ctx *c, embi_js_value response, embi_js_value stream)
{
    embi_bind_obj *bo = embi_bind_obj_of(response);
    if (bo == nullptr || bo->iface != EMBI_IF_Response || embi_bind_stale(bo)) {
        return;
    }
    ((resp_rec *)bo->ptr)->has_body = true;
    slot_set(c, bo, B_STREAM, stream);
}

/* Reads the ResponseInit members. status defaults to 200. False with an exception pending. */
static bool response_init(embi_bind_ctx *c, embi_js_value init, uint32_t *status, embi_sb *text, embi_headers *hdrs)
{
    *status = 200;
    if (embi_js_is_nullish(init)) {
        return true;
    }
    if (!embi_js_is_object(init)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Response': The provided value is not of type 'ResponseInit'.");
        return false;
    }
    embi_js_value st = ldr_get(c, init, "status");
    embi_js_value tx = ldr_get(c, init, "statusText");
    embi_js_value hv = ldr_get(c, init, "headers");
    if (st == EMBI_JS_EXCEPTION || tx == EMBI_JS_EXCEPTION || hv == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (!embi_js_is_undefined(st)) {
        double d;
        if (!embi_js_to_number(c->rt, st, &d)) {
            return false;
        }
        if (d != d || d < 200 || d > 599 || d != (double)(uint32_t)d) {
            (void)embi_js_throw_range_error(c->rt, "Failed to construct 'Response': The status provided (%.0f) is outside the range [200, 599].", d);
            return false;
        }
        *status = (uint32_t)d;
    }
    if (!embi_js_is_undefined(tx)) {
        if (!ldr_to_utf8(c, tx, text)) {
            return false;
        }
        /* ByteString: a code point above U+00FF is a TypeError at the conversion. */
        bool bytes_ok = true;
        for (const char *q = text->data, *qe = text->len > 0 ? text->data + text->len : nullptr; qe != nullptr && q < qe;) {
            if (embi_utf8_decode(&q, qe) > 0xFF) {
                bytes_ok = false;
                break;
            }
        }
        if (!bytes_ok || !embi_status_text_ok((embi_str){text->data, text->len})) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Response': Invalid statusText");
            return false;
        }
    }
    return ldr_headers_fill(c, hdrs, hv, "Failed to construct 'Response'");
}

/* Builds a Response from a body init and a ResponseInit. */
static embi_js_value response_make(embi_bind_ctx *c, embi_js_value body, embi_js_value init, const char *default_ctype)
{
    uint32_t status;
    embi_sb text = {nullptr, 0, 0};
    embi_headers hdrs = {0};
    hdrs.guard = EMBI_GUARD_RESPONSE;
    embi_js_value result = EMBI_JS_EXCEPTION;
    if (response_init(c, init, &status, &text, &hdrs)) {
        if (!embi_js_is_nullish(body) && embi_status_null_body(status)) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'Response': Response with null body status cannot have body");
        } else {
            embi_js_value hw = ldr_headers_new(c, &hdrs, EMBI_GUARD_RESPONSE);
            embi_bind_obj *bo;
            embi_js_value v = resp_wrap(c, EMBI_RESP_DEFAULT, (uint16_t)status, EMBI_STR(""), (embi_str){text.data, text.len},
                                        false, hw, &bo);
            embi_sb ctype = {nullptr, 0, 0};
            if (default_ctype != nullptr) {
                embi_sb_append(&ctype, (embi_str){default_ctype, (uint32_t)strlen(default_ctype)});
            }
            bool has_body = false;
            bool ok = true;
            if (!embi_js_is_nullish(body)) {
                embi_sb ct2 = {nullptr, 0, 0};
                ok = set_body(c, bo, body, &has_body, &ct2);
                if (ok && ct2.len > 0 && default_ctype == nullptr) {
                    embi_sb_clear(&ctype);
                    embi_sb_append(&ctype, (embi_str){ct2.data, ct2.len});
                }
                embi_sb_free(&ct2);
            }
            if (ok) {
                ((resp_rec *)bo->ptr)->has_body = has_body;
                if (ctype.len > 0 && (has_body || default_ctype != nullptr)) {
                    embi_headers h;
                    if (ldr_headers_load(hw, &h)) {
                        if (!embi_headers_has(&h, EMBI_STR("content-type"))) {
                            (void)embi_headers_append(&h, EMBI_STR("content-type"), (embi_str){ctype.data, ctype.len});
                            ldr_headers_store(hw, &h);
                        }
                        embi_headers_free(&h);
                    }
                }
                result = v;
            }
            embi_sb_free(&ctype);
        }
    }
    embi_sb_free(&text);
    embi_headers_free(&hdrs);
    return result;
}

embi_js_value embi_impl_Response_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    (void)ldr_state_of(c);
    embi_js_value v = response_make(c, a0, a1, nullptr);
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

embi_js_value embi_impl_Response_error(embi_bind_ctx *c)
{
    (void)ldr_state_of(c);
    embi_headers h = {0};
    embi_js_value hw = ldr_headers_new(c, &h, EMBI_GUARD_IMMUTABLE);
    embi_bind_obj *bo;
    return resp_wrap(c, EMBI_RESP_ERROR, 0, EMBI_STR(""), EMBI_STR(""), false, hw, &bo);
}

embi_dom_err embi_impl_Response_redirect(embi_bind_ctx *c, embi_dstr a0, uint16_t a1, embi_js_value *out)
{
    (void)ldr_state_of(c);
    embi_sb href = {nullptr, 0, 0};
    if (!parse_request_url(c, embi_bind_utf8(c, a0), &href)) {
        return EMBI_BIND_THREW;
    }
    if (!embi_status_redirect(a1)) {
        embi_sb_free(&href);
        (void)embi_js_throw_range_error(c->rt, "Failed to execute 'redirect' on 'Response': Invalid status code");
        return EMBI_BIND_THREW;
    }
    embi_headers h = {0};
    embi_headers_add_raw(&h, EMBI_STR("location"), (embi_str){href.data, href.len});
    embi_js_value hw = ldr_headers_new(c, &h, EMBI_GUARD_IMMUTABLE);
    embi_headers_free(&h);
    embi_sb_free(&href);
    embi_bind_obj *bo;
    *out = resp_wrap(c, EMBI_RESP_DEFAULT, a1, EMBI_STR(""), EMBI_STR(""), false, hw, &bo);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Response_json_static(embi_bind_ctx *c, embi_js_value a0, embi_js_value a1, embi_js_value *out)
{
    (void)ldr_state_of(c);
    embi_js_value json = ldr_get(c, embi_js_from_object(c->realm->global), "JSON");
    if (json == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    embi_js_value text = embi_js_invoke(c->rt, json, embi_js_key_c(c->rt, "stringify"), 1, &a0);
    if (text == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    if (!embi_js_is_string(text)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'json' on 'Response': The value is not JSON serializable.");
        return EMBI_BIND_THREW;
    }
    *out = response_make(c, text, a1, "application/json");
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

#define RESP_GUARD(self) (embi_bind_stale(self) ? nullptr : (const resp_rec *)(self)->ptr)

embi_dstr embi_impl_Response_get_type(embi_bind_ctx *c, embi_bind_obj *self)
{
    const resp_rec *r = RESP_GUARD(self);
    return cstr(c, r != nullptr && r->type < COUNT(k_resp_type) ? k_resp_type[r->type] : "default");
}

embi_dstr embi_impl_Response_get_url(embi_bind_ctx *c, embi_bind_obj *self)
{
    const resp_rec *r = RESP_GUARD(self);
    if (r == nullptr) {
        return cstr(c, "");
    }
    /* The fragment is not part of the response URL's serialization with exclude-fragment true. */
    uint32_t n = r->url_len;
    for (uint32_t i = 0; i < n; i++) {
        if (r->tail[i] == '#') {
            n = i;
            break;
        }
    }
    return embi_bind_tmp_utf8(c, (embi_str){r->tail, n});
}

bool embi_impl_Response_get_redirected(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const resp_rec *r = RESP_GUARD(self);
    return r != nullptr && r->redirected;
}

uint16_t embi_impl_Response_get_status(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const resp_rec *r = RESP_GUARD(self);
    return r != nullptr ? r->status : 0;
}

bool embi_impl_Response_get_ok(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const resp_rec *r = RESP_GUARD(self);
    return r != nullptr && r->status >= 200 && r->status <= 299;
}

embi_dstr embi_impl_Response_get_statusText(embi_bind_ctx *c, embi_bind_obj *self)
{
    const resp_rec *r = RESP_GUARD(self);
    return r == nullptr ? cstr(c, "") : embi_bind_tmp_utf8(c, (embi_str){r->tail + r->url_len, r->text_len});
}

embi_js_value embi_impl_Response_get_headers(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_stale(self) ? EMBI_JS_UNDEFINED : slot(c, self, B_HEADERS);
}

embi_js_value embi_impl_Response_get_body(embi_bind_ctx *c, embi_bind_obj *self)
{
    return body_getter(c, self);
}

bool embi_impl_Response_get_bodyUsed(embi_bind_ctx *c, embi_bind_obj *self)
{
    return body_used(c, self);
}

embi_dom_err embi_impl_Response_clone(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    if (embi_bind_stale(self)) {
        *out = EMBI_JS_UNDEFINED;
        return EMBI_DOM_OK;
    }
    resp_rec *r = self->ptr;
    if (body_used(c, self) || (r->has_body && embi_js_is_undefined(slot(c, self, B_BODY)))) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'clone' on 'Response': Response body is already used or is a stream.");
        return EMBI_BIND_THREW;
    }
    embi_headers h;
    embi_js_value hw = EMBI_JS_UNDEFINED;
    if (ldr_headers_load(slot(c, self, B_HEADERS), &h)) {
        hw = ldr_headers_new(c, &h, (embi_guard)((embi_headers *)&h)->guard);
        embi_headers_free(&h);
    }
    embi_bind_obj *bo;
    *out = resp_wrap(c, r->type, r->status, (embi_str){r->tail, r->url_len}, (embi_str){r->tail + r->url_len, r->text_len},
                     r->redirected, hw, &bo);
    ((resp_rec *)bo->ptr)->has_body = r->has_body;
    slot_set(c, bo, B_BODY, slot(c, self, B_BODY));
    return EMBI_DOM_OK;
}

#define RESP_CONSUME(fn, kind)                                                              \
    embi_js_value embi_impl_Response_##fn(embi_bind_ctx *c, embi_bind_obj *self)            \
    {                                                                                       \
        return consume(c, self, kind);                                                      \
    }

RESP_CONSUME(arrayBuffer, LDR_BODY_ARRAYBUFFER)
RESP_CONSUME(blob, LDR_BODY_BLOB)
RESP_CONSUME(bytes, LDR_BODY_BYTES)
RESP_CONSUME(formData, LDR_BODY_FORMDATA)
RESP_CONSUME(json, LDR_BODY_JSON)
RESP_CONSUME(text, LDR_BODY_TEXT)
