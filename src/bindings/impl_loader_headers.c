/*
 * Headers (fetch.webidl, W7c). A Headers wrapper owns one flat block: the guard and the header list serialized
 * (name length, value length, name bytes, value bytes per entry). Every operation loads the list into an embi_headers
 * (src/loader/fetch.h, where the Fetch rules live), applies the change, and stores a new block over the old one, so the
 * wrapper's finalizer (which frees one block) is enough and nothing leaks. Header lists are short; the copies are cheap.
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"

typedef struct hdrs_rec {
    uint32_t guard;
    uint32_t count;
    uint32_t bytes; /* payload bytes */
    char payload[];
} hdrs_rec;

static size_t rec_size(uint32_t payload)
{
    return sizeof(hdrs_rec) + payload;
}

static uint32_t read_u32(const char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Serializes h into a new block. */
static hdrs_rec *rec_from(const embi_headers *h, embi_guard guard)
{
    size_t payload = 0;
    for (uint32_t i = 0; i < h->list.len; i++) {
        payload += 8u + h->list.data[i].name.len + h->list.data[i].value.len;
    }
    hdrs_rec *r = embi_alloc(rec_size((uint32_t)payload));
    r->guard = (uint32_t)guard;
    r->count = h->list.len;
    r->bytes = (uint32_t)payload;
    char *w = r->payload;
    for (uint32_t i = 0; i < h->list.len; i++) {
        const embi_kv *kv = &h->list.data[i];
        uint32_t nl = kv->name.len, vl = kv->value.len;
        memcpy(w, &nl, 4);
        memcpy(w + 4, &vl, 4);
        w += 8;
        if (nl > 0) {
            memcpy(w, kv->name.data, nl);
        }
        w += nl;
        if (vl > 0) {
            memcpy(w, kv->value.data, vl);
        }
        w += vl;
    }
    return r;
}

/* Loads the list of a record into out (zeroed, freed by the caller with embi_headers_free). */
static void rec_load(const hdrs_rec *r, embi_headers *out)
{
    *out = (embi_headers){0};
    out->guard = (embi_guard)r->guard;
    const char *p = r->payload;
    for (uint32_t i = 0; i < r->count; i++) {
        uint32_t nl = read_u32(p), vl = read_u32(p + 4);
        p += 8;
        embi_headers_add_raw(out, (embi_str){p, nl}, (embi_str){p + nl, vl});
        p += nl + vl;
    }
}

static embi_js_value rec_wrap(embi_bind_ctx *c, hdrs_rec *r)
{
    return embi_bind_other_value(c, EMBI_IF_Headers, r, r, (uint32_t)rec_size(r->bytes));
}

/* Replaces the block of bo by the serialization of h. */
static void rec_store(embi_bind_obj *bo, const embi_headers *h)
{
    hdrs_rec *old = bo->owned;
    uint32_t old_size = bo->owned_size;
    hdrs_rec *nr = rec_from(h, (embi_guard)old->guard);
    bo->ptr = nr;
    bo->owned = nr;
    bo->owned_size = (uint32_t)rec_size(nr->bytes);
    embi_free(old, old_size);
}

embi_js_value ldr_headers_new(embi_bind_ctx *c, const embi_headers *src, embi_guard guard)
{
    return rec_wrap(c, rec_from(src, guard));
}

bool ldr_headers_load(embi_js_value v, embi_headers *out)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || bo->iface != EMBI_IF_Headers || embi_bind_stale(bo)) {
        return false;
    }
    rec_load(bo->ptr, out);
    return true;
}

void ldr_headers_store(embi_js_value v, const embi_headers *h)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo != nullptr && bo->iface == EMBI_IF_Headers && !embi_bind_stale(bo)) {
        rec_store(bo, h);
    }
}

/* ------------------------------------------------------------------ init */

/* Representation: the header list holds UTF-8 bytes of the code points the script gave (so get() returns them unchanged,
 * and U+00E9 comes back as "é"). Fetch defines a header value as bytes, so a value of U+0080..U+00FF is stored as the
 * UTF-8 of that code point, not the single byte Fetch would send. Serialization to the wire is a later task (the
 * request/response layer), and it must convert code points to bytes there. */
/* A ByteString (WebIDL): every code point is at most U+00FF, or the conversion throws a TypeError. */
static bool is_bytestring(embi_str s)
{
    if (s.len == 0) {
        return true;
    }
    const char *p = s.ptr, *end = s.ptr + s.len;
    while (p < end) {
        if (embi_utf8_decode(&p, end) > 0xFF) {
            return false;
        }
    }
    return true;
}

static bool throw_bytestring(embi_bind_ctx *c, const char *what)
{
    (void)embi_js_throw_type_error(c->rt, "%s: Invalid name or value", what);
    return false;
}

static bool throw_status(embi_bind_ctx *c, embi_hdr_status st, const char *what)
{
    if (st == EMBI_HDR_OK) {
        return true;
    }
    (void)embi_js_throw_type_error(c->rt, "%s: %s", what,
                                   st == EMBI_HDR_IMMUTABLE ? "Headers are immutable" : "Invalid name or value");
    return false;
}

bool ldr_headers_fill(embi_bind_ctx *c, embi_headers *h, embi_js_value init, const char *what)
{
    if (embi_js_is_undefined(init)) {
        return true;
    }
    if (!embi_js_is_object(init)) {
        (void)embi_js_throw_type_error(c->rt, "%s: The provided value is not of type '(record<ByteString, ByteString> or sequence<sequence<ByteString>>)'.", what);
        return false;
    }
    /* A Headers object is a sequence of pairs like any other iterable: its own @@iterator (which a script may
     * replace) decides what the init yields, so the record shortcut is not taken here. */
    embi_js_key itkey = ldr_wk_key(c, EMBI_JS_WK_ITERATOR);
    embi_js_value method = embi_js_get_method(c->rt, init, itkey);
    if (method == EMBI_JS_EXCEPTION) {
        return false;
    }
    if (!embi_js_is_undefined(method)) {
        embi_js_value *items = nullptr;
        uint32_t n = 0;
        if (!embi_bind_seq_open(c, init, what, &items, &n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_js_value *pair = nullptr;
            uint32_t pn = 0;
            if (!embi_js_is_object(items[i]) || !embi_bind_seq_open(c, items[i], what, &pair, &pn)) {
                if (!embi_js_is_object(items[i])) {
                    (void)embi_js_throw_type_error(c->rt, "%s: The provided value cannot be converted to a sequence.", what);
                }
                return false;
            }
            if (pn != 2) {
                (void)embi_js_throw_type_error(c->rt, "%s: Invalid value", what);
                return false;
            }
            embi_sb name = {nullptr, 0, 0}, value = {nullptr, 0, 0};
            bool ok = ldr_to_utf8(c, pair[0], &name) && ldr_to_utf8(c, pair[1], &value);
            if (ok && !(is_bytestring((embi_str){name.data, name.len}) && is_bytestring((embi_str){value.data, value.len}))) {
                ok = throw_bytestring(c, what);
            }
            if (ok) {
                ok = throw_status(c, embi_headers_append(h, (embi_str){name.data, name.len}, (embi_str){value.data, value.len}),
                                  what);
            }
            embi_sb_free(&name);
            embi_sb_free(&value);
            if (!ok) {
                return false;
            }
        }
        return true;
    }
    /* record<ByteString, ByteString> (WebIDL 3.2.20): [[OwnPropertyKeys]], then per key [[GetOwnProperty]], and for an
     * enumerable one [[Get]] and the conversions, all before the next key, so a bad name or value stops the walk. A
     * symbol key that is enumerable fails ToString with a TypeError; a non-enumerable one is skipped. */
    embi_js_object *o = embi_js_as_object(init);
    embi_js_key_vec ks = {0};
    if (embi_js_obj_own_keys(c->rt, o, &ks) != EMBI_JS_YES) {
        embi_vec_free(&ks);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; i < ks.len && ok; i++) {
        embi_js_key k = ks.data[i];
        embi_js_pdesc d;
        int r = embi_js_obj_get_own_property(c->rt, o, k, &d);
        if (r == EMBI_JS_THREW) {
            ok = false;
            break;
        }
        if (r != EMBI_JS_YES || !(d.attrs & EMBI_JS_ATTR_ENUMERABLE)) {
            continue;
        }
        /* The name converts (and may throw) before the value is read. */
        embi_js_value kv = embi_bind_pin(c, embi_js_key_to_value(c->rt, k));
        embi_sb name = {nullptr, 0, 0}, value = {nullptr, 0, 0};
        ok = ldr_to_utf8(c, kv, &name);
        if (ok && !is_bytestring((embi_str){name.data, name.len})) {
            ok = throw_bytestring(c, what);
        }
        if (ok) {
            embi_js_value val = embi_js_obj_get(c->rt, o, k, init);
            if (val == EMBI_JS_EXCEPTION) {
                ok = false;
            } else {
                embi_bind_pin(c, val);
                ok = ldr_to_utf8(c, val, &value);
                if (ok && !is_bytestring((embi_str){value.data, value.len})) {
                    ok = throw_bytestring(c, what);
                }
            }
        }
        if (ok) {
            ok = throw_status(c, embi_headers_append(h, (embi_str){name.data, name.len}, (embi_str){value.data, value.len}),
                              what);
        }
        embi_sb_free(&name);
        embi_sb_free(&value);
    }
    embi_vec_free(&ks);
    return ok;
}

embi_js_value embi_impl_Headers_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    (void)ldr_state_of(c);
    embi_headers h = {0};
    h.guard = EMBI_GUARD_NONE;
    embi_js_value v = EMBI_JS_EXCEPTION;
    if (ldr_headers_fill(c, &h, a0, "Failed to construct 'Headers'")) {
        v = ldr_headers_new(c, &h, EMBI_GUARD_NONE);
        embi_bind_adopt_new_target(c, v, new_target);
    }
    embi_headers_free(&h);
    return embi_bind_leave(sc, v);
}

/* ------------------------------------------------------------------ members */

/* Runs a change on the headers of self. */
typedef embi_hdr_status (*change_fn)(embi_headers *h, embi_str name, embi_str value);

static embi_dom_err change(embi_bind_ctx *c, embi_bind_obj *self, change_fn fn, const char *op, embi_dstr name,
                           embi_dstr value)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_headers h;
    rec_load(self->ptr, &h);
    char what[64];
    SDL_snprintf(what, sizeof what, "Failed to execute '%s' on 'Headers'", op);
    embi_str n = embi_bind_utf8(c, name), v = embi_bind_utf8(c, value);
    if (!is_bytestring(n) || !is_bytestring(v)) {
        embi_headers_free(&h);
        return throw_bytestring(c, what) ? EMBI_DOM_OK : EMBI_BIND_THREW;
    }
    embi_hdr_status st = fn(&h, n, v);
    if (st == EMBI_HDR_OK) {
        rec_store(self, &h);
    }
    embi_headers_free(&h);
    return throw_status(c, st, what) ? EMBI_DOM_OK : EMBI_BIND_THREW;
}

static embi_hdr_status do_append(embi_headers *h, embi_str n, embi_str v)
{
    return embi_headers_append(h, n, v);
}

static embi_hdr_status do_set(embi_headers *h, embi_str n, embi_str v)
{
    return embi_headers_set(h, n, v);
}

static embi_hdr_status do_delete(embi_headers *h, embi_str n, embi_str v)
{
    (void)v;
    return embi_headers_delete(h, n);
}

embi_dom_err embi_impl_Headers_append(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    return change(c, self, do_append, "append", a0, a1);
}

embi_dom_err embi_impl_Headers_set(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    return change(c, self, do_set, "set", a0, a1);
}

embi_dom_err embi_impl_Headers_delete(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    return change(c, self, do_delete, "delete", a0, embi_dstr_empty());
}

embi_dom_err embi_impl_Headers_get(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value *out)
{
    *out = EMBI_JS_NULL;
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_str name = embi_bind_utf8(c, a0);
    if (!embi_header_name_ok(name)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'get' on 'Headers': Invalid name");
        return EMBI_BIND_THREW;
    }
    embi_headers h;
    rec_load(self->ptr, &h);
    embi_sb v = {nullptr, 0, 0};
    if (embi_headers_get(&h, name, &v)) {
        *out = embi_bind_utf8_value(c, (embi_str){v.data, v.len});
    }
    embi_sb_free(&v);
    embi_headers_free(&h);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Headers_has(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool *out)
{
    *out = false;
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_str name = embi_bind_utf8(c, a0);
    if (!embi_header_name_ok(name)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'has' on 'Headers': Invalid name");
        return EMBI_BIND_THREW;
    }
    embi_headers h;
    rec_load(self->ptr, &h);
    *out = embi_headers_has(&h, name);
    embi_headers_free(&h);
    return EMBI_DOM_OK;
}

embi_bind_seq embi_impl_Headers_getSetCookie(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_seq out = {nullptr, 0};
    if (embi_bind_stale(self)) {
        return out;
    }
    embi_headers h;
    rec_load(self->ptr, &h);
    uint32_t n = 0;
    for (uint32_t i = 0; i < h.list.len; i++) {
        n += embi_str_eq((embi_str){h.list.data[i].name.data, h.list.data[i].name.len}, EMBI_STR("set-cookie")) ? 1u : 0u;
    }
    if (n > 0) {
        embi_dstr *arr = (embi_dstr *)embi_bind_alloc(c, (size_t)n * sizeof *arr);
        uint32_t k = 0;
        for (uint32_t i = 0; i < h.list.len; i++) {
            if (embi_str_eq((embi_str){h.list.data[i].name.data, h.list.data[i].name.len}, EMBI_STR("set-cookie"))) {
                arr[k++] = embi_bind_tmp_utf8(c, (embi_str){h.list.data[i].value.data, h.list.data[i].value.len});
            }
        }
        out.data = arr;
        out.len = n;
    }
    embi_headers_free(&h);
    return out;
}

/* The sorted-and-combined list (the iteration order) of self. */
static bool sorted_of(embi_bind_obj *self, embi_headers *out)
{
    *out = (embi_headers){0};
    if (embi_bind_stale(self)) {
        return false;
    }
    embi_headers h;
    rec_load(self->ptr, &h);
    bool ok = embi_headers_sorted(&h, out);
    embi_headers_free(&h);
    return ok;
}

uint32_t embi_impl_Headers_iter_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_headers s;
    (void)sorted_of(self, &s);
    uint32_t n = s.list.len;
    embi_headers_free(&s);
    return n;
}

static embi_js_value iter_part(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i, bool value)
{
    embi_headers s;
    embi_js_value v = embi_bind_utf8_value(c, EMBI_STR(""));
    if (sorted_of(self, &s) && i < s.list.len) {
        embi_sb *part = value ? &s.list.data[i].value : &s.list.data[i].name;
        v = embi_bind_utf8_value(c, (embi_str){part->data, part->len});
    }
    embi_headers_free(&s);
    return v;
}

embi_js_value embi_impl_Headers_iter_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return iter_part(c, self, i, false);
}

embi_js_value embi_impl_Headers_iter_value(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return iter_part(c, self, i, true);
}

void ldr_headers_copy(const embi_headers *src, embi_headers *dst)
{
    for (uint32_t i = 0; i < src->list.len; i++) {
        const embi_kv *kv = &src->list.data[i];
        embi_headers_add_raw(dst, (embi_str){kv->name.data, kv->name.len}, (embi_str){kv->value.data, kv->value.len});
    }
}
