/*
 * The network transport of fetch (N1b): an http or https hop with no embedder handler is a request on the
 * runtime's network service (src/net/net.h), posted to the view's thread. The head completes the operation (the response
 * then streams: its body arrives through the operation's sink as the network delivers it), and an abort cancels the request.
 *
 * The net core follows redirects itself (follow and error modes), so the hop is marked followed and the operation does not
 * re-follow; manual redirects are delivered as they are and the operation makes its opaque response. Every event runs on the
 * view's thread (the net request has a post function), so nothing here needs a lock.
 *
 * The hop handle is a net_hop, whose first member is the hop tag (resource.c hop_abort dispatches on it). Two owners hold it:
 * the start call and the request (its release callback). The release callback may run inline inside embi_net_start (the
 * service is shutting down, or the post is refused), before the start call has the request; state tells the start call which
 * happened, so a handle is only handed out for a request that is still alive.
 *
 * ORDERING REQUIRED: the loader detach (loader.c embi_loader_detach) aborts every operation of a context before the view's
 * poster is detached (embi_rt_binding_free). A request still in flight after its poster is detached gets its events dropped
 * (the release callback then runs on the network thread and touches no JS). Do not reorder that.
 */
#include "embed/resource_int.h"

#include "embed/embed_int.h"
#include "loader/fetch.h"
#include "net/net.h"

#include <stdatomic.h>
#include <string.h>

#ifdef EMBER_NET

enum { NET_HOP_STARTING, NET_HOP_HANDED_OUT, NET_HOP_RELEASED };

typedef struct net_hop {
    embi_hop_tag tag;            /* first: EMBI_HOP_NET */
    embi_fetch_op *op;           /* the operation this hop completes; nullptr once the hop is over or abandoned (view thread) */
    atomic_int refs;             /* the start call's reference and the request's (dropped in on_release) */
    atomic_int state;            /* NET_HOP_*: set by the start call and by on_release */
    _Atomic(embi_net_req *) req; /* the caller's reference to the request, dropped by whoever sees it last */
    void *poster;                /* a reference, held while the request can still post */
} net_hop;

static void hop_unref(net_hop *h)
{
    if (atomic_fetch_sub_explicit(&h->refs, 1, memory_order_acq_rel) == 1) {
        embi_rt_poster_unref(h->poster);
        embi_free(h, sizeof *h);
    }
}

/* Drops the caller's reference to the request, once. */
static void hop_drop_req(net_hop *h)
{
    embi_net_req *r = atomic_exchange(&h->req, nullptr);
    if (r != nullptr) {
        embi_net_req_unref(r);
    }
}

/* Gives up the hop: no more operation events, and the request is cancelled (its release callback still comes). */
static void hop_abandon(net_hop *h)
{
    h->op = nullptr;
    embi_net_req *r = atomic_load(&h->req);
    if (r != nullptr) {
        embi_net_abort(r);
    }
}

static embi_net_redirect map_redirect(embi_redirect r)
{
    switch (r) {
    case EMBI_REDIRECT_ERROR:
        return EMBI_NET_REDIRECT_ERROR;
    case EMBI_REDIRECT_MANUAL:
        return EMBI_NET_REDIRECT_MANUAL;
    case EMBI_REDIRECT_FOLLOW:
    default:
        return EMBI_NET_REDIRECT_FOLLOW;
    }
}

static embi_net_creds map_creds(embi_fetch_creds c)
{
    switch (c) {
    case EMBI_FETCH_CREDS_INCLUDE:
        return EMBI_NET_CREDS_INCLUDE;
    case EMBI_FETCH_CREDS_SAME_ORIGIN:
        return EMBI_NET_CREDS_SAME_ORIGIN;
    case EMBI_FETCH_CREDS_OMIT:
    default:
        return EMBI_NET_CREDS_OMIT;
    }
}

static void on_head(void *user, const embi_net_head *head)
{
    net_hop *h = user;
    embi_fetch_op *op = h->op;
    if (op == nullptr) {
        return;
    }
    embi_fetch_options opt;
    embi_fetch_op_options(op, &opt);

    /* The net core's headers are in the stored form, as the fetch core keeps them: they are passed on as they are. */
    uint32_t n = head->headers != nullptr ? head->headers->list.len : 0;
    embi_hdr_view *views = n > 0 ? embi_alloc((size_t)n * sizeof *views) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        const embi_kv *kv = &head->headers->list.data[i];
        views[i] = (embi_hdr_view){{kv->name.data, kv->name.len}, {kv->value.data, kv->value.len}};
    }

    embi_fetch_hop hop = {0};
    hop.status = head->status;
    hop.status_text = (embi_str){head->status_text, head->status_text != nullptr ? (uint32_t)strlen(head->status_text) : 0};
    hop.headers = views;
    hop.nheaders = n;
    hop.url = (embi_str){head->url, (uint32_t)strlen(head->url)};
    hop.followed = opt.redirect != EMBI_REDIRECT_MANUAL;
    hop.redirected = head->redirected;
    hop.cors = true;

    bool has_location = false;
    for (uint32_t i = 0; i < n; i++) {
        if (embi_str_ieq(views[i].name, EMBI_STR("location"))) {
            has_location = true;
            break;
        }
    }
    if (!embi_status_ok(hop.status)) {
        embi_fetch_hop_fail(op, EMBI_STR("invalid response status"));
        hop_abandon(h);
    } else if (opt.redirect == EMBI_REDIRECT_MANUAL && embi_status_redirect(hop.status) && has_location) {
        /* A manual redirect is the response itself: no body, and the rest of the response is not wanted. */
        embi_fetch_hop_done(op, &hop);
        hop_abandon(h);
    } else {
        embi_fetch_hop_stream_begin(op, &hop);
        if (!embi_fetch_op_streaming(op)) {
            /* The operation finished inside the head (done with ok false): nothing else is wanted. */
            hop_abandon(h);
        }
    }

    if (n > 0) {
        embi_free(views, (size_t)n * sizeof *views);
    }
}

static void on_data(void *user, const uint8_t *data, size_t len)
{
    net_hop *h = user;
    if (h->op != nullptr) {
        embi_fetch_hop_stream_chunk(h->op, data, len);
    }
}

static void on_end(void *user)
{
    net_hop *h = user;
    if (h->op != nullptr) {
        embi_fetch_op *op = h->op;
        h->op = nullptr;
        embi_fetch_hop_stream_end(op);
    }
}

static void on_error(void *user, embi_net_err err, const char *message)
{
    (void)err;
    net_hop *h = user;
    if (h->op != nullptr) {
        embi_fetch_op *op = h->op;
        h->op = nullptr;
        embi_fetch_hop_fail(op, (embi_str){message, (uint32_t)strlen(message)});
    }
}

/* The request is over. May run inline inside embi_net_start, before the start call has the request: then the start call finds
 * the state released and drops the reference itself. */
static void on_release(void *user)
{
    net_hop *h = user;
    /* A release while the op still waits on a handed-out hop means the request ended without end or error (the
     * network service stopped with it in flight): fail the op, so it leaves its hop and no later abort reaches this
     * freed handle. Not while STARTING: the start call then refuses the hop itself, and a refused hop must not be
     * completed. The op is set only while the poster is attached, so this runs on the view thread (loader detach
     * aborts every op, clearing h->op, before the poster detaches). */
    embi_fetch_op *op = h->op;
    h->op = nullptr;
    if (op != nullptr && atomic_load(&h->state) == NET_HOP_HANDED_OUT) {
        embi_fetch_hop_fail(op, EMBI_STR("the network service stopped"));
    }
    atomic_store(&h->state, NET_HOP_RELEASED);
    hop_drop_req(h);
    hop_unref(h);
}

/* A NUL-terminated copy of s, freed with embi_free(p, len + 1). */
static char *cstr_copy(embi_str s)
{
    char *p = embi_alloc((size_t)s.len + 1);
    if (s.len > 0) {
        memcpy(p, s.ptr, s.len);
    }
    p[s.len] = '\0';
    return p;
}

bool embi_net_fetch_start(emb_runtime *rt, void *poster, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                          const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    *handle = nullptr;
    if (rt->net == nullptr || poster == nullptr) {
        return false;
    }
    embi_fetch_options opt;
    embi_fetch_op_options(op, &opt);

    /* The request headers (stored form): the forbidden names are dropped (the network sets Host, Cookie, Accept-Encoding and
     * the rest). */
    embi_headers hs = {0};
    hs.guard = EMBI_GUARD_NONE;
    for (uint32_t i = 0; i < nheaders; i++) {
        if (embi_header_forbidden_request(headers[i].name)) {
            continue;
        }
        embi_headers_add_raw(&hs, headers[i].name, headers[i].value); /* stored form; the net core writes the wire form */
    }

    char *m = cstr_copy(method);
    char *u = cstr_copy(url);
    char *o = opt.origin.len > 0 ? cstr_copy(opt.origin) : nullptr;

    net_hop *h = embi_alloc(sizeof *h);
    *h = (net_hop){0};
    h->tag.kind = EMBI_HOP_NET;
    h->op = op;
    atomic_init(&h->refs, 2); /* the start call's and the request's */
    atomic_init(&h->state, NET_HOP_STARTING);
    atomic_init(&h->req, nullptr);
    h->poster = embi_rt_poster_ref(poster);

    embi_net_request r = {0};
    r.method = m;
    r.url = u;
    r.headers = &hs;
    r.body = body;
    r.body_len = body_len;
    r.redirect = map_redirect(opt.redirect);
    r.creds = map_creds(opt.creds);
    r.initiator_origin = o;
    r.timeout_ms = 0;
    r.cb = (embi_net_callbacks){on_head, on_data, on_end, on_error, on_release};
    r.user = h;
    r.post = embi_rt_poster_post;
    r.post_user = poster;
    embi_net_req *req = embi_net_start(rt->net, &r);

    embi_free(m, (size_t)method.len + 1);
    embi_free(u, (size_t)url.len + 1);
    if (o != nullptr) {
        embi_free(o, (size_t)opt.origin.len + 1);
    }
    embi_headers_free(&hs);
    if (req == nullptr) {
        /* No request exists, so its reference to the hop will never be dropped: drop both. */
        h->op = nullptr;
        hop_unref(h);
        hop_unref(h);
        return false;
    }
    atomic_store(&h->req, req);
    int expected = NET_HOP_STARTING;
    if (!atomic_compare_exchange_strong(&h->state, &expected, NET_HOP_HANDED_OUT)) {
        /* on_release already ran (inline): the request is over, and no events will come. Drop the reference here. */
        hop_drop_req(h);
        hop_unref(h); /* the start call's reference; the request's was dropped in on_release */
        return false;
    }
    hop_unref(h); /* the start call's reference: the handle now lives until on_release drops the request's */
    *handle = h;
    return true;
}

void embi_net_fetch_abort(void *handle)
{
    net_hop *h = handle;
    hop_abandon(h);
}

void embi_net_fetch_hold(void *handle, bool on)
{
    net_hop *h = handle;
    embi_net_req *r = atomic_load(&h->req);
    if (r != nullptr) {
        embi_net_req_hold(r, on);
    }
}

#else /* EMBER_NET=OFF: no network hops; the transport refuses http and https as it did before N1. */

bool embi_net_fetch_start(emb_runtime *rt, void *poster, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                          const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    (void)rt;
    (void)poster;
    (void)op;
    (void)method;
    (void)url;
    (void)headers;
    (void)nheaders;
    (void)body;
    (void)body_len;
    *handle = nullptr;
    return false;
}

void embi_net_fetch_abort(void *handle)
{
    (void)handle;
}

void embi_net_fetch_hold(void *handle, bool on)
{
    (void)handle;
    (void)on;
}

#endif
