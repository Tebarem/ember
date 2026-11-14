/*
 * URL and URLSearchParams (url.webidl, W7c).
 *
 * A URL record is one block: the embi_url and its href stored inline. The wrapper's finalizer frees only the owned
 * block, so the block must hold every byte of the record. A setter parses into a heap copy, then replaces the block
 * (the old one is freed here).
 *
 * A URLSearchParams is either standalone, keeping its query as a JS string in the wrapper's ref slot (traced), or
 * linked to the searchParams of a URL: it then reads and writes that URL's query, and keeps the URL wrapper alive
 * through the ref slot.
 */
#include "bindings/impl.h"
#include "bindings/gen/url.h"

#include "loader/loader.h"

#include <string.h>

/* The URL record: the embi_url and its href bytes (NUL-terminated), inline. */
typedef struct embi_url_rec {
    embi_url u;
    char href[];
} embi_url_rec;

/* The URLSearchParams record: the URL it is linked to (nullptr when standalone). */
typedef struct embi_usp {
    embi_bind_obj *url;
} embi_usp;

static size_t rec_size(uint32_t href_len)
{
    return sizeof(embi_url_rec) + (size_t)href_len + 1u;
}

static embi_url_rec *rec_new(const embi_url *src)
{
    embi_url_rec *r = embi_alloc(rec_size(src->href_len));
    r->u = *src;
    r->u.href = r->href;
    if (src->href_len > 0) {
        memcpy(r->href, src->href, src->href_len);
    }
    r->href[src->href_len] = '\0';
    return r;
}

/* Replaces the record of bo by a copy of u (the old block is freed). */
static void url_replace(embi_bind_obj *bo, const embi_url *u)
{
    embi_url_rec *old = bo->owned;
    uint32_t old_size = bo->owned_size;
    embi_url_rec *nr = rec_new(u);
    bo->ptr = nr;
    bo->owned = nr;
    bo->owned_size = (uint32_t)rec_size(u->href_len);
    embi_free(old, old_size);
}

static embi_js_value url_value(embi_bind_ctx *c, const embi_url *u)
{
    embi_url_rec *r = rec_new(u);
    return embi_bind_other_value(c, EMBI_IF_URL, r, r, (uint32_t)rec_size(u->href_len));
}

/* Parses input against the optional base argument (undefined: none). Returns 0 for a URL in out, 1 when the input
 * or the base is not a URL, and 2 when a conversion threw. */
static int url_parse_args(embi_bind_ctx *c, embi_dstr input, embi_js_value base_v, embi_url *out)
{
    *out = (embi_url){0};
    embi_url base = {0};
    bool has_base = !embi_js_is_undefined(base_v);
    if (has_base) {
        embi_dstr bd;
        if (!embi_bind_to_dstr(c, base_v, &bd)) {
            return 2;
        }
        if (!embi_url_parse(embi_bind_utf8(c, bd), nullptr, &base)) {
            return 1;
        }
    }
    bool ok = embi_url_parse(embi_bind_utf8(c, input), has_base ? &base : nullptr, out);
    if (has_base) {
        embi_url_free(&base);
    }
    return ok ? 0 : 1;
}

static embi_dstr url_get(embi_bind_ctx *c, embi_bind_obj *self, embi_url_attr attr)
{
    if (embi_bind_stale(self)) {
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    embi_url_rec *r = self->ptr;
    embi_sb sb = {nullptr, 0, 0};
    embi_url_get(&r->u, attr, &sb);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

/* Applies the setter. False when the value did not parse for the attribute (the href setter throws then; the others
 * keep the URL as it was, as the URL Standard says). */
static bool url_set(embi_bind_ctx *c, embi_bind_obj *self, embi_url_attr attr, embi_dstr v)
{
    if (embi_bind_stale(self)) {
        return true;
    }
    embi_url_rec *r = self->ptr;
    embi_url tmp = {0};
    if (!embi_url_copy(&r->u, &tmp)) {
        return false;
    }
    bool ok = embi_url_set(&tmp, attr, embi_bind_utf8(c, v));
    if (ok) {
        url_replace(self, &tmp);
    }
    embi_url_free(&tmp);
    return ok;
}

/* ------------------------------------------------------------------ URL */

embi_js_value embi_impl_URL_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_url u = {0};
    int st = url_parse_args(c, a0, a1, &u);
    if (st != 0) {
        if (st == 1) {
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'URL': Invalid URL");
        }
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value v = url_value(c, &u);
    embi_url_free(&u);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

bool embi_impl_URL_canParse(embi_bind_ctx *c, embi_dstr a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_url u = {0};
    int st = url_parse_args(c, a0, a1, &u);
    if (st == 0) {
        embi_url_free(&u);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return st == 0;
}

embi_js_value embi_impl_URL_parse(embi_bind_ctx *c, embi_dstr a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_url u = {0};
    int st = url_parse_args(c, a0, a1, &u);
    if (st == 2) {
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (st != 0) {
        return embi_bind_leave(sc, EMBI_JS_NULL);
    }
    embi_js_value v = url_value(c, &u);
    embi_url_free(&u);
    return embi_bind_leave(sc, v);
}

/* [Throws] on href: the getter writes its result through out (it never fails). */
embi_dom_err embi_impl_URL_get_href(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr *out)
{
    *out = url_get(c, self, EMBI_URL_HREF);
    return EMBI_DOM_OK;
}
embi_dom_err embi_impl_URL_set_href(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    /* The href setter parses the whole input: an invalid URL is a TypeError (URL Standard, the href setter). */
    if (!url_set(c, self, EMBI_URL_HREF, a0)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to set the 'href' property on 'URL': Invalid URL");
        return EMBI_BIND_THREW;
    }
    return EMBI_DOM_OK;
}

embi_dstr embi_impl_URL_get_origin(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    embi_url_rec *r = self->ptr;
    embi_sb sb = {nullptr, 0, 0};
    embi_url_origin(&r->u, &sb);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

embi_dstr embi_impl_URL_get_protocol(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_PROTOCOL); }
void embi_impl_URL_set_protocol(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_PROTOCOL, a0);
}
embi_dstr embi_impl_URL_get_username(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_USERNAME); }
void embi_impl_URL_set_username(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_USERNAME, a0);
}
embi_dstr embi_impl_URL_get_password(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_PASSWORD); }
void embi_impl_URL_set_password(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_PASSWORD, a0);
}
embi_dstr embi_impl_URL_get_host(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_HOST); }
void embi_impl_URL_set_host(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_HOST, a0);
}
embi_dstr embi_impl_URL_get_hostname(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_HOSTNAME); }
void embi_impl_URL_set_hostname(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_HOSTNAME, a0);
}
embi_dstr embi_impl_URL_get_port(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_PORT); }
void embi_impl_URL_set_port(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_PORT, a0);
}
embi_dstr embi_impl_URL_get_pathname(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_PATHNAME); }
void embi_impl_URL_set_pathname(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_PATHNAME, a0);
}
embi_dstr embi_impl_URL_get_search(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_SEARCH); }
void embi_impl_URL_set_search(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_SEARCH, a0);
}
embi_dstr embi_impl_URL_get_hash(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_HASH); }
void embi_impl_URL_set_hash(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    url_set(c, self, EMBI_URL_HASH, a0);
}

embi_js_value embi_impl_URL_get_searchParams(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_usp *usp = embi_alloc(sizeof *usp);
    usp->url = self;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_URLSearchParams, usp, usp, sizeof *usp);
    embi_bind_obj *ub = embi_bind_obj_of(v);
    if (ub != nullptr) {
        /* The URL stays alive while the linked search params do. */
        embi_bind_set_ref(ub, embi_bind_obj_value(self));
    }
    return embi_bind_leave(sc, v);
}

embi_dstr embi_impl_URL_stringifier(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_HREF); }
embi_dstr embi_impl_URL_toJSON(embi_bind_ctx *c, embi_bind_obj *self) { return url_get(c, self, EMBI_URL_HREF); }

/* ------------------------------------------------------------------ URLSearchParams: state */

static embi_usp *usp_of(embi_bind_obj *self)
{
    return self->ptr;
}

/* The query of the search params as UTF-8, without the '?'. */
static void usp_query(embi_bind_ctx *c, embi_bind_obj *self, embi_sb *out)
{
    embi_usp *u = usp_of(self);
    if (u->url != nullptr) {
        if (embi_bind_stale(u->url)) {
            return;
        }
        embi_url_rec *r = u->url->ptr;
        embi_url_get(&r->u, EMBI_URL_SEARCH, out);
        if (out->len > 0 && out->data[0] == '?') {
            memmove(out->data, out->data + 1, out->len - 1);
            out->len--;
        }
        return;
    }
    embi_dstr d;
    if (self->ref != 0 && embi_bind_to_dstr(c, self->ref, &d)) {
        embi_sb_append(out, embi_bind_utf8(c, d));
    }
}

/* Stores the query (UTF-8, without '?') into the linked URL or the standalone state. */
static void usp_store(embi_bind_ctx *c, embi_bind_obj *self, const embi_sb *query)
{
    embi_usp *u = usp_of(self);
    if (u->url != nullptr) {
        if (embi_bind_stale(u->url)) {
            return;
        }
        embi_url_rec *r = u->url->ptr;
        embi_url tmp = {0};
        if (!embi_url_copy(&r->u, &tmp)) {
            return;
        }
        embi_sb s = {nullptr, 0, 0};
        if (query->len > 0) {
            embi_sb_append_char(&s, '?');
            embi_sb_append(&s, (embi_str){query->data, query->len});
        }
        if (embi_url_set(&tmp, EMBI_URL_SEARCH, (embi_str){s.data, s.len})) {
            url_replace(u->url, &tmp);
        }
        embi_url_free(&tmp);
        embi_sb_free(&s);
        return;
    }
    embi_js_value sv = embi_bind_utf8_value(c, (embi_str){query->data, query->len});
    embi_bind_set_ref(self, sv);
}

static void usp_form(embi_bind_ctx *c, embi_bind_obj *self, embi_form *f)
{
    embi_sb q = {nullptr, 0, 0};
    usp_query(c, self, &q);
    embi_form_parse((embi_str){q.data, q.len}, f);
    embi_sb_free(&q);
}

static void usp_save(embi_bind_ctx *c, embi_bind_obj *self, const embi_form *f)
{
    embi_sb q = {nullptr, 0, 0};
    embi_form_serialize(f, &q);
    usp_store(c, self, &q);
    embi_sb_free(&q);
}

static void form_push(embi_form *f, embi_str name, embi_str value)
{
    embi_vec_reserve(f, f->len + 1u);
    embi_form_pair *p = &f->data[f->len++];
    p->name = (embi_sb){nullptr, 0, 0};
    p->value = (embi_sb){nullptr, 0, 0};
    embi_sb_append(&p->name, name);
    embi_sb_append(&p->value, value);
}

static bool name_is(const embi_form_pair *p, embi_str name)
{
    return embi_str_eq((embi_str){p->name.data, p->name.len}, name);
}

static bool value_is(const embi_form_pair *p, embi_str value)
{
    return embi_str_eq((embi_str){p->value.data, p->value.len}, value);
}

static void form_remove_at(embi_form *f, uint32_t i)
{
    embi_sb_free(&f->data[i].name);
    embi_sb_free(&f->data[i].value);
    for (uint32_t k = i + 1; k < f->len; k++) {
        f->data[k - 1] = f->data[k];
    }
    f->len--;
}

/* Converts a JS value to a UTF-8 string into the scratch of the call. Returns false with a pending exception. */
static bool to_utf8(embi_bind_ctx *c, embi_js_value v, embi_str *out)
{
    embi_dstr d;
    if (!embi_bind_to_dstr(c, v, &d)) {
        return false;
    }
    *out = embi_bind_utf8(c, d);
    return true;
}

/* ------------------------------------------------------------------ URLSearchParams */

embi_js_value embi_impl_URLSearchParams_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_form f = {0};
    bool iterable = false;
    if (embi_js_is_object(a0)) {
        embi_js_value m = embi_js_get_method(c->rt, a0, embi_js_wk_sym(c->rt, EMBI_JS_WK_ITERATOR));
        if (m == EMBI_JS_EXCEPTION) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        iterable = !embi_js_is_undefined(m);
    }
    if (iterable) {
        embi_js_value *items = nullptr;
        uint32_t n = 0;
        if (!embi_bind_seq_open(c, a0, "URLSearchParams", &items, &n)) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_js_value *pair = nullptr;
            uint32_t pn = 0;
            if (!embi_bind_seq_open(c, items[i], "URLSearchParams", &pair, &pn)) {
                embi_form_free(&f);
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            if (pn != 2) {
                embi_form_free(&f);
                (void)embi_js_throw_type_error(c->rt, "Failed to construct 'URLSearchParams': Each query pair must be an iterable [name, value] tuple");
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            embi_str nm;
            embi_str vl;
            if (!to_utf8(c, pair[0], &nm) || !to_utf8(c, pair[1], &vl)) {
                embi_form_free(&f);
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            form_push(&f, nm, vl);
        }
    } else if (embi_js_is_object(a0)) {
        embi_dstr *keys = nullptr;
        embi_js_value *vals = nullptr;
        uint32_t n = 0;
        if (!embi_bind_rec_open(c, a0, "URLSearchParams", &keys, &vals, &n)) {
            embi_form_free(&f);
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_str vl;
            if (!to_utf8(c, vals[i], &vl)) {
                embi_form_free(&f);
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            form_push(&f, embi_bind_utf8(c, keys[i]), vl);
        }
    } else if (!embi_js_is_undefined(a0)) {
        embi_str s;
        if (!to_utf8(c, a0, &s)) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        embi_form_parse(s, &f);
    }
    embi_sb q = {nullptr, 0, 0};
    embi_form_serialize(&f, &q);
    embi_form_free(&f);
    embi_usp *usp = embi_alloc(sizeof *usp);
    usp->url = nullptr;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_URLSearchParams, usp, usp, sizeof *usp);
    embi_bind_pin(c, v);
    embi_bind_set_ref(embi_bind_obj_of(v), embi_bind_utf8_value(c, (embi_str){q.data, q.len}));
    embi_sb_free(&q);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

void embi_impl_URLSearchParams_append(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    if (embi_bind_stale(self)) {
        return;
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    form_push(&f, embi_bind_utf8(c, a0), embi_bind_utf8(c, a1));
    usp_save(c, self, &f);
    embi_form_free(&f);
}

void embi_impl_URLSearchParams_delete(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1)
{
    if (embi_bind_stale(self)) {
        return;
    }
    embi_str name = embi_bind_utf8(c, a0);
    bool has_value = !embi_js_is_undefined(a1);
    embi_str value = {nullptr, 0};
    if (has_value && !to_utf8(c, a1, &value)) {
        return;
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    for (uint32_t i = 0; i < f.len;) {
        if (name_is(&f.data[i], name) && (!has_value || value_is(&f.data[i], value))) {
            form_remove_at(&f, i);
        } else {
            i++;
        }
    }
    usp_save(c, self, &f);
    embi_form_free(&f);
}

embi_js_value embi_impl_URLSearchParams_get(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (embi_bind_stale(self)) {
        return EMBI_JS_NULL;
    }
    embi_str name = embi_bind_utf8(c, a0);
    embi_form f = {0};
    usp_form(c, self, &f);
    embi_js_value r = EMBI_JS_NULL;
    for (uint32_t i = 0; i < f.len; i++) {
        if (name_is(&f.data[i], name)) {
            r = embi_bind_utf8_value(c, (embi_str){f.data[i].value.data, f.data[i].value.len});
            break;
        }
    }
    embi_form_free(&f);
    return r;
}

embi_bind_seq embi_impl_URLSearchParams_getAll(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_bind_seq out = {nullptr, 0};
    if (embi_bind_stale(self)) {
        return out;
    }
    embi_str name = embi_bind_utf8(c, a0);
    embi_form f = {0};
    usp_form(c, self, &f);
    uint32_t n = 0;
    for (uint32_t i = 0; i < f.len; i++) {
        n += name_is(&f.data[i], name) ? 1u : 0u;
    }
    if (n > 0) {
        embi_dstr *arr = (embi_dstr *)embi_bind_alloc(c, (size_t)n * sizeof *arr);
        uint32_t k = 0;
        for (uint32_t i = 0; i < f.len; i++) {
            if (name_is(&f.data[i], name)) {
                arr[k++] = embi_bind_tmp_utf8(c, (embi_str){f.data[i].value.data, f.data[i].value.len});
            }
        }
        out.data = arr;
        out.len = n;
    }
    embi_form_free(&f);
    return out;
}

bool embi_impl_URLSearchParams_has(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1)
{
    if (embi_bind_stale(self)) {
        return false;
    }
    embi_str name = embi_bind_utf8(c, a0);
    bool has_value = !embi_js_is_undefined(a1);
    embi_str value = {nullptr, 0};
    if (has_value && !to_utf8(c, a1, &value)) {
        return false;
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    bool found = false;
    for (uint32_t i = 0; i < f.len && !found; i++) {
        found = name_is(&f.data[i], name) && (!has_value || value_is(&f.data[i], value));
    }
    embi_form_free(&f);
    return found;
}

void embi_impl_URLSearchParams_set(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr a1)
{
    if (embi_bind_stale(self)) {
        return;
    }
    embi_str name = embi_bind_utf8(c, a0);
    embi_str value = embi_bind_utf8(c, a1);
    embi_form f = {0};
    usp_form(c, self, &f);
    /* The first pair with the name takes the value; the others with the name go. */
    uint32_t first = UINT32_MAX;
    for (uint32_t i = 0; i < f.len; i++) {
        if (name_is(&f.data[i], name)) {
            first = i;
            break;
        }
    }
    if (first == UINT32_MAX) {
        form_push(&f, name, value);
    } else {
        embi_sb_clear(&f.data[first].value);
        embi_sb_append(&f.data[first].value, value);
        for (uint32_t i = first + 1; i < f.len;) {
            if (name_is(&f.data[i], name)) {
                form_remove_at(&f, i);
            } else {
                i++;
            }
        }
    }
    usp_save(c, self, &f);
    embi_form_free(&f);
}

/* The UTF-16 code units of a UTF-8 string, one at a time (a supplementary character gives its two surrogates). */
typedef struct u16_iter {
    const unsigned char *p;
    uint32_t n;
    uint32_t i;
    uint32_t lo;
    bool has_lo;
} u16_iter;

static bool u16_next(u16_iter *it, uint32_t *unit)
{
    if (it->has_lo) {
        *unit = it->lo;
        it->has_lo = false;
        return true;
    }
    if (it->i >= it->n) {
        return false;
    }
    uint32_t c = it->p[it->i];
    uint32_t len = 1, cp = c;
    if (c >= 0xF0 && it->i + 4 <= it->n) {
        len = 4;
        cp = ((c & 0x07u) << 18) | ((it->p[it->i + 1] & 0x3Fu) << 12) | ((it->p[it->i + 2] & 0x3Fu) << 6) |
             (it->p[it->i + 3] & 0x3Fu);
    } else if (c >= 0xE0 && it->i + 3 <= it->n) {
        len = 3;
        cp = ((c & 0x0Fu) << 12) | ((it->p[it->i + 1] & 0x3Fu) << 6) | (it->p[it->i + 2] & 0x3Fu);
    } else if (c >= 0xC0 && it->i + 2 <= it->n) {
        len = 2;
        cp = ((c & 0x1Fu) << 6) | (it->p[it->i + 1] & 0x3Fu);
    }
    it->i += len;
    if (cp >= 0x10000u) {
        cp -= 0x10000u;
        *unit = 0xD800u + (cp >> 10);
        it->lo = 0xDC00u + (cp & 0x3FFu);
        it->has_lo = true;
    } else {
        *unit = cp;
    }
    return true;
}

/* Compares two UTF-8 names by UTF-16 code units (the URL Standard's sort order): <0, 0 or >0. */
static int name_cmp_u16(embi_str a, embi_str b)
{
    u16_iter ia = {(const unsigned char *)a.ptr, a.len, 0, 0, false};
    u16_iter ib = {(const unsigned char *)b.ptr, b.len, 0, 0, false};
    for (;;) {
        uint32_t ua = 0, ub = 0;
        bool ha = u16_next(&ia, &ua);
        bool hb = u16_next(&ib, &ub);
        if (!ha || !hb) {
            return ha ? 1 : (hb ? -1 : 0);
        }
        if (ua != ub) {
            return ua < ub ? -1 : 1;
        }
    }
}

/* Sorts the pairs by name: a stable insertion sort in UTF-16 code unit order, case-sensitive (URL Standard,
 * URLSearchParams sort). */
void embi_impl_URLSearchParams_sort(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return;
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    for (uint32_t i = 1; i < f.len; i++) {
        embi_form_pair cur = f.data[i];
        uint32_t j = i;
        while (j > 0 && name_cmp_u16((embi_str){f.data[j - 1].name.data, f.data[j - 1].name.len},
                                     (embi_str){cur.name.data, cur.name.len}) > 0) {
            f.data[j] = f.data[j - 1];
            j--;
        }
        f.data[j] = cur;
    }
    usp_save(c, self, &f);
    embi_form_free(&f);
}

embi_dstr embi_impl_URLSearchParams_stringifier(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_sb q = {nullptr, 0, 0};
    if (!embi_bind_stale(self)) {
        embi_form f = {0};
        usp_form(c, self, &f);
        embi_form_serialize(&f, &q);
        embi_form_free(&f);
    }
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){q.data, q.len});
    embi_sb_free(&q);
    return d;
}

uint32_t embi_impl_URLSearchParams_get_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (embi_bind_stale(self)) {
        return 0;
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    uint32_t n = f.len;
    embi_form_free(&f);
    return n;
}

/* ------------------------------------------------------------------ URLSearchParams: iterable<USVString, USVString> */

uint32_t embi_impl_URLSearchParams_iter_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_URLSearchParams_get_size(c, self);
}

/* The name and the value of pair i (the pair must exist: the iterator checks the size first). */
static embi_js_value usp_pair_part(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i, bool value)
{
    if (embi_bind_stale(self)) {
        return embi_bind_utf8_value(c, EMBI_STR(""));
    }
    embi_form f = {0};
    usp_form(c, self, &f);
    embi_js_value r = embi_bind_utf8_value(c, EMBI_STR(""));
    if (i < f.len) {
        const embi_sb *part = value ? &f.data[i].value : &f.data[i].name;
        r = embi_bind_utf8_value(c, (embi_str){part->data, part->len});
    }
    embi_form_free(&f);
    return r;
}

embi_js_value embi_impl_URLSearchParams_iter_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return usp_pair_part(c, self, i, false);
}

embi_js_value embi_impl_URLSearchParams_iter_value(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return usp_pair_part(c, self, i, true);
}
