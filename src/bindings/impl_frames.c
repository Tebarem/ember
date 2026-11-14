/*
 * iframes in the bindings (IF1, frames.webidl): HTMLIFrameElement.contentWindow and contentDocument,
 * window.parent, window.top and window.frameElement, and the remote window proxies (RemoteWindow).
 *
 * Every frame is its own realm. A window of another frame is a RemoteWindow wrapper that holds only the window id the
 * embedder gave that document (bind.h embi_bind_frames). The context keeps one proxy per id, alive in a persistent handle,
 * so the same document always gives the same object (contentWindow, parent, top and MessageEvent.source compare with ===);
 * a document that replaces it has a new id and so a new proxy. postMessage on a proxy serializes the message in this realm
 * (a DataCloneError throws here) and hands the bytes to the embedder, which queues a task on the receiver's loop; the task
 * comes back through embi_bind_frames_deliver in the receiver's realm.
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"

#include "bindings/gen/frames.h"
#include "bindings/gen/html.h"
#include "bindings/gen/window.h"

/* A cached proxy: the window id and the persistent handle of its wrapper. */
typedef struct fproxy {
    uint32_t id;
    uint32_t slot;
} fproxy;

typedef struct fhost {
    embi_bind_frames h;
    fproxy *proxies;
    uint32_t n, cap;
} fhost;

/* The block behind a RemoteWindow wrapper. */
typedef struct rwin {
    uint32_t id;
} rwin;

void embi_bind_set_frames(embi_bind_ctx *c, const embi_bind_frames *f)
{
    fhost *h = c->frames_host;
    if (h == nullptr) {
        h = SDL_calloc(1, sizeof *h);
        if (h == nullptr) {
            return;
        }
        c->frames_host = h;
    }
    h->h = *f;
}

void embi_impl_frames_release(embi_bind_ctx *c)
{
    fhost *h = c->frames_host;
    if (h == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < h->n; i++) {
        embi_js_persistent_free(c->rt, h->proxies[i].slot);
    }
    SDL_free(h->proxies);
    SDL_free(h);
    c->frames_host = nullptr;
}

/* The proxy of window id in this realm (made on first use), or null for id 0. Proxies of windows that are gone are dropped
 * from the cache as new ones are made. */
static embi_js_value proxy_of(embi_bind_ctx *c, uint32_t id)
{
    fhost *h = c->frames_host;
    if (h == nullptr || id == 0) {
        return EMBI_JS_NULL;
    }
    for (uint32_t i = 0; i < h->n; i++) {
        if (h->proxies[i].id == id) {
            return embi_js_persistent_get(c->rt, h->proxies[i].slot);
        }
    }
    /* Windows that are gone are forgotten before a new one is cached (their proxies stay valid objects, closed). */
    uint32_t k = 0;
    for (uint32_t i = 0; i < h->n; i++) {
        if (h->h.alive != nullptr && !h->h.alive(h->h.user, h->proxies[i].id)) {
            embi_js_persistent_free(c->rt, h->proxies[i].slot);
        } else {
            h->proxies[k++] = h->proxies[i];
        }
    }
    h->n = k;
    if (h->n == h->cap) {
        uint32_t cap = h->cap ? h->cap * 2 : 4;
        fproxy *p = SDL_realloc(h->proxies, (size_t)cap * sizeof *p);
        if (p == nullptr) {
            return EMBI_JS_NULL;
        }
        h->proxies = p;
        h->cap = cap;
    }
    rwin *w = embi_alloc(sizeof *w);
    w->id = id;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_RemoteWindow, w, w, sizeof *w);
    h->proxies[h->n++] = (fproxy){id, embi_js_persistent_new(c->rt, v)};
    return v;
}

static uint32_t rwin_id(embi_bind_obj *self)
{
    const rwin *w = embi_bind_stale(self) ? nullptr : self->ptr;
    return w != nullptr ? w->id : 0;
}

embi_js_value embi_impl_frames_parent(embi_bind_ctx *c, embi_bind_obj *self, bool top)
{
    fhost *h = c->frames_host;
    uint32_t id = 0;
    if (h != nullptr) {
        uint32_t (*fn)(void *) = top ? h->h.top_id : h->h.parent_id;
        id = fn != nullptr ? fn(h->h.user) : 0;
    }
    if (id == 0) {
        return embi_impl_Window_get_window(c, self); /* the top: the window itself */
    }
    return proxy_of(c, id);
}

void embi_bind_frames_deliver(embi_bind_ctx *c, embi_str bytes, embi_str origin, uint32_t source)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value src = embi_bind_pin(c, proxy_of(c, source));
    ldr_msg_fire_window(c, bytes, origin, src);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_bind_fire_window_event(embi_bind_ctx *c, const char *type)
{
    if (c->gbo == nullptr) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, type, EMBI_EVF_TRUSTED, EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        ev = EMBI_JS_UNDEFINED;
    }
    embi_impl_fire(c, c->gbo, type, ev);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ HTMLIFrameElement, Window */

embi_js_value embi_impl_HTMLIFrameElement_get_contentWindow(embi_bind_ctx *c, embi_node *self)
{
    fhost *h = c->frames_host;
    if (h == nullptr || h->h.content_id == nullptr || !embi_is_element(self)) {
        return EMBI_JS_NULL;
    }
    return proxy_of(c, h->h.content_id(h->h.user, embi_as_element(self)));
}

embi_js_value embi_impl_HTMLIFrameElement_get_contentDocument(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return EMBI_JS_NULL; /* every frame is cross-origin */
}

embi_js_value embi_impl_Window_get_frameElement(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return EMBI_JS_NULL;
}

/* ------------------------------------------------------------------ RemoteWindow */

embi_js_value embi_impl_RemoteWindow_get_window(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_bind_obj_value(self);
}

embi_js_value embi_impl_RemoteWindow_get_self(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_bind_obj_value(self);
}

bool embi_impl_RemoteWindow_get_closed(embi_bind_ctx *c, embi_bind_obj *self)
{
    fhost *h = c->frames_host;
    uint32_t id = rwin_id(self);
    return id == 0 || h == nullptr || h->h.alive == nullptr || !h->h.alive(h->h.user, id);
}

embi_dom_err embi_impl_RemoteWindow_postMessage(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1,
                                                embi_js_value a2)
{
    embi_sb bytes = {0}, origin = {0}, target = {0};
    embi_dom_err err = ldr_msg_window_args(c, a0, a1, a2, "Failed to execute 'postMessage' on 'Window'", &bytes, &origin,
                                           &target);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    fhost *h = c->frames_host;
    uint32_t id = rwin_id(self);
    if (h != nullptr && h->h.post != nullptr && id != 0) {
        uint32_t source = h->h.self_id != nullptr ? h->h.self_id(h->h.user) : 0;
        h->h.post(h->h.user, id, source, &bytes, (embi_str){origin.data, origin.len}, (embi_str){target.data, target.len});
    }
    embi_sb_free(&bytes);
    embi_sb_free(&origin);
    embi_sb_free(&target);
    return EMBI_DOM_OK;
}
