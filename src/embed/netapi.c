/*
 * WebSocket and EventSource hooks over the network service (N1c): see netapi.h.
 */
#include "embed/netapi.h"

#ifdef EMBER_NET

#include "net/net.h"
#include "net/sse.h"
#include "net/ws.h"

#include <SDL3/SDL_stdinc.h>

typedef struct glue {
    const embi_netapi_host *host;
    embi_netapi_ws_cb ws;
    embi_netapi_sse_cb sse;
    void *user;
} glue;

static void glue_free(glue *g)
{
    const embi_netapi_host *h = g->host;
    SDL_free(g);
    if (h->unretain != nullptr) {
        h->unretain(h->user);
    }
}

/* ------------------------------------------------------------------ WebSocket */

static void g_ws_open(void *u, const char *protocol)
{
    glue *g = u;
    g->ws.on_open(g->user, protocol);
}

static void g_ws_message(void *u, bool binary, const uint8_t *d, size_t n)
{
    glue *g = u;
    g->ws.on_message(g->user, binary, d, n);
}

static void g_ws_error(void *u, const char *m)
{
    glue *g = u;
    g->ws.on_error(g->user, m);
}

static void g_ws_close(void *u, bool clean, uint16_t code, const char *reason)
{
    glue *g = u;
    g->ws.on_close(g->user, clean, code, reason);
}

static void g_ws_release(void *u)
{
    glue *g = u;
    g->ws.release(g->user);
    glue_free(g);
}

static void *hook_ws_open(void *user, const char *url, const char *const *protocols, uint32_t np, const char *origin,
                          const embi_netapi_ws_cb *cb, void *cb_user)
{
    const embi_netapi_host *host = user;
    if (host->net == nullptr) {
        return nullptr;
    }
    glue *g = SDL_calloc(1, sizeof *g);
    g->host = host;
    g->ws = *cb;
    g->user = cb_user;
    if (host->retain != nullptr) {
        host->retain(host->user);
    }
    embi_ws_request q = {0};
    q.url = url;
    q.protocols = protocols;
    q.nprotocols = np;
    q.origin = origin;
    q.cb = (embi_ws_callbacks){g_ws_open, g_ws_message, g_ws_error, g_ws_close, g_ws_release};
    q.user = g;
    q.post = host->post;
    q.post_user = host->user;
    embi_net_req *r = embi_ws_connect(host->net, &q);
    if (r == nullptr) {
        glue_free(g);
    }
    return r;
}

static bool hook_ws_send(void *h, bool binary, const void *data, size_t len)
{
    return embi_ws_send(h, binary, data, len);
}

static void hook_ws_close(void *h, uint16_t code, const char *reason, size_t len)
{
    embi_ws_close(h, code, reason, len);
}

static uint64_t hook_ws_sent(void *h)
{
    return embi_ws_sent(h);
}

static void hook_ws_free(void *h)
{
    embi_net_abort(h);
    embi_net_req_unref(h);
}

/* ------------------------------------------------------------------ EventSource */

static void g_sse_open(void *u)
{
    glue *g = u;
    g->sse.on_open(g->user);
}

static void g_sse_message(void *u, const char *type, const char *data, size_t len, const char *id, const char *origin)
{
    glue *g = u;
    g->sse.on_message(g->user, type, data, len, id, origin);
}

static void g_sse_error(void *u, bool reconnecting, const char *m)
{
    glue *g = u;
    g->sse.on_error(g->user, reconnecting, m);
}

static void g_sse_release(void *u)
{
    glue *g = u;
    g->sse.release(g->user);
    glue_free(g);
}

static void *hook_sse_open(void *user, const char *url, bool creds, const char *initiator, const embi_netapi_sse_cb *cb,
                           void *cb_user)
{
    const embi_netapi_host *host = user;
    if (host->net == nullptr || host->post == nullptr) {
        return nullptr;
    }
    glue *g = SDL_calloc(1, sizeof *g);
    g->host = host;
    g->sse = *cb;
    g->user = cb_user;
    if (host->retain != nullptr) {
        host->retain(host->user);
    }
    embi_sse_params p = {0};
    p.url = url;
    p.with_credentials = creds;
    p.initiator_origin = initiator;
    p.cb = (embi_sse_callbacks){g_sse_open, g_sse_message, g_sse_error, g_sse_release};
    p.user = g;
    p.post = host->post;
    p.post_user = host->user;
    embi_sse *s = embi_sse_open(host->net, &p);
    if (s == nullptr) {
        glue_free(g);
    }
    return s;
}

static void hook_sse_close(void *h)
{
    embi_sse_close(h);
}

void embi_netapi_fill(embi_netapi_env *out, const embi_netapi_host *host)
{
    *out = (embi_netapi_env){
        .user = (void *)host,
        .ws_open = hook_ws_open,
        .ws_send = hook_ws_send,
        .ws_close = hook_ws_close,
        .ws_sent = hook_ws_sent,
        .ws_free = hook_ws_free,
        .sse_open = hook_sse_open,
        .sse_close = hook_sse_close,
    };
}

#else

void embi_netapi_fill(embi_netapi_env *out, const embi_netapi_host *host)
{
    (void)host;
    *out = (embi_netapi_env){0};
}

#endif
