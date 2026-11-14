/*
 * WebSocket and CloseEvent (websocket.webidl, N1c, HTML Standard 9.3), and the plumbing WebSocket and EventSource share
 * (impl_net.h): the record behind a wrapper, its link to the network side, and the per-context registry.
 *
 * The protocol is src/net/websocket.c, reached through the hooks of net_host.h (the embedding layer implements them
 * with the network service; a context without them gets an object that fails like a network error). readyState and
 * bufferedAmount are tracked here: the events that change them arrive on the context's thread, in order.
 */
#include "bindings/impl_net.h"

#include "bindings/gen/events.h"
#include "bindings/gen/messaging.h"
#include "bindings/gen/websocket.h"

#include <math.h>

enum { WS_CONNECTING, WS_OPEN, WS_CLOSING, WS_CLOSED };

/* ------------------------------------------------------------------ shared plumbing */

nlink *embi_impl_net_link_new(nrec *r)
{
    nlink *l = SDL_malloc(sizeof *l);
    SDL_SetAtomicInt(&l->refs, 2);
    l->rec = r;
    return l;
}

void embi_impl_net_link_unref(nlink *l)
{
    if (SDL_AddAtomicInt(&l->refs, -1) == 1) {
        SDL_free(l);
    }
}

void embi_impl_net_start(nrec *r)
{
    embi_bind_ctx *c = r->c;
    r->pin = embi_js_persistent_new(c->rt, embi_bind_obj_value(r->bo)) + 1u;
    nrec **head = (nrec **)ldr_net_registry(c);
    r->next = *head;
    r->prev = nullptr;
    if (*head != nullptr) {
        (*head)->prev = r;
    }
    *head = r;
    r->slot = head;
    r->registered = true;
}

void embi_impl_net_unpin(nrec *r)
{
    if (r->pin != 0 && !r->dead) {
        embi_js_persistent_free(r->c->rt, r->pin - 1u);
    }
    r->pin = 0;
}

static void rec_unlink(nrec *r)
{
    if (!r->registered) {
        return;
    }
    r->registered = false;
    if (r->prev != nullptr) {
        r->prev->next = r->next;
    } else {
        *r->slot = r->next;
    }
    if (r->next != nullptr) {
        r->next->prev = r->prev;
    }
}

/* The document goes away: nothing of the network side may reach the context any more. */
void embi_impl_net_release(embi_bind_ctx *c, void *registry)
{
    for (nrec *r = registry; r != nullptr;) {
        nrec *next = r->next;
        r->dead = true;
        r->registered = false;
        r->link->rec = nullptr;
        if (r->pin != 0) {
            embi_js_persistent_free(c->rt, r->pin - 1u);
            r->pin = 0;
        }
        if (r->h != nullptr && r->drop != nullptr) {
            r->drop(r->h);
        }
        r->h = nullptr;
        r = next;
    }
}

void embi_impl_net_rec_release(void *owned)
{
    nrec *r = owned;
    if (!r->dead) {
        rec_unlink(r);
        r->link->rec = nullptr;
        if (r->h != nullptr && r->drop != nullptr) {
            r->drop(r->h);
        }
        r->h = nullptr;
    }
    embi_impl_net_link_unref(r->link);
    SDL_free(r->url);
    SDL_free(r->protocol);
    SDL_free(r->origin);
}

char *embi_impl_net_page_origin(embi_bind_ctx *c)
{
    embi_sb o = {0};
    embi_url u = {0};
    if (embi_url_parse(embi_bind_url(c), nullptr, &u)) {
        embi_url_origin(&u, &o);
        embi_url_free(&u);
    } else {
        embi_sb_append(&o, EMBI_STR("null"));
    }
    char *s = SDL_strndup(embi_sb_cstr(&o), o.len);
    embi_sb_free(&o);
    return s;
}

bool embi_impl_net_post(embi_bind_ctx *c, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    const embi_loader_env *env = ldr_env(c);
    return env->post != nullptr && env->post(env->user, fn, drop, arg);
}

void embi_impl_net_fire(nrec *r, const char *type)
{
    embi_bind_ctx *c = r->c;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, type, EMBI_EVF_TRUSTED, EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        embi_impl_fire(c, r->bo, type, ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_impl_net_fire_message(nrec *r, const char *type, embi_js_value data, const char *origin, const char *last_event_id)
{
    embi_bind_ctx *c = r->c;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, p, "data", data);
    embi_bind_object_set_c(c, p, "origin", embi_bind_utf8_value(c, (embi_str){origin, (uint32_t)SDL_strlen(origin)}));
    embi_bind_object_set_c(c, p, "lastEventId", embi_bind_utf8_value(c, (embi_str){last_event_id, (uint32_t)SDL_strlen(last_event_id)}));
    embi_bind_object_set_c(c, p, "source", EMBI_JS_NULL);
    embi_js_value ports = embi_bind_array_new(c, 0);
    (void)embi_js_set_integrity(c->rt, embi_js_as_object(ports), true);
    embi_bind_object_set_c(c, p, "ports", ports);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_MessageEvent, type, EMBI_EVF_TRUSTED, p);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        embi_impl_fire(c, r->bo, type, ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_impl_net_warn(nrec *r, const char *what, const char *message)
{
    char buf[512];
    int n = SDL_snprintf(buf, sizeof buf, "%s to '%s' failed: %s", what, r->url != nullptr ? r->url : "", message);
    embi_bind_log(r->c, EMBI_BIND_LOG_WARN, (embi_str){buf, (uint32_t)(n > 0 ? (n < (int)sizeof buf ? n : (int)sizeof buf - 1) : 0)});
}

/* ------------------------------------------------------------------ CloseEvent */

static void pset(embi_bind_ctx *c, embi_js_value obj, const char *k, embi_js_value v)
{
    if (embi_js_is_object(obj)) {
        embi_bind_object_set_c(c, obj, k, v);
    }
}

static embi_js_value pget(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (!embi_js_is_object(obj)) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return EMBI_JS_UNDEFINED;
    }
    return v;
}

embi_js_value embi_impl_CloseEvent_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0, embi_dict_CloseEventInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    pset(c, p, "wasClean", embi_js_from_bool(a1.wasClean));
    pset(c, p, "code", embi_js_from_double((double)a1.code));
    pset(c, p, "reason", embi_bind_dstr_value(c, a1.reason));
    uint32_t flags = (a1.base.bubbles ? EMBI_EVF_BUBBLES : 0u) | (a1.base.cancelable ? EMBI_EVF_CANCELABLE : 0u) |
                     (a1.base.composed ? EMBI_EVF_COMPOSED : 0u);
    embi_str type = embi_bind_utf8(c, a0);
    char *t = embi_bind_alloc(c, (size_t)type.len + 1u);
    memcpy(t, type.ptr, type.len);
    t[type.len] = 0;
    embi_js_value v = embi_impl_event_make(c, EMBI_IF_CloseEvent, t, flags, p);
    if (v != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, v, new_target);
    }
    return embi_bind_leave(sc, v);
}

bool embi_impl_CloseEvent_get_wasClean(embi_bind_ctx *c, embi_bind_obj *self)
{
    return pget(c, self->ref, "wasClean") == EMBI_JS_TRUE;
}

uint16_t embi_impl_CloseEvent_get_code(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "code");
    double d = 0;
    if (embi_js_is_undefined(v) || !embi_js_to_number(c->rt, v, &d)) {
        embi_js_exception_clear(c->rt);
        return 0;
    }
    return (uint16_t)d;
}

embi_dstr embi_impl_CloseEvent_get_reason(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_js_value v = pget(c, self->ref, "reason");
    embi_dstr d;
    if (embi_js_is_undefined(v) || !embi_bind_to_dstr(c, v, &d)) {
        embi_js_exception_clear(c->rt);
        return embi_bind_tmp_utf8(c, EMBI_STR(""));
    }
    return d;
}

/* ------------------------------------------------------------------ events from the network (the context's thread) */

static nrec *rec_of(void *user)
{
    nlink *l = user;
    nrec *r = l->rec;
    return r != nullptr && !r->dead ? r : nullptr;
}

static const embi_loader_env *env_of(nrec *r)
{
    return ldr_env(r->c);
}

/* The connection is over: the wrapper may be collected, the handle goes. */
static void ws_finish(nrec *r)
{
    embi_bind_ctx *c = r->c;
    embi_impl_net_unpin(r);
    if (r->h != nullptr) {
        const embi_loader_env *env = env_of(r);
        if (env->net.ws_sent != nullptr) {
            r->sent_final = env->net.ws_sent(r->h);
        }
        void *h = r->h;
        r->h = nullptr;
        if (env->net.ws_free != nullptr) {
            env->net.ws_free(h);
        }
    }
    (void)c;
}

static void ws_on_open(void *user, const char *protocol)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready != WS_CONNECTING) {
        return;
    }
    r->ready = WS_OPEN;
    SDL_free(r->protocol);
    r->protocol = SDL_strdup(protocol);
    embi_impl_net_fire(r, "open");
}

static void ws_on_message(void *user, bool binary, const uint8_t *d, size_t n)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready != WS_OPEN) {
        return;
    }
    embi_bind_ctx *c = r->c;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value data;
    if (binary) {
        data = r->arraybuffer ? ldr_new_arraybuffer(c, d, n) : ldr_blob_new(c, d, n, EMBI_STR(""));
    } else {
        data = embi_bind_utf8_value(c, (embi_str){(const char *)d, (uint32_t)n});
    }
    if (data == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        data = embi_bind_pin(c, data);
        embi_impl_net_fire_message(r, "message", data, r->origin, "");
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void ws_on_error(void *user, const char *message)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready == WS_CLOSED) {
        return;
    }
    embi_impl_net_warn(r, "WebSocket connection", message);
    embi_impl_net_fire(r, "error");
}

static void ws_on_close(void *user, bool was_clean, uint16_t code, const char *reason)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready == WS_CLOSED) {
        return;
    }
    r->ready = WS_CLOSED;
    embi_bind_ctx *c = r->c;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value p = embi_bind_pin(c, embi_bind_object_new(c));
    pset(c, p, "wasClean", embi_js_from_bool(was_clean));
    pset(c, p, "code", embi_js_from_double((double)code));
    pset(c, p, "reason", embi_bind_utf8_value(c, (embi_str){reason, (uint32_t)SDL_strlen(reason)}));
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_CloseEvent, "close", EMBI_EVF_TRUSTED, p);
    /* Keep the wrapper alive through the dispatch; release it after the handlers ran. */
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    } else {
        embi_impl_fire(c, r->bo, "close", ev);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    if (!r->dead) {
        ws_finish(r);
    }
}

static void ws_release(void *user)
{
    embi_impl_net_link_unref(user);
}

static const embi_netapi_ws_cb k_ws_cb = {ws_on_open, ws_on_message, ws_on_error, ws_on_close, ws_release};

/* The network is not there: the failure the standard describes for a connection that cannot be made, later. */
typedef struct ftask {
    nlink *l;
} ftask;

static void fail_run(void *arg)
{
    ftask *t = arg;
    nrec *r = rec_of(t->l);
    if (r != nullptr && (r->ready == WS_CONNECTING || r->ready == WS_CLOSING)) {
        ws_on_error(t->l, "the network is not available");
        ws_on_close(t->l, false, 1006, "");
    }
    embi_impl_net_link_unref(t->l);
    SDL_free(t);
}

static void fail_drop(void *arg)
{
    ftask *t = arg;
    embi_impl_net_link_unref(t->l);
    SDL_free(t);
}

/* ------------------------------------------------------------------ WebSocket */

static bool token_char(unsigned char ch)
{
    return ch > 0x20 && ch < 0x7F && SDL_strchr("()<>@,;:\\\"/[]?={}", ch) == nullptr;
}

static embi_js_value syntax_error(embi_bind_ctx *c, embi_bind_scope sc, const char *msg)
{
    (void)embi_bind_throw_named(c, "SyntaxError", msg);
    return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
}

embi_js_value embi_impl_WebSocket_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str input = embi_bind_utf8(c, a0);
    embi_url base = {0}, u = {0};
    bool has_base = embi_url_parse(embi_bind_url(c), nullptr, &base);
    char msg[300];
    if (!embi_url_parse(input, has_base ? &base : nullptr, &u)) {
        embi_url_free(&base);
        SDL_snprintf(msg, sizeof msg, "Failed to construct 'WebSocket': The URL '%.*s' is invalid.", (int)input.len, input.ptr);
        return syntax_error(c, sc, msg);
    }
    embi_url_free(&base);
    /* http and https stand for ws and wss; anything but ws and wss is refused, and so is a fragment. */
    embi_sb wsurl = {0};
    if (u.scheme_len == 4 && memcmp(u.href, "http", 4) == 0) {
        embi_sb_append(&wsurl, EMBI_STR("ws"));
        embi_sb_append(&wsurl, (embi_str){u.href + 4, u.href_len - 4});
    } else if (u.scheme_len == 5 && memcmp(u.href, "https", 5) == 0) {
        embi_sb_append(&wsurl, EMBI_STR("wss"));
        embi_sb_append(&wsurl, (embi_str){u.href + 5, u.href_len - 5});
    } else if ((u.scheme_len == 2 && memcmp(u.href, "ws", 2) == 0) || (u.scheme_len == 3 && memcmp(u.href, "wss", 3) == 0)) {
        embi_sb_append(&wsurl, (embi_str){u.href, u.href_len});
    } else {
        SDL_snprintf(msg, sizeof msg, "Failed to construct 'WebSocket': The URL's scheme must be either 'http', 'https', 'ws', or 'wss'. '%.*s' is not allowed.",
                     (int)u.scheme_len, u.href); /* before the URL is freed */
        embi_url_free(&u);
        embi_sb_free(&wsurl);
        return syntax_error(c, sc, msg);
    }
    if (u.has_fragment) {
        embi_url_free(&u);
        embi_sb_free(&wsurl);
        return syntax_error(c, sc, "Failed to construct 'WebSocket': The URL contains a fragment identifier.");
    }
    embi_url_free(&u);
    /* The subprotocols: a string or a sequence of strings, each a token, with no duplicates. */
    const char **protos = nullptr;
    uint32_t np = 0;
    if (!embi_js_is_undefined(a1)) {
        embi_js_value *items = nullptr;
        embi_js_value one = a1;
        uint32_t n = 1;
        if (embi_js_is_object(a1)) {
            if (!embi_bind_seq_open(c, a1, "Failed to construct 'WebSocket'", &items, &n)) {
                embi_sb_free(&wsurl);
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
        } else {
            items = &one;
        }
        protos = (const char **)embi_bind_alloc(c, sizeof *protos * (n + 1u));
        for (uint32_t i = 0; i < n; i++) {
            embi_dstr d;
            if (!embi_bind_to_dstr(c, items[i], &d)) {
                embi_sb_free(&wsurl);
                return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
            }
            embi_str s = embi_bind_utf8(c, d);
            bool ok = s.len > 0;
            for (uint32_t k = 0; k < s.len && ok; k++) {
                ok = token_char((unsigned char)s.ptr[k]);
            }
            char *copy = embi_bind_alloc(c, (size_t)s.len + 1u);
            memcpy(copy, s.ptr, s.len);
            copy[s.len] = 0;
            for (uint32_t k = 0; k < np && ok; k++) {
                ok = strcmp(protos[k], copy) != 0;
            }
            if (!ok) {
                embi_sb_free(&wsurl);
                SDL_snprintf(msg, sizeof msg, "Failed to construct 'WebSocket': The subprotocol '%s' is invalid or duplicated.", copy);
                return syntax_error(c, sc, msg);
            }
            protos[np++] = copy;
        }
    }
    nrec *r = embi_alloc(sizeof *r);
    *r = (nrec){0};
    r->kind = 1;
    r->c = c;
    r->ready = WS_CONNECTING;
    r->url = SDL_strndup(embi_sb_cstr(&wsurl), wsurl.len);
    r->protocol = SDL_strdup("");
    r->arraybuffer = false;
    {
        embi_url wu = {0};
        embi_sb o = {0};
        if (embi_url_parse((embi_str){r->url, wsurl.len}, nullptr, &wu)) {
            embi_url_origin(&wu, &o);
            embi_url_free(&wu);
        }
        r->origin = SDL_strndup(o.len > 0 ? embi_sb_cstr(&o) : "null", o.len > 0 ? o.len : 4);
        embi_sb_free(&o);
    }
    embi_sb_free(&wsurl);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_WebSocket, r, r, sizeof *r);
    r->bo = embi_bind_obj_of(v);
    r->bo->release = embi_impl_net_rec_release;
    embi_bind_adopt_new_target(c, v, new_target);
    r->link = embi_impl_net_link_new(r);
    embi_impl_net_start(r);

    const embi_loader_env *env = ldr_env(c);
    char *origin = embi_impl_net_page_origin(c);
    if (env->net.ws_open != nullptr) {
        r->h = env->net.ws_open(env->net.user, r->url, protos, np, origin, &k_ws_cb, r->link);
        r->drop = env->net.ws_free;
    }
    SDL_free(origin);
    if (r->h == nullptr) { /* nothing consumed the network side reference: the failure comes as events, later */
        embi_impl_net_link_unref(r->link);
        ftask *t = SDL_malloc(sizeof *t);
        t->l = r->link;
        SDL_AddAtomicInt(&r->link->refs, 1);
        if (!embi_impl_net_post(c, fail_run, fail_drop, t)) {
            fail_drop(t);
        }
    }
    return embi_bind_leave(sc, v);
}

static nrec *self_rec(embi_bind_obj *self)
{
    nrec *r = self->ptr;
    return r != nullptr && r->kind == 1 ? r : nullptr;
}

embi_dstr embi_impl_WebSocket_get_url(embi_bind_ctx *c, embi_bind_obj *self)
{
    nrec *r = self_rec(self);
    return embi_bind_tmp_utf8(c, (embi_str){r != nullptr ? r->url : "", r != nullptr ? (uint32_t)strlen(r->url) : 0});
}

uint16_t embi_impl_WebSocket_get_readyState(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    nrec *r = self_rec(self);
    return r != nullptr ? r->ready : WS_CLOSED;
}

uint64_t embi_impl_WebSocket_get_bufferedAmount(embi_bind_ctx *c, embi_bind_obj *self)
{
    nrec *r = self_rec(self);
    if (r == nullptr) {
        return 0;
    }
    uint64_t sent = r->sent_final;
    if (r->h != nullptr) {
        const embi_loader_env *env = ldr_env(c);
        if (env->net.ws_sent != nullptr) {
            sent = env->net.ws_sent(r->h);
        }
    }
    return r->queued > sent ? r->queued - sent : 0;
}

embi_dstr embi_impl_WebSocket_get_extensions(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, EMBI_STR(""));
}

embi_dstr embi_impl_WebSocket_get_protocol(embi_bind_ctx *c, embi_bind_obj *self)
{
    nrec *r = self_rec(self);
    return embi_bind_tmp_utf8(c, (embi_str){r != nullptr ? r->protocol : "", r != nullptr ? (uint32_t)strlen(r->protocol) : 0});
}

embi_dstr embi_impl_WebSocket_get_binaryType(embi_bind_ctx *c, embi_bind_obj *self)
{
    nrec *r = self_rec(self);
    return embi_bind_tmp_utf8(c, r != nullptr && r->arraybuffer ? EMBI_STR("arraybuffer") : EMBI_STR("blob"));
}

/* Only "blob" and "arraybuffer" are accepted; anything else leaves the attribute alone. */
void embi_impl_WebSocket_set_binaryType(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    nrec *r = self_rec(self);
    if (r == nullptr) {
        return;
    }
    embi_str s = embi_bind_utf8(c, a0);
    if (embi_str_eq(s, EMBI_STR("blob"))) {
        r->arraybuffer = false;
    } else if (embi_str_eq(s, EMBI_STR("arraybuffer"))) {
        r->arraybuffer = true;
    }
}

embi_dom_err embi_impl_WebSocket_close(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1)
{
    nrec *r = self_rec(self);
    if (r == nullptr) {
        return EMBI_DOM_OK;
    }
    uint16_t code = 0;
    if (!embi_js_is_undefined(a0)) {
        double d;
        if (!embi_js_to_number(c->rt, a0, &d)) {
            return EMBI_BIND_THREW;
        }
        d = isnan(d) ? 0 : (d < 0 ? 0 : (d > 65535 ? 65535 : rint(d))); /* [Clamp] unsigned short */
        code = (uint16_t)d;
        if (code != 1000 && (code < 3000 || code > 4999)) {
            char msg[200];
            SDL_snprintf(msg, sizeof msg,
                         "Failed to execute 'close' on 'WebSocket': The close code must be either 1000, or between 3000 and 4999. %u is neither.",
                         (unsigned)code);
            return embi_impl_throw(c, "InvalidAccessError", msg);
        }
    }
    embi_sb reason = {0};
    if (!embi_js_is_undefined(a1) && !ldr_to_utf8(c, a1, &reason)) {
        embi_sb_free(&reason);
        return EMBI_BIND_THREW;
    }
    if (reason.len > 123) {
        embi_sb_free(&reason);
        return embi_impl_throw(c, "SyntaxError", "Failed to execute 'close' on 'WebSocket': The message must not be greater than 123 bytes.");
    }
    if (r->ready == WS_CLOSING || r->ready == WS_CLOSED) {
        embi_sb_free(&reason);
        return EMBI_DOM_OK;
    }
    r->ready = WS_CLOSING;
    const embi_loader_env *env = ldr_env(c);
    if (r->h != nullptr && env->net.ws_close != nullptr) {
        env->net.ws_close(r->h, code, reason.data, code != 0 ? reason.len : 0);
    }
    embi_sb_free(&reason);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_WebSocket_send(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    nrec *r = self_rec(self);
    if (r == nullptr) {
        return EMBI_DOM_OK;
    }
    if (r->ready == WS_CONNECTING) {
        return embi_impl_throw(c, "InvalidStateError", "Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.");
    }
    const uint8_t *p = nullptr;
    size_t n = 0;
    bool binary = true, detached = false;
    embi_str type;
    embi_sb text = {0};
    if (ldr_blob_view(a0, &p, &n, &type)) {
        /* a Blob or File */
    } else if (ldr_buffer_source(a0, &p, &n, &detached)) {
        if (detached) {
            n = 0;
        }
    } else {
        binary = false;
        if (!ldr_to_utf8(c, a0, &text)) {
            embi_sb_free(&text);
            return EMBI_BIND_THREW;
        }
        p = (const uint8_t *)text.data;
        n = text.len;
    }
    r->queued += n;
    if (r->ready == WS_OPEN && r->h != nullptr) {
        const embi_loader_env *env = ldr_env(c);
        if (env->net.ws_send != nullptr) {
            (void)env->net.ws_send(r->h, binary, p, n);
        }
    }
    embi_sb_free(&text);
    return EMBI_DOM_OK;
}
