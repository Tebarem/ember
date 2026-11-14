/*
 * Window, Location, History, Screen, Navigator and MediaQueryList (window.webidl).
 *
 * The history is an in-memory list of entries (URL and state) in the context. pushState and
 * replaceState change the current entry; back, forward and go move along the list and fire
 * popstate synchronously (W5c turns this into a task once the event loop exists). Location
 * assignments that leave the document are passed to the embedder's navigate hook. A state is
 * stored as its structured serialization (W7d), so the page keeps a copy and history.state and
 * popstate hand out a value made from the bytes.
 */
#include "bindings/impl.h"
#include "bindings/impl_input.h"
#include "bindings/impl_loader.h"
#include "bindings/gen/window.h"
#include "bindings/gen/eventtarget.h"
#include "css/css.h"
#include "loader/clone.h"
#include "layout/layout.h"
#include "ember/ember.h"
#include "style/style.h"

/* ------------------------------------------------------------------ URL helpers */

typedef struct url_parts {
    embi_str scheme;   /* without the colon */
    embi_str host;     /* hostname */
    embi_str port;
    embi_str path;
    embi_str query;    /* without the question mark */
    embi_str frag;     /* without the number sign */
    bool has_query, has_frag, has_authority;
} url_parts;

static bool is_alpha(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static url_parts url_parse(embi_str u)
{
    url_parts p = {0};
    uint32_t i = 0;
    uint32_t end = u.len;
    /* fragment and query */
    for (uint32_t k = 0; k < end; k++) {
        if (u.ptr[k] == '#') {
            p.has_frag = true;
            p.frag = (embi_str){u.ptr + k + 1, end - k - 1};
            end = k;
            break;
        }
    }
    for (uint32_t k = 0; k < end; k++) {
        if (u.ptr[k] == '?') {
            p.has_query = true;
            p.query = (embi_str){u.ptr + k + 1, end - k - 1};
            end = k;
            break;
        }
    }
    /* scheme */
    if (end && is_alpha(u.ptr[0])) {
        uint32_t k = 1;
        while (k < end && (is_alpha(u.ptr[k]) || (u.ptr[k] >= '0' && u.ptr[k] <= '9') || u.ptr[k] == '+' ||
                           u.ptr[k] == '-' || u.ptr[k] == '.')) {
            k++;
        }
        if (k < end && u.ptr[k] == ':') {
            p.scheme = (embi_str){u.ptr, k};
            i = k + 1;
        }
    }
    if (i + 1 < end && u.ptr[i] == '/' && u.ptr[i + 1] == '/') {
        p.has_authority = true;
        uint32_t a = i + 2, e = a;
        while (e < end && u.ptr[e] != '/') {
            e++;
        }
        uint32_t at = a;
        for (uint32_t k = a; k < e; k++) {
            if (u.ptr[k] == '@') {
                at = k + 1;
            }
        }
        uint32_t colon = e;
        for (uint32_t k = at; k < e; k++) {
            if (u.ptr[k] == ':') {
                colon = k;
            }
            if (u.ptr[k] == ']') {
                colon = e;
            }
        }
        p.host = (embi_str){u.ptr + at, colon - at};
        if (colon < e) {
            p.port = (embi_str){u.ptr + colon + 1, e - colon - 1};
        }
        i = e;
    }
    p.path = (embi_str){u.ptr + i, end - i};
    return p;
}

static bool special_scheme(embi_str s)
{
    return embi_str_eq(s, EMBI_STR("http")) || embi_str_eq(s, EMBI_STR("https")) ||
           embi_str_eq(s, EMBI_STR("ws")) || embi_str_eq(s, EMBI_STR("wss")) ||
           embi_str_eq(s, EMBI_STR("ftp")) || embi_str_eq(s, EMBI_STR("file"));
}

/* Removes . and .. segments from an absolute path. */
static void normalize_path(embi_str path, embi_sb *out)
{
    uint32_t base = out->len;
    uint32_t i = 0;
    while (i < path.len) {
        uint32_t s = i;
        while (i < path.len && path.ptr[i] != '/') {
            i++;
        }
        embi_str seg = {path.ptr + s, i - s};
        bool last = i >= path.len;
        if (embi_str_eq(seg, EMBI_STR(".")) || embi_str_eq(seg, EMBI_STR(".."))) {
            if (embi_str_eq(seg, EMBI_STR("..")) && out->len > base) {
                /* drop the previous segment */
                while (out->len > base && out->data[out->len - 1] != '/') {
                    out->len--;
                }
                if (out->len > base) {
                    out->len--;
                }
            }
            if (last) {
                embi_sb_append_char(out, '/');
            }
        } else if (s == 0 && seg.len == 0) {
            /* leading slash handled by the separator below */
        } else {
            if (seg.len || !last) {
                embi_sb_append_char(out, '/');
                embi_sb_append(out, seg);
            } else {
                embi_sb_append_char(out, '/');
            }
        }
        if (i < path.len) {
            i++;
        }
    }
    if (out->len == base) {
        embi_sb_append_char(out, '/');
    }
}

/* Resolves rel against base into out (an absolute URL, or rel itself when it has a scheme). */
static void url_resolve(embi_str base, embi_str rel, embi_sb *out)
{
    url_parts r = url_parse(rel);
    if (r.scheme.len) {
        embi_sb_append(out, rel);
        return;
    }
    url_parts b = url_parse(base);
    embi_sb_append(out, b.scheme);
    embi_sb_append_char(out, ':');
    if (r.has_authority) {
        embi_sb_append(out, rel);
        return;
    }
    if (b.has_authority) {
        embi_sb_append(out, EMBI_STR("//"));
        embi_sb_append(out, b.host);
        if (b.port.len) {
            embi_sb_append_char(out, ':');
            embi_sb_append(out, b.port);
        }
    }
    if (r.path.len == 0) {
        embi_sb_append(out, b.path);
        if (r.has_query) {
            embi_sb_append_char(out, '?');
            embi_sb_append(out, r.query);
        } else if (b.has_query) {
            embi_sb_append_char(out, '?');
            embi_sb_append(out, b.query);
        }
    } else {
        embi_sb joined = {0};
        if (r.path.ptr[0] == '/') {
            embi_sb_append(&joined, r.path);
        } else {
            uint32_t cut = b.path.len;
            while (cut > 0 && b.path.ptr[cut - 1] != '/') {
                cut--;
            }
            if (cut == 0 && b.has_authority) {
                embi_sb_append_char(&joined, '/');
            } else {
                embi_sb_append(&joined, (embi_str){b.path.ptr, cut});
            }
            embi_sb_append(&joined, r.path);
        }
        if (b.has_authority || joined.len) {
            normalize_path((embi_str){joined.data, joined.len}, out);
        }
        embi_sb_free(&joined);
        if (r.has_query) {
            embi_sb_append_char(out, '?');
            embi_sb_append(out, r.query);
        }
    }
    if (r.has_frag) {
        embi_sb_append_char(out, '#');
        embi_sb_append(out, r.frag);
    }
}

static bool same_origin(embi_str a, embi_str b)
{
    url_parts x = url_parse(a), y = url_parse(b);
    return embi_str_eq(x.scheme, y.scheme) && embi_str_eq(x.host, y.host) && embi_str_eq(x.port, y.port) &&
           x.has_authority && y.has_authority;
}

/* For documents without a tuple origin (about:blank, data:): only the fragment may change. */
static bool differs_in_fragment_only(embi_str a, embi_str b)
{
    uint32_t na = 0, nb = 0;
    while (na < a.len && ((const char *)a.ptr)[na] != '#') {
        na++;
    }
    while (nb < b.len && ((const char *)b.ptr)[nb] != '#') {
        nb++;
    }
    return na == nb && (na == 0 || SDL_memcmp(a.ptr, b.ptr, na) == 0);
}

/* ------------------------------------------------------------------ history entries */

static void hist_init(embi_bind_ctx *c)
{
    if (c->nhist) {
        return;
    }
    embi_str u = embi_bind_url(c);
    if (c->cap_hist == 0) {
        c->cap_hist = 8;
        c->hist = embi_calloc(c->cap_hist, sizeof *c->hist);
    }
    char *z = embi_alloc((size_t)u.len + 1);
    if (u.len) {
        SDL_memcpy(z, u.ptr, u.len);
    }
    z[u.len] = '\0';
    c->hist[0] = (embi_bind_hist){z, u.len, {nullptr, 0, 0}, false, 0};
    c->nhist = 1;
    c->hist_pos = 0;
}

static embi_str cur_url(embi_bind_ctx *c)
{
    hist_init(c);
    return (embi_str){c->hist[c->hist_pos].url, c->hist[c->hist_pos].url_len};
}

static void hist_truncate(embi_bind_ctx *c)
{
    while (c->nhist > c->hist_pos + 1) {
        c->nhist--;
        embi_free(c->hist[c->nhist].url, c->hist[c->nhist].url_len + 1u);
        embi_sb_free(&c->hist[c->nhist].bytes);
        c->hist[c->nhist].url = nullptr;
        c->hist[c->nhist].state = 0;
        c->hist[c->nhist].has_state = false;
    }
}

static void hist_set_url(embi_bind_ctx *c, uint32_t pos, embi_str u)
{
    embi_bind_hist *h = &c->hist[pos];
    embi_free(h->url, h->url_len + 1u);
    h->url = embi_alloc((size_t)u.len + 1);
    if (u.len) {
        SDL_memcpy(h->url, u.ptr, u.len);
    }
    h->url[u.len] = '\0';
    h->url_len = u.len;
}

/* The state of the entry at pos as a value: a copy made from its bytes on the first read (then cached), or null. */
static embi_js_value entry_state(embi_bind_ctx *c, uint32_t pos)
{
    embi_bind_hist *h = &c->hist[pos];
    if (!h->has_state) {
        return EMBI_JS_NULL;
    }
    if (h->state == 0) {
        embi_js_value v = EMBI_JS_UNDEFINED;
        if (!embi_clone_deserialize(embi_bind_realm(c), (embi_str){h->bytes.data, h->bytes.len}, &v)) {
            embi_js_exception_clear(c->rt);
            return EMBI_JS_NULL;
        }
        h->state = v;
    }
    return h->state;
}

/* Pushes an entry whose state is the serialization bytes (taken). */
static void hist_push(embi_bind_ctx *c, embi_str url, embi_sb bytes, bool has_state)
{
    hist_truncate(c);
    if (c->nhist == c->cap_hist) {
        uint32_t cap = c->cap_hist * 2;
        c->hist = embi_realloc(c->hist, c->cap_hist * sizeof *c->hist, cap * sizeof *c->hist);
        c->cap_hist = cap;
    }
    c->hist[c->nhist] = (embi_bind_hist){nullptr, 0, bytes, has_state, 0};
    hist_set_url(c, c->nhist, url);
    c->hist_pos = c->nhist;
    c->nhist++;
}

static void fire_popstate(embi_bind_ctx *c)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value state = entry_state(c, c->hist_pos);
    embi_js_value p = embi_bind_object_new(c);
    embi_bind_pin(c, p);
    embi_bind_object_set_c(c, p, "state", state);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_PopStateEvent, "popstate", EMBI_EVF_TRUSTED, p);
    embi_bind_pin(c, ev);
    embi_impl_fire(c, c->gbo, "popstate", ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void history_go(embi_bind_ctx *c, int64_t delta)
{
    hist_init(c);
    int64_t to = (int64_t)c->hist_pos + delta;
    if (delta == 0 || to < 0 || to >= (int64_t)c->nhist) {
        return;
    }
    c->hist_pos = (uint32_t)to;
    fire_popstate(c);
}

/* A navigation requested through location: a change of the fragment stays in the document, the
 * rest goes to the embedder. */
/* Scrolls the viewport so that the element with the id frag is at the top (the fragment's target). An empty fragment or
 * "top" scrolls to the top; an unknown id leaves the scroll alone. */
static void scroll_to_fragment(embi_bind_ctx *c, embi_str frag)
{
    double y = 0;
    if (frag.len != 0 && !embi_str_eq(frag, EMBI_STR("top"))) {
        embi_element *el = embi_get_element_by_id(c->doc, frag);
        embi_layout_engine *e = el != nullptr ? embi_bind_layout(c) : nullptr;
        embi_rectf r;
        if (e == nullptr || !embi_layout_border_box(e, el, &r)) {
            return;
        }
        y = (double)r.y;
    }
    c->scroll_y = y;
    if (c->host.scroll) {
        c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
    }
}

/* The hashchange event of a same-document navigation (at the window, after the scroll). */
static void fire_hashchange(embi_bind_ctx *c, embi_str old_url, embi_str new_url)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, p, "oldURL", embi_bind_utf8_value(c, old_url));
    embi_bind_object_set_c(c, p, "newURL", embi_bind_utf8_value(c, new_url));
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_HashChangeEvent, "hashchange", EMBI_EVF_TRUSTED, p);
    if (ev != EMBI_JS_EXCEPTION) {
        embi_bind_pin(c, ev);
        embi_impl_fire(c, c->gbo, "hashchange", ev);
    } else {
        embi_js_exception_clear(c->rt);
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void navigate_to(embi_bind_ctx *c, embi_str url, bool replace)
{
    embi_str cur = cur_url(c);
    url_parts a = url_parse(cur), b = url_parse(url);
    bool same_doc = b.has_frag && embi_str_eq(a.scheme, b.scheme) && embi_str_eq(a.host, b.host) &&
                    embi_str_eq(a.port, b.port) && embi_str_eq(a.path, b.path) && a.has_query == b.has_query &&
                    embi_str_eq(a.query, b.query);
    if (same_doc) {
        /* The old URL is copied: the history entries may move when the new one is pushed. */
        embi_sb old = {0};
        embi_sb_append(&old, cur);
        bool changed = !embi_str_eq((embi_str){old.data, old.len}, url);
        if (replace) {
            hist_set_url(c, c->hist_pos, url);
        } else {
            hist_push(c, url, (embi_sb){nullptr, 0, 0}, false);
        }
        scroll_to_fragment(c, b.frag);
        if (changed) {
            fire_hashchange(c, (embi_str){old.data, old.len}, url);
        }
        embi_sb_free(&old);
        return;
    }
    if (c->host.navigate) {
        (void)c->host.navigate(c->host.user, url, replace);
    }
}

void embi_impl_follow_form(embi_bind_ctx *c, embi_str method, embi_str raw, embi_str body, embi_str ctype)
{
    embi_sb sb = {0};
    url_resolve(cur_url(c), raw, &sb);
    if (sb.len > 0) {
        if (!(c->host.navigate_form != nullptr &&
              c->host.navigate_form(c->host.user, method, (embi_str){sb.data, sb.len}, body, ctype))) {
            navigate_to(c, (embi_str){sb.data, sb.len}, false);
        }
    }
    embi_sb_free(&sb);
}

void embi_impl_follow_link(embi_bind_ctx *c, embi_str raw)
{
    embi_sb sb = {0};
    url_resolve(cur_url(c), raw, &sb);
    if (sb.len > 0) {
        navigate_to(c, (embi_str){sb.data, sb.len}, false);
    }
    embi_sb_free(&sb);
}

/* ------------------------------------------------------------------ Location */

static embi_str loc_cur(embi_bind_ctx *c)
{
    return cur_url(c);
}

embi_dstr embi_impl_Location_get_href(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, loc_cur(c));
}

void embi_impl_Location_set_href(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    embi_sb sb = {0};
    url_resolve(loc_cur(c), embi_bind_utf8(c, a0), &sb);
    navigate_to(c, (embi_str){sb.data ? sb.data : "", sb.len}, false);
    embi_sb_free(&sb);
}

embi_dstr embi_impl_Location_get_origin(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    if (!special_scheme(p.scheme) || !p.has_authority || embi_str_eq(p.scheme, EMBI_STR("file"))) {
        return embi_impl_lit("null");
    }
    embi_sb sb = {0};
    embi_sb_append(&sb, p.scheme);
    embi_sb_append(&sb, EMBI_STR("://"));
    embi_sb_append(&sb, p.host);
    if (p.port.len) {
        embi_sb_append_char(&sb, ':');
        embi_sb_append(&sb, p.port);
    }
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

embi_dstr embi_impl_Location_get_protocol(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    embi_sb sb = {0};
    embi_sb_append(&sb, p.scheme);
    embi_sb_append_char(&sb, ':');
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

/* Rebuilds the URL with one component replaced and navigates to it. */
typedef enum { PART_PROTOCOL, PART_HOST, PART_HOSTNAME, PART_PORT, PART_PATHNAME, PART_SEARCH, PART_HASH } part_kind;

static void set_part(embi_bind_ctx *c, part_kind kind, embi_str v)
{
    url_parts p = url_parse(loc_cur(c));
    embi_sb sb = {0};
    embi_str scheme = p.scheme;
    embi_str host = p.host, port = p.port, path = p.path, query = p.query, frag = p.frag;
    bool has_query = p.has_query, has_frag = p.has_frag;
    switch (kind) {
    case PART_PROTOCOL:
        if (v.len && v.ptr[v.len - 1] == ':') {
            v.len--;
        }
        scheme = v;
        break;
    case PART_HOST: {
        uint32_t colon = v.len;
        for (uint32_t i = 0; i < v.len; i++) {
            if (v.ptr[i] == ':') {
                colon = i;
            }
        }
        host = (embi_str){v.ptr, colon};
        if (colon < v.len) {
            port = (embi_str){v.ptr + colon + 1, v.len - colon - 1};
        }
        break;
    }
    case PART_HOSTNAME:
        host = v;
        break;
    case PART_PORT:
        port = v;
        break;
    case PART_PATHNAME:
        path = v;
        break;
    case PART_SEARCH:
        if (v.len && v.ptr[0] == '?') {
            v = (embi_str){v.ptr + 1, v.len - 1};
        }
        query = v;
        has_query = v.len != 0;
        break;
    case PART_HASH:
        if (v.len && v.ptr[0] == '#') {
            v = (embi_str){v.ptr + 1, v.len - 1};
        }
        frag = v;
        has_frag = v.len != 0;
        break;
    }
    embi_sb_append(&sb, scheme);
    embi_sb_append_char(&sb, ':');
    if (p.has_authority) {
        embi_sb_append(&sb, EMBI_STR("//"));
        embi_sb_append(&sb, host);
        if (port.len) {
            embi_sb_append_char(&sb, ':');
            embi_sb_append(&sb, port);
        }
    }
    if (kind == PART_PATHNAME && path.len && path.ptr[0] != '/' && p.has_authority) {
        embi_sb_append_char(&sb, '/');
    }
    embi_sb_append(&sb, path);
    if (has_query) {
        embi_sb_append_char(&sb, '?');
        embi_sb_append(&sb, query);
    }
    if (has_frag) {
        embi_sb_append_char(&sb, '#');
        embi_sb_append(&sb, frag);
    }
    navigate_to(c, (embi_str){sb.data, sb.len}, false);
    embi_sb_free(&sb);
}

void embi_impl_Location_set_protocol(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_PROTOCOL, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_host(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    embi_sb sb = {0};
    embi_sb_append(&sb, p.host);
    if (p.port.len) {
        embi_sb_append_char(&sb, ':');
        embi_sb_append(&sb, p.port);
    }
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
    return d;
}

void embi_impl_Location_set_host(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_HOST, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_hostname(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, url_parse(loc_cur(c)).host);
}

void embi_impl_Location_set_hostname(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_HOSTNAME, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_port(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, url_parse(loc_cur(c)).port);
}

void embi_impl_Location_set_port(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_PORT, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_pathname(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    if (p.has_authority && p.path.len == 0 && special_scheme(p.scheme)) {
        return embi_impl_lit("/");
    }
    return embi_bind_tmp_utf8(c, p.path);
}

void embi_impl_Location_set_pathname(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_PATHNAME, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_search(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    if (!p.has_query || p.query.len == 0) {
        return embi_dstr_empty();
    }
    embi_sb sb = {0};
    embi_sb_append_char(&sb, '?');
    embi_sb_append(&sb, p.query);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

void embi_impl_Location_set_search(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_SEARCH, embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_Location_get_hash(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    url_parts p = url_parse(loc_cur(c));
    if (!p.has_frag || p.frag.len == 0) {
        return embi_dstr_empty();
    }
    embi_sb sb = {0};
    embi_sb_append_char(&sb, '#');
    embi_sb_append(&sb, p.frag);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

void embi_impl_Location_set_hash(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    set_part(c, PART_HASH, embi_bind_utf8(c, a0));
}

void embi_impl_Location_assign(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_impl_Location_set_href(c, self, a0);
}

void embi_impl_Location_replace(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    embi_sb sb = {0};
    url_resolve(loc_cur(c), embi_bind_utf8(c, a0), &sb);
    navigate_to(c, (embi_str){sb.data ? sb.data : "", sb.len}, true);
    embi_sb_free(&sb);
}

void embi_impl_Location_reload(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    if (c->host.navigate) {
        (void)c->host.navigate(c->host.user, loc_cur(c), true);
    }
}

/* ------------------------------------------------------------------ History */

uint32_t embi_impl_History_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    hist_init(c);
    return c->nhist;
}

int32_t embi_impl_History_get_scrollRestoration(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return (c->gbo->aux & 1u) ? EMBI_ENUM_ScrollRestoration_manual : EMBI_ENUM_ScrollRestoration_auto;
}

void embi_impl_History_set_scrollRestoration(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    if (a0 == EMBI_ENUM_ScrollRestoration_manual) {
        c->gbo->aux |= 1u;
    } else if (a0 == EMBI_ENUM_ScrollRestoration_auto) {
        c->gbo->aux &= ~1u;
    }
}

embi_js_value embi_impl_History_get_state(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    hist_init(c);
    return entry_state(c, c->hist_pos);
}

void embi_impl_History_go(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    history_go(c, a0);
}

void embi_impl_History_back(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    history_go(c, -1);
}

void embi_impl_History_forward(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    history_go(c, 1);
}

static embi_dom_err state_change(embi_bind_ctx *c, embi_js_value data, embi_bind_nstr url, bool replace)
{
    hist_init(c);
    /* The state is serialized first (a DataCloneError leaves the history as it was), then the URL is checked. */
    embi_sb bytes = {0};
    if (!embi_clone_serialize(embi_bind_realm(c), data, nullptr, 0, &bytes)) {
        embi_sb_free(&bytes);
        return EMBI_BIND_THREW;
    }
    embi_sb sb = {0};
    embi_str cur = cur_url(c);
    if (!url.is_null) {
        url_resolve(cur, embi_bind_utf8(c, url.s), &sb);
        embi_str target = {sb.data ? sb.data : "", sb.len};
        url_parts t = url_parse(target);
        if (!same_origin(cur, target) && !differs_in_fragment_only(cur, target) && !(t.scheme.len == 0)) {
            embi_sb_free(&sb);
            embi_sb_free(&bytes);
            return embi_impl_throw(c, "SecurityError",
                                   "A history state object with the given URL cannot be created in a document with this origin.");
        }
    } else {
        embi_sb_append(&sb, cur);
    }
    embi_str target = {sb.data ? sb.data : "", sb.len};
    if (replace) {
        hist_set_url(c, c->hist_pos, target);
        embi_bind_hist *h = &c->hist[c->hist_pos];
        embi_sb_free(&h->bytes);
        h->bytes = bytes;
        h->has_state = true;
        h->state = 0;
    } else {
        hist_push(c, target, bytes, true);
    }
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_History_pushState(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_dstr a1,
                                         embi_bind_nstr a2)
{
    (void)self;
    (void)a1;
    return state_change(c, a0, a2, false);
}

embi_dom_err embi_impl_History_replaceState(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                            embi_dstr a1, embi_bind_nstr a2)
{
    (void)self;
    (void)a1;
    return state_change(c, a0, a2, true);
}

/* ------------------------------------------------------------------ Screen and Navigator */

static void screen_size(embi_bind_ctx *c, int32_t *w, int32_t *h)
{
    double vw, vh, dpr;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    *w = (int32_t)vw;
    *h = (int32_t)vh;
}

#define SCREEN_GET(NAME, EXPR)                                                                         \
    int32_t embi_impl_Screen_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)                         \
    {                                                                                                  \
        (void)self;                                                                                    \
        int32_t w, h;                                                                                  \
        screen_size(c, &w, &h);                                                                        \
        return (EXPR);                                                                                 \
    }

SCREEN_GET(availWidth, w)
SCREEN_GET(availHeight, h)
SCREEN_GET(width, w)
SCREEN_GET(height, h)

uint32_t embi_impl_Screen_get_colorDepth(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 24;
}

uint32_t embi_impl_Screen_get_pixelDepth(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 24;
}

embi_dstr embi_impl_Navigator_get_userAgent(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("Ember/" EMB_VERSION_STRING);
}

embi_dstr embi_impl_Navigator_get_appCodeName(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("Mozilla");
}

embi_dstr embi_impl_Navigator_get_appName(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("Netscape");
}

embi_dstr embi_impl_Navigator_get_appVersion(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit(EMB_VERSION_STRING);
}

embi_dstr embi_impl_Navigator_get_platform(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("");
}

embi_dstr embi_impl_Navigator_get_product(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("Gecko");
}

embi_dstr embi_impl_Navigator_get_vendor(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("");
}

embi_dstr embi_impl_Navigator_get_language(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, embi_bind_language(c));
}

bool embi_impl_Navigator_get_onLine(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return true;
}

bool embi_impl_Navigator_get_cookieEnabled(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return false;
}

uint64_t embi_impl_Navigator_get_hardwareConcurrency(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 1;
}

bool embi_impl_Navigator_get_webdriver(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return false;
}

/* ------------------------------------------------------------------ MediaQueryList */

typedef struct mql {
    uint32_t len;
    bool matches;
    bool watched;
    char text[];
} mql;

static bool mql_eval(embi_bind_ctx *c, const mql *m)
{
    embi_style_env env = {0};
    double vw, vh, dpr;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    env.width = vw;
    env.height = vh;
    env.dppx = dpr;
    env.scripting = true;
    env.hover = true;
    env.pointer = 2;
    env.update = 2;
    embi_css_slice q = embi_css_parse_cvs(&c->arena, (embi_str){m->text, m->len});
    return q.n == 0 || embi_style_media_match(q, &env);
}

embi_js_value embi_impl_Window_matchMedia(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    embi_str q = embi_bind_utf8(c, a0);
    size_t total = sizeof(mql) + q.len + 1;
    mql *m = embi_calloc(1, total);
    m->len = q.len;
    if (q.len) {
        SDL_memcpy(m->text, q.ptr, q.len);
    }
    m->matches = mql_eval(c, m);
    return embi_bind_other_value(c, EMBI_IF_MediaQueryList, m, m, (uint32_t)total);
}

embi_dstr embi_impl_MediaQueryList_get_media(embi_bind_ctx *c, embi_bind_obj *self)
{
    const mql *m = self->ptr;
    return embi_bind_tmp_utf8(c, (embi_str){m->text, m->len});
}

bool embi_impl_MediaQueryList_get_matches(embi_bind_ctx *c, embi_bind_obj *self)
{
    mql *m = self->ptr;
    m->matches = mql_eval(c, m);
    return m->matches;
}

void embi_impl_MediaQueryList_addListener(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_bind_union opts = {0};
    embi_impl_EventTarget_addEventListener(c, self, embi_impl_lit("change"), a0, opts);
}

void embi_impl_MediaQueryList_removeListener(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    embi_bind_union opts = {0};
    embi_impl_EventTarget_removeEventListener(c, self, embi_impl_lit("change"), a0, opts);
}

/* Called by the EventTarget store when a change listener is added to a MediaQueryList. */
void embi_impl_mql_watch(embi_bind_ctx *c, embi_bind_obj *bo)
{
    mql *m = bo->ptr;
    if (m->watched) {
        return;
    }
    m->watched = true;
    if (embi_js_is_empty(c->mqls)) {
        c->mqls = embi_bind_array_new(c, 0);
    }
    embi_bind_array_set(c, c->mqls, embi_bind_array_len(c, c->mqls), embi_bind_obj_value(bo));
}

void embi_bind_viewport_changed(embi_bind_ctx *c)
{
    if (embi_js_is_empty(c->mqls)) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    uint32_t n = embi_bind_array_len(c, c->mqls);
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value v = embi_bind_array_get(c, c->mqls, i);
        embi_bind_obj *bo = embi_bind_obj_of(v);
        if (!bo || embi_bind_stale(bo)) {
            continue;
        }
        mql *m = bo->ptr;
        bool now = mql_eval(c, m);
        if (now == m->matches) {
            continue;
        }
        m->matches = now;
        embi_js_value p = embi_bind_object_new(c);
        embi_bind_pin(c, p);
        embi_bind_object_set_c(c, p, "media", embi_bind_utf8_value(c, (embi_str){m->text, m->len}));
        embi_bind_object_set_c(c, p, "matches", embi_js_from_bool(now));
        embi_js_value ev = embi_impl_event_make(c, EMBI_IF_MediaQueryListEvent, "change", EMBI_EVF_TRUSTED, p);
        embi_bind_pin(c, ev);
        embi_impl_fire(c, bo, "change", ev);
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ Window */

embi_js_value embi_impl_Window_get_window(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_obj_value(c->gbo);
}

embi_js_value embi_impl_Window_get_self(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_window(c, self);
}

embi_node *embi_impl_Window_get_document(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return &c->doc->base;
}

embi_js_value embi_impl_Window_get_location(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_other_value(c, EMBI_IF_Location, c, nullptr, 0);
}

embi_js_value embi_impl_Window_get_event(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return c->has_win_event ? c->win_event : EMBI_JS_UNDEFINED;
}

embi_js_value embi_impl_Window_get_history(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_other_value(c, EMBI_IF_History, c, nullptr, 0);
}

embi_js_value embi_impl_Window_get_screen(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_other_value(c, EMBI_IF_Screen, c, nullptr, 0);
}

embi_js_value embi_impl_Window_get_navigator(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_other_value(c, EMBI_IF_Navigator, c, nullptr, 0);
}

embi_js_value embi_impl_Window_get_frames(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_window(c, self);
}

embi_js_value embi_impl_Window_get_top(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_window(c, self);
}

embi_js_value embi_impl_Window_get_parent(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_window(c, self);
}

int32_t embi_impl_Window_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 0;
}

bool embi_impl_Window_get_closed(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return false;
}

embi_dstr embi_impl_Window_get_name(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    embi_dstr d = embi_dstr_empty();
    if (!embi_js_is_empty(c->win_name)) {
        (void)embi_bind_to_dstr(c, c->win_name, &d);
    }
    return d;
}

void embi_impl_Window_set_name(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    c->win_name = embi_bind_dstr_value(c, a0);
}

#define WIN_VIEW_GET(NAME, TYPE, EXPR)                                                                 \
    TYPE embi_impl_Window_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)                            \
    {                                                                                                  \
        (void)self;                                                                                    \
        double w, h, dpr;                                                                              \
        embi_bind_viewport(c, &w, &h, &dpr);                                                           \
        return (EXPR);                                                                                 \
    }

WIN_VIEW_GET(innerWidth, int32_t, (int32_t)w)
WIN_VIEW_GET(innerHeight, int32_t, (int32_t)h)
WIN_VIEW_GET(outerWidth, int32_t, (int32_t)w)
WIN_VIEW_GET(outerHeight, int32_t, (int32_t)h)
WIN_VIEW_GET(devicePixelRatio, double, dpr)
WIN_VIEW_GET(screenX, int32_t, 0)
WIN_VIEW_GET(screenY, int32_t, 0)

double embi_impl_Window_get_scrollX(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return c->scroll_x;
}

double embi_impl_Window_get_scrollY(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return c->scroll_y;
}

double embi_impl_Window_get_pageXOffset(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_scrollX(c, self);
}

double embi_impl_Window_get_pageYOffset(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_Window_get_scrollY(c, self);
}

static void scroll_apply(embi_bind_ctx *c, double x, double y, bool relative)
{
    /* NaN means "keep" */
    if (x == x) {
        c->scroll_x = relative ? c->scroll_x + x : x;
    }
    if (y == y) {
        c->scroll_y = relative ? c->scroll_y + y : y;
    }
    if (c->scroll_x < 0) {
        c->scroll_x = 0;
    }
    if (c->scroll_y < 0) {
        c->scroll_y = 0;
    }
    if (c->host.scroll) {
        c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
    }
}

void embi_impl_Window_scroll_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_ScrollToOptions a0)
{
    (void)self;
    scroll_apply(c, a0.left, a0.top, false);
}

void embi_impl_Window_scroll_1(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)self;
    scroll_apply(c, a0, a1, false);
}

void embi_impl_Window_scrollTo_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_ScrollToOptions a0)
{
    (void)self;
    scroll_apply(c, a0.left, a0.top, false);
}

void embi_impl_Window_scrollTo_1(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)self;
    scroll_apply(c, a0, a1, false);
}

void embi_impl_Window_scrollBy_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_ScrollToOptions a0)
{
    (void)self;
    scroll_apply(c, a0.left, a0.top, true);
}

void embi_impl_Window_scrollBy_1(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)self;
    scroll_apply(c, a0, a1, true);
}

embi_js_value embi_impl_Window_getComputedStyle(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0,
                                                embi_bind_nstr a1)
{
    (void)self;
    (void)a1;
    return embi_impl_style_computed(c, embi_impl_node_rec(c, a0));
}

void embi_impl_Window_focus(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
}

void embi_impl_Window_blur(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
}

void embi_impl_Window_close(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
}

void embi_impl_Window_stop(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
}

/* ------------------------------------------------------------------ named properties of the Window object */

/* The named properties object of the Window (WebIDL [Global]): an object between Window.prototype and
 * EventTarget.prototype. The element whose id is the name of a property that nothing else defines is found here,
 * so a var or an assignment of the page creates an own property that shadows it. */
static int named_props_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    if (!embi_js_is_string(k)) {
        return EMBI_JS_NO;
    }
    embi_bind_obj *bo = (embi_bind_obj *)o->internal;
    embi_bind_ctx *c = bo ? bo->ctx : nullptr;
    if (c == nullptr || c->doc == nullptr || embi_bind_proto_has(rt, o, k)) {
        return EMBI_JS_NO;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str name = embi_bind_key_utf8(c, k);
    embi_element *el = name.len ? embi_get_element_by_id(c->doc, name) : nullptr;
    embi_js_value v = el ? embi_bind_node_value(c, &el->base) : EMBI_JS_UNDEFINED;
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    if (embi_js_is_undefined(v)) {
        return EMBI_JS_NO;
    }
    out->value = v;
    out->get = EMBI_JS_UNDEFINED;
    out->set = EMBI_JS_UNDEFINED;
    out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
    out->attrs = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE;
    return EMBI_JS_YES;
}

static const embi_js_class k_window_properties = {
    .name = "WindowProperties",
    .get_own = named_props_get_own,
};

void embi_impl_window_install_named(embi_bind_ctx *c)
{
    ldr_install_lazy_globals(c); /* W7f: WritableStream, TransformStream ... (impl_loader_prelude.c) */
    embi_impl_anim_install(c);   /* W6c: requestAnimationFrame, Element.animate, Animation ... (impl_anim.c) */
    embi_impl_hit_install(c);    /* W8b: document.elementFromPoint, elementsFromPoint (impl_hit.c) */
    embi_impl_css_ns_install(c); /* W6e: CSS.supports, CSS.escape (impl_css_ns.c) */
    if (!embi_js_is_object(c->protos[EMBI_IF_Window])) {
        return;
    }
    embi_js_object *wp = embi_js_as_object(c->protos[EMBI_IF_Window]);
    /* The generator put an empty object between Window.prototype and EventTarget.prototype (the named properties
     * object of a [Global] interface); it becomes the WindowProperties object. */
    embi_js_value mid = embi_js_obj_proto(wp);
    if (!embi_js_is_object(mid)) {
        return;
    }
    embi_js_object *np = embi_js_as_object(mid);
    np->klass = &k_window_properties;
    np->internal = c->gbo;
}
