/*
 * EventSource (eventsource.webidl, N1c, HTML Standard 9.2). The connection, the reconnection and the event stream
 * parser are src/net/sse.c, reached through the hooks of net_host.h; this file keeps readyState and turns the
 * callbacks into events. The record, its link to the network side and the registry are shared with WebSocket
 * (impl_net.h, impl_websocket.c).
 */
#include "bindings/impl_net.h"

#include "bindings/gen/eventsource.h"
#include "bindings/gen/events.h"

enum { ES_CONNECTING, ES_OPEN, ES_CLOSED };

static nrec *rec_of(void *user)
{
    nlink *l = user;
    nrec *r = l->rec;
    return r != nullptr && !r->dead ? r : nullptr;
}

/* Closed for good (close() or a failure): the connection goes, and so does the pin. */
static void es_finish(nrec *r)
{
    r->ready = ES_CLOSED;
    embi_impl_net_unpin(r);
    if (r->h != nullptr) {
        void *h = r->h;
        r->h = nullptr;
        if (r->drop != nullptr) {
            r->drop(h);
        }
    }
}

static void es_on_open(void *user)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready == ES_CLOSED) {
        return;
    }
    r->ready = ES_OPEN;
    embi_impl_net_fire(r, "open");
}

static void es_on_message(void *user, const char *type, const char *data, size_t len, const char *id, const char *origin)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready != ES_OPEN) {
        return;
    }
    embi_bind_ctx *c = r->c;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value v = embi_bind_pin(c, embi_bind_utf8_value(c, (embi_str){data, (uint32_t)len}));
    embi_impl_net_fire_message(r, type, v, origin, id);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void es_on_error(void *user, bool reconnecting, const char *message)
{
    nrec *r = rec_of(user);
    if (r == nullptr || r->ready == ES_CLOSED) {
        return;
    }
    if (reconnecting) {
        r->ready = ES_CONNECTING;
    } else {
        embi_impl_net_warn(r, "EventSource connection", message);
        r->ready = ES_CLOSED;
    }
    embi_impl_net_fire(r, "error");
    if (!reconnecting && !r->dead) {
        es_finish(r);
    }
}

static void es_release(void *user)
{
    embi_impl_net_link_unref(user);
}

static const embi_netapi_sse_cb k_es_cb = {es_on_open, es_on_message, es_on_error, es_release};

typedef struct ftask {
    nlink *l;
} ftask;

static void fail_run(void *arg)
{
    ftask *t = arg;
    nrec *r = rec_of(t->l);
    if (r != nullptr && r->ready == ES_CONNECTING) {
        es_on_error(t->l, false, "the network is not available");
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

embi_js_value embi_impl_EventSource_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0, embi_dict_EventSourceInit a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str input = embi_bind_utf8(c, a0);
    embi_url base = {0}, u = {0};
    bool has_base = embi_url_parse(embi_bind_url(c), nullptr, &base);
    bool ok = embi_url_parse(input, has_base ? &base : nullptr, &u);
    embi_url_free(&base);
    if (!ok) {
        char msg[300];
        SDL_snprintf(msg, sizeof msg, "Failed to construct 'EventSource': Cannot open an EventSource to '%.*s'. The URL is invalid.",
                     (int)input.len, input.ptr);
        (void)embi_bind_throw_named(c, "SyntaxError", msg);
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    nrec *r = embi_alloc(sizeof *r);
    *r = (nrec){0};
    r->kind = 2;
    r->c = c;
    r->ready = ES_CONNECTING;
    r->creds = a1.withCredentials;
    r->url = SDL_strndup(u.href, u.href_len);
    embi_url_free(&u);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_EventSource, r, r, sizeof *r);
    r->bo = embi_bind_obj_of(v);
    r->bo->release = embi_impl_net_rec_release;
    embi_bind_adopt_new_target(c, v, new_target);
    r->link = embi_impl_net_link_new(r);
    embi_impl_net_start(r);

    const embi_loader_env *env = ldr_env(c);
    if (env->net.sse_open != nullptr) {
        char *origin = embi_impl_net_page_origin(c);
        r->h = env->net.sse_open(env->net.user, r->url, r->creds, origin, &k_es_cb, r->link);
        r->drop = env->net.sse_close;
        SDL_free(origin);
    }
    if (r->h == nullptr) { /* no network: the connection fails, later, with an error event */
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
    return r != nullptr && r->kind == 2 ? r : nullptr;
}

embi_dstr embi_impl_EventSource_get_url(embi_bind_ctx *c, embi_bind_obj *self)
{
    nrec *r = self_rec(self);
    return embi_bind_tmp_utf8(c, (embi_str){r != nullptr ? r->url : "", r != nullptr ? (uint32_t)strlen(r->url) : 0});
}

bool embi_impl_EventSource_get_withCredentials(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    nrec *r = self_rec(self);
    return r != nullptr && r->creds;
}

uint16_t embi_impl_EventSource_get_readyState(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    nrec *r = self_rec(self);
    return r != nullptr ? r->ready : ES_CLOSED;
}

void embi_impl_EventSource_close(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    nrec *r = self_rec(self);
    if (r != nullptr && r->ready != ES_CLOSED) {
        es_finish(r);
    }
}
