/*
 * Resource handlers v2 (W7c).
 *
 * A v2 registration (embi_v2) occupies a slot of the runtime's handler table: its v1 function is embi_v2_tramp and its
 * user pointer is the record, so the v1 callers (document loads) reach it too, and the fetch transport below finds the
 * record to give a handler the whole request (method, headers, body) and to take the whole response back (status,
 * media type, headers, bytes).
 *
 * Asynchronous requests (emb_request) are reference counted: one reference is held by the requester's side (the
 * registration's live list), one by the handler, which owes exactly one emb_resource_complete. A completion from any
 * thread copies the response into the request, then either finishes at once (inside the handler's start, on the
 * requester's thread) or posts the finishing step to the view's event queue, where it runs on the view's thread. All the
 * requester-side state (the operation, the live list, the aborted flag) is touched on the view's thread only, so the
 * only shared fields are the reference count, the completed flag and the immutable poster.
 *
 * The built-in file: and data: loaders are served through the same transport (as hops with a status, a media type and
 * the bytes), so fetch() sees one interface for every scheme.
 */
#include "embed/resource_int.h"
#include "embed/frames.h"
#include "embed/netapi.h"
#include "embed/netload.h"

#include "bindings/canvas_host.h"
#include "bindings/loader_host.h"
#include "embed/script_int.h"
#include "embed/url.h"
#include "embed_int.h"
#include "loader/loader.h"

#include <ember/ember_resource.h>
#include <ember/ember_script.h>

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

/* ------------------------------------------------------------------ the poster */

/* Where a completion from another thread goes: the view that owns the requester. Detached when the view's document
 * goes away (the view pointer is then null and a late completion is dropped). */
typedef struct embi_poster {
    SDL_Mutex *m;
    emb_view *view;
    atomic_int refs;
} embi_poster;

static embi_poster *poster_new(emb_view *view)
{
    embi_poster *p = embi_alloc(sizeof *p);
    p->m = SDL_CreateMutex();
    p->view = view;
    atomic_init(&p->refs, 1);
    return p;
}

static embi_poster *poster_ref(embi_poster *p)
{
    if (p != nullptr) {
        atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
    }
    return p;
}

static void poster_unref(embi_poster *p)
{
    if (p != nullptr && atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) == 1) {
        SDL_DestroyMutex(p->m);
        embi_free(p, sizeof *p);
    }
}

static void poster_detach(embi_poster *p)
{
    SDL_LockMutex(p->m);
    p->view = nullptr;
    SDL_UnlockMutex(p->m);
}

/* Queues fn(arg) on the view's thread. False when the poster is detached or the view cannot take the task. */
static bool poster_post(embi_poster *p, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    SDL_LockMutex(p->m);
    bool ok = p->view != nullptr && embi_view_post_task_ex(p->view, fn, arg, drop);
    SDL_UnlockMutex(p->m);
    return ok;
}

/* The network glue's hold on a poster (net_fetch.c): a reference for the life of a request. */
void *embi_rt_poster_ref(void *p)
{
    return poster_ref(p);
}

void embi_rt_poster_unref(void *p)
{
    poster_unref(p);
}

bool embi_rt_poster_post(void *p, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    return poster_post(p, fn, drop, arg);
}

/* ------------------------------------------------------------------ records */

typedef struct embi_v2 {
    emb_request *reqs; /* the live asynchronous requests of this registration (requester-side list) */
    bool async;
    emb_resource_v2_fn sync_fn;
    emb_request_start_fn start;
    emb_request_cancel_fn cancel;
    void *user;
    emb_request *last; /* the completed request whose bytes the last v1 call returned; freed by the next call */
    embi_sb last_body; /* a synchronous handler's bytes, kept for the v1 caller until its next call */
} embi_v2;

struct emb_request {
    embi_hop_tag tag;          /* first: hop_abort tells the handles of the network glue apart (EMBI_HOP_REQUEST) */
    atomic_int refs;
    atomic_int completed;      /* set by the first emb_resource_complete */
    embi_v2 *owner;            /* requester side: the registration whose list holds this request; nullptr when off it */
    emb_request *prev, *next;  /* requester side */
    embi_fetch_op *op;         /* requester side: the operation the hop completes (nullptr for a v1 caller or after abort) */
    emb_request_cancel_fn cancel;
    void *cancel_user;
    embi_poster *poster;       /* immutable; a reference. nullptr: completions run on the completing thread */
    SDL_ThreadID start_tid;    /* the thread inside start */
    bool in_start;             /* requester thread: inside the handler's start */
    bool inline_done;          /* the handler completed on the requester's thread inside start */
    bool aborted;              /* requester side: the requester gave up; the result is dropped */
    uint16_t status;           /* the response, written by the completing thread before the finishing step */
    embi_sb mime;
    embi_headers headers;
    embi_sb body;
};

static void request_unref(emb_request *req)
{
    if (atomic_fetch_sub_explicit(&req->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    embi_sb_free(&req->mime);
    embi_headers_free(&req->headers);
    embi_sb_free(&req->body);
    poster_unref(req->poster);
    embi_free(req, sizeof *req);
}

/* link: the request joins the registration's live list (a fetch hop, which the view's thread owns and can abort). Without
 * it (a v1 document load, which cannot wait) the request is on no list: the second reference is the caller's own, held
 * until start returns and the caller has read the result, so a handler that completes from another thread while start
 * is still running only drops its own reference and touches no shared state. */
static emb_request *request_new(embi_v2 *v, embi_poster *poster, bool link)
{
    emb_request *req = embi_alloc(sizeof *req);
    *req = (emb_request){0};
    req->tag.kind = EMBI_HOP_REQUEST;
    atomic_init(&req->refs, 2); /* the requester's (list or local), the handler's completion */
    atomic_init(&req->completed, 0);
    req->poster = poster_ref(poster);
    req->cancel = v->cancel;
    req->cancel_user = v->user;
    req->start_tid = SDL_GetCurrentThreadID();
    if (!link) {
        req->aborted = true; /* nobody delivers a result */
        return req;
    }
    /* Link into the live list. */
    req->owner = v;
    req->next = v->reqs;
    if (v->reqs != nullptr) {
        v->reqs->prev = req;
    }
    v->reqs = req;
    return req;
}

/* Takes the request off its registration's list and drops the requester's reference. Requester thread only. */
static void req_unlink(emb_request *req)
{
    embi_v2 *v = req->owner;
    if (v == nullptr) {
        return;
    }
    if (req->prev != nullptr) {
        req->prev->next = req->next;
    } else {
        v->reqs = req->next;
    }
    if (req->next != nullptr) {
        req->next->prev = req->prev;
    }
    req->owner = nullptr;
    req->prev = nullptr;
    req->next = nullptr;
    request_unref(req);
}

/* Non-2xx responses fail a v1 load (the document path has no status to show). */
static bool status_is_ok(uint32_t status)
{
    uint32_t st = status == 0 ? 200 : status;
    return st >= 200 && st <= 299;
}

/* Hands the completed response to the loader operation of the request (not when the requester aborted it). */
static void request_deliver(emb_request *req)
{
    if (req->aborted || req->op == nullptr) {
        return;
    }
    embi_fetch_op *op = req->op;
    req->op = nullptr;
    uint32_t n = req->headers.list.len;
    embi_hdr_view *hv = n > 0 ? embi_alloc((size_t)n * sizeof *hv) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        embi_kv *kv = &req->headers.list.data[i];
        hv[i] = (embi_hdr_view){{kv->name.data, kv->name.len}, {kv->value.data, kv->value.len}};
    }
    embi_fetch_hop hop = {0};
    hop.status = req->status;
    hop.mime = (embi_str){req->mime.data, req->mime.len};
    hop.headers = hv;
    hop.nheaders = n;
    hop.body = (const uint8_t *)req->body.data;
    hop.body_len = req->body.len;
    embi_fetch_hop_done(op, &hop);
    if (n > 0) {
        embi_free(hv, (size_t)n * sizeof *hv);
    }
}

/* The finishing step of a completion, on the requester's thread: delivers (unless aborted), drops the requester's
 * reference and the handler's. */
static void request_finish(void *arg)
{
    emb_request *req = arg;
    request_deliver(req);
    req_unlink(req);
    request_unref(req); /* the handler's reference */
}

/* A posted finishing step that was discarded unrun (the view navigated or was destroyed). The requester side is torn
 * down by the abort of the context's fetches (which unlinks the request and forgets the operation); only the handler's
 * reference is ours to drop. The request must not be unlinked here: the operation may still hold it as its handle. */
static void request_drop(void *arg)
{
    request_unref(arg);
}

void emb_resource_complete(emb_request *req, const emb_response *resp)
{
    if (req == nullptr) {
        return;
    }
    if (atomic_exchange_explicit(&req->completed, 1, memory_order_acq_rel) != 0) {
        return; /* completed already: the handle may be gone, but a second call is a handler bug and must not corrupt */
    }
    if (resp != nullptr) {
        req->status = resp->status == 0 ? 200 : resp->status;
        if (resp->mime != nullptr && resp->mime_len > 0) {
            embi_sb_append(&req->mime, (embi_str){resp->mime, (uint32_t)resp->mime_len});
        }
        for (size_t i = 0; i < resp->nheaders; i++) {
            const emb_header *hd = &resp->headers[i];
            embi_str n = {hd->name, (uint32_t)hd->name_len};
            if (embi_header_name_ok(n)) {
                /* The handler gives the value as bytes; the stored form is the UTF-8 of their code points. */
                embi_sb v = {nullptr, 0, 0};
                embi_header_value_from_wire((embi_str){hd->value, (uint32_t)hd->value_len}, &v);
                embi_headers_add_raw(&req->headers, n, (embi_str){v.data, v.len});
                embi_sb_free(&v);
            }
        }
        if (resp->data != nullptr && resp->len > 0) {
            embi_sb_append(&req->body, (embi_str){(const char *)resp->data, (uint32_t)resp->len});
        }
    } else {
        req->status = 0; /* a network error: status 0 */
    }
    /* Inside the handler's start, on the requester's thread: the starter finishes the request when start returns. The
     * thread id is compared first: in_start is the requester thread's variable. */
    if (req->start_tid == SDL_GetCurrentThreadID() && req->in_start) {
        req->inline_done = true;
        return;
    }
    if (req->poster == nullptr) {
        request_finish(req); /* no view: the completing thread is the requester's (single-threaded users) */
        return;
    }
    if (!poster_post(req->poster, request_finish, request_drop, req)) {
        /* The view is gone: nobody waits for the result. The requester side was detached with the view, so only the
         * handler's reference is left. */
        request_unref(req);
    }
}

/* ------------------------------------------------------------------ registration */

void embi_v2_free(void *record)
{
    embi_v2 *v = record;
    if (v == nullptr) {
        return;
    }
    /* A registration that goes away (replaced, removed or the runtime destroyed) aborts its live requests: each is
     * marked aborted, taken off the list, and its handler is asked to complete it. A request whose handler has no
     * cancel function stays allocated until that handler completes it (the handler owes one completion). */
    while (v->reqs != nullptr) {
        emb_request *req = v->reqs;
        req->aborted = true;
        embi_fetch_op *op = req->op;
        req->op = nullptr;
        emb_request_cancel_fn cancel = req->cancel;
        void *cancel_user = req->cancel_user;
        bool done = atomic_load_explicit(&req->completed, memory_order_acquire) != 0;
        if (op != nullptr) {
            /* The operation stops being pending without calling the transport's abort again (the request is aborted). */
            embi_fetch_op_abort(op);
        }
        req_unlink(req); /* may free the request when the handler already completed it */
        if (cancel != nullptr && !done) {
            cancel(cancel_user, req);
        }
    }
    if (v->last != nullptr) {
        request_unref(v->last);
    }
    embi_sb_free(&v->last_body);
    embi_free(v, sizeof *v);
}

bool emb_runtime_set_resource_handler_v2(emb_runtime *rt, const char *scheme, emb_resource_v2_fn fn, void *user)
{
    if (fn == nullptr) {
        return embi_rt_handler_set(rt, scheme, nullptr, nullptr);
    }
    embi_v2 *v = embi_alloc(sizeof *v);
    *v = (embi_v2){0};
    v->sync_fn = fn;
    v->user = user;
    if (!embi_rt_handler_set(rt, scheme, embi_v2_tramp, v)) {
        embi_v2_free(v);
        return false;
    }
    return true;
}

bool emb_runtime_set_resource_handler_async(emb_runtime *rt, const char *scheme, emb_request_start_fn start,
                                            emb_request_cancel_fn cancel, void *user)
{
    if (start == nullptr) {
        return embi_rt_handler_set(rt, scheme, nullptr, nullptr);
    }
    embi_v2 *v = embi_alloc(sizeof *v);
    *v = (embi_v2){0};
    v->async = true;
    v->start = start;
    v->cancel = cancel;
    v->user = user;
    if (!embi_rt_handler_set(rt, scheme, embi_v2_tramp, v)) {
        embi_v2_free(v);
        return false;
    }
    return true;
}

/* A v2 handler behind the v1 interface (document loads): GET with no headers, the bytes kept in the record until the next
 * call. An asynchronous handler that does not complete inside start fails the v1 load (the v1 caller cannot wait). */
bool embi_v2_tramp(void *user, const char *url, size_t url_len, emb_resource *out)
{
    embi_v2 *v = user;
    if (v->last != nullptr) {
        request_unref(v->last);
        v->last = nullptr;
    }
    embi_sb_clear(&v->last_body);
    emb_request_info info = {.size = sizeof info, .method = "GET", .method_len = 3, .url = url, .url_len = url_len};
    if (!v->async) {
        emb_response r = {.size = sizeof r};
        if (!v->sync_fn(v->user, &info, &r) || !status_is_ok(r.status)) {
            return false;
        }
        if (r.len > 0) {
            if (r.data == nullptr) {
                return false;
            }
            embi_sb_append(&v->last_body, (embi_str){(const char *)r.data, (uint32_t)r.len});
        }
        out->data = v->last_body.len > 0 ? (const uint8_t *)v->last_body.data : nullptr;
        out->len = v->last_body.len;
        return true;
    }
    emb_request *req = request_new(v, nullptr, false);
    req->in_start = true;
    info.request = req;
    v->start(v->user, &info);
    req->in_start = false;
    if (!req->inline_done) {
        /* The handler completes it later (possibly already, from another thread); the v1 caller gets no bytes. The
         * request is on no list, so that completion frees it on its own thread and touches nothing shared. */
        request_unref(req);
        return false;
    }
    bool ok = req->status != 0 && status_is_ok(req->status);
    if (!ok) {
        request_unref(req);
        request_unref(req);
        return false;
    }
    /* Keep the handler's reference as the "last" slot, drop the caller's. */
    request_unref(req);
    out->data = req->body.len > 0 ? (const uint8_t *)req->body.data : nullptr;
    out->len = req->body.len;
    v->last = req;
    return true;
}

/* ------------------------------------------------------------------ built-in file: and data: hops */

/* A hop served by the runtime itself: file: (the allowed directories) and data:. Returns false for a refused hop (a
 * network error). The status is 404 for an allowed file that cannot be read. */
static bool builtin_hop(emb_runtime *rt, embi_fetch_op *op, embi_str scheme, embi_str method, embi_str url)
{
    bool get = method.len == 3 && memcmp(method.ptr, "GET", 3) == 0;
    bool head = method.len == 4 && memcmp(method.ptr, "HEAD", 4) == 0;
    if (!get && !head) {
        return false;
    }
    embi_fetch_hop hop = {0};
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    uint8_t *file = nullptr;
    size_t flen = 0;
    if (embi_url_scheme_is(scheme, "data")) {
        if (!embi_data_url(url, &mime, &body)) {
            embi_sb_free(&mime);
            embi_sb_free(&body);
            return false;
        }
        hop.status = 200;
        hop.mime = (embi_str){mime.data, mime.len};
        hop.body = (const uint8_t *)body.data;
        hop.body_len = body.len;
    } else if (embi_url_scheme_is(scheme, "blob")) {
        if (!embi_rt_blob_lookup(rt, url, &file, &flen, &mime)) {
            embi_sb_free(&mime);
            return false;
        }
        hop.status = 200;
        hop.mime = (embi_str){mime.data, mime.len};
        hop.body = file;
        hop.body_len = flen;
    } else {
        int r = embi_rt_file_load(rt, url, &file, &flen);
        if (r == 0) {
            return false;
        }
        hop.status = r == 1 ? 200 : 404;
        if (r == 1) {
            hop.mime = embi_mime_from_path(url);
            hop.body = file;
            hop.body_len = flen;
        }
    }
    if (head) {
        hop.body = nullptr;
        hop.body_len = 0;
    }
    embi_fetch_hop_done(op, &hop);
    embi_sb_free(&mime);
    embi_sb_free(&body);
    SDL_free(file);
    return true;
}

/* ------------------------------------------------------------------ the loader's transport */

struct embi_rt_binding {
    emb_runtime *rt;
    embi_poster *poster;
    atomic_int refs; /* the owner's, plus one per in-progress cross-thread post (env_retain) */
    embi_netapi_host net_host; /* N1c: where WebSocket and EventSource events go (netapi.h) */
};

static embi_rt_handler *find_handler(emb_runtime *rt, embi_str scheme)
{
    for (uint32_t i = 0; i < EMBI_RT_MAX_HANDLERS; i++) {
        embi_rt_handler *h = &rt->handlers[i];
        if (h->scheme != nullptr && embi_url_scheme_is(scheme, h->scheme)) {
            return h;
        }
    }
    return nullptr;
}

/* The headers as the handler sees them. Values are stored as UTF-8 of the byte string's code points (0 to 255), so a
 * handler gets them in wire form (one byte per code point, embi_header_value_to_wire); the converted values live in
 * *store, which the caller frees after the call. */
static emb_header *to_emb_headers(const embi_hdr_view *h, uint32_t n, embi_sb *store)
{
    if (n == 0) {
        return nullptr;
    }
    emb_header *out = embi_alloc((size_t)n * sizeof *out);
    size_t *off = embi_alloc((size_t)n * sizeof *off);
    size_t *len = embi_alloc((size_t)n * sizeof *len);
    for (uint32_t i = 0; i < n; i++) {
        off[i] = store->len;
        embi_header_value_to_wire(h[i].value, store);
        len[i] = store->len - off[i];
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (emb_header){h[i].name.ptr, h[i].name.len, len[i] > 0 ? store->data + off[i] : "", len[i]};
    }
    embi_free(off, (size_t)n * sizeof *off);
    embi_free(len, (size_t)n * sizeof *len);
    return out;
}

static bool sync_v2_hop(embi_v2 *v, embi_fetch_op *op, emb_request_info *info)
{
    emb_response r = {.size = sizeof r};
    if (!v->sync_fn(v->user, info, &r)) {
        return false;
    }
    uint32_t n = (uint32_t)r.nheaders;
    embi_hdr_view *hv = n > 0 ? embi_alloc((size_t)n * sizeof *hv) : nullptr;
    /* The values are bytes from the handler; the stored form is the UTF-8 of their code points. */
    embi_sb store = {nullptr, 0, 0};
    size_t *off = n > 0 ? embi_alloc((size_t)n * sizeof *off) : nullptr;
    size_t *len = n > 0 ? embi_alloc((size_t)n * sizeof *len) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        off[i] = store.len;
        embi_header_value_from_wire((embi_str){r.headers[i].value, (uint32_t)r.headers[i].value_len}, &store);
        len[i] = store.len - off[i];
    }
    for (uint32_t i = 0; i < n; i++) {
        hv[i] = (embi_hdr_view){{r.headers[i].name, (uint32_t)r.headers[i].name_len},
                                {len[i] > 0 ? store.data + off[i] : "", (uint32_t)len[i]}};
    }
    embi_fetch_hop hop = {0};
    hop.status = r.status == 0 ? 200 : r.status;
    hop.mime = r.mime != nullptr ? (embi_str){r.mime, (uint32_t)r.mime_len} : (embi_str){nullptr, 0};
    hop.headers = hv;
    hop.nheaders = n;
    hop.body = r.data;
    hop.body_len = r.len;
    embi_fetch_hop_done(op, &hop);
    if (n > 0) {
        embi_free(hv, (size_t)n * sizeof *hv);
        embi_free(off, (size_t)n * sizeof *off);
        embi_free(len, (size_t)n * sizeof *len);
    }
    embi_sb_free(&store);
    return true;
}

static void async_v2_hop(embi_v2 *v, embi_poster *poster, embi_fetch_op *op, void **handle, emb_request_info *info)
{
    emb_request *req = request_new(v, poster, true);
    req->op = op;
    req->in_start = true;
    info->request = req;
    v->start(v->user, info);
    req->in_start = false;
    if (req->inline_done) {
        request_finish(req);
        *handle = nullptr;
    } else {
        *handle = req;
    }
}

static bool hop_start(emb_runtime *rt, embi_poster *poster, embi_fetch_op *op, void **handle, embi_str method,
                      embi_str url, const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body,
                      size_t body_len)
{
    *handle = nullptr;
    embi_str scheme;
    if (!embi_url_scheme(url, &scheme)) {
        return false;
    }
    embi_rt_handler *h = find_handler(rt, scheme);
    if (h == nullptr) {
        if (embi_url_scheme_is(scheme, "data") || embi_url_scheme_is(scheme, "file") ||
            embi_url_scheme_is(scheme, "blob")) {
            return builtin_hop(rt, op, scheme, method, url);
        }
        /* http and https with no embedder handler: the network (N1b). A transport without a view (no poster) has no
         * thread to complete on, so it cannot wait for the network and is refused. */
        if (poster != nullptr && (embi_url_scheme_is(scheme, "http") || embi_url_scheme_is(scheme, "https")) &&
            embi_net_fetch_start(rt, poster, op, handle, method, url, headers, nheaders, body, body_len)) {
            return true;
        }
        embi_rt_log(rt, EMB_LOG_WARN, "no resource handler for %.*s", (int)scheme.len, scheme.ptr);
        return false;
    }
    if (h->fn != embi_v2_tramp) {
        /* A v1 handler: the bytes come whole, status 200, no headers; it sees the URL only (GET). */
        uint8_t *data = nullptr;
        size_t len = 0;
        if (!embi_rt_fetch(rt, url, &data, &len)) {
            return false;
        }
        embi_fetch_hop hop = {0};
        hop.status = 200;
        hop.body = data;
        hop.body_len = len;
        embi_fetch_hop_done(op, &hop);
        SDL_free(data);
        return true;
    }
    embi_sb wire = {nullptr, 0, 0};
    emb_header *eh = to_emb_headers(headers, nheaders, &wire);
    emb_request_info info = {.size = sizeof info,
                             .method = method.ptr,
                             .method_len = method.len,
                             .url = url.ptr,
                             .url_len = url.len,
                             .headers = eh,
                             .nheaders = nheaders,
                             .body = body,
                             .body_len = body_len};
    embi_v2 *v = h->user;
    bool ok = true;
    if (v->async) {
        async_v2_hop(v, poster, op, handle, &info);
    } else {
        ok = sync_v2_hop(v, op, &info);
    }
    if (nheaders > 0) {
        embi_free(eh, (size_t)nheaders * sizeof *eh);
    }
    embi_sb_free(&wire);
    return ok;
}

static void hop_abort(void *user, void *handle)
{
    (void)user;
    if (handle != nullptr && ((embi_hop_tag *)handle)->kind == EMBI_HOP_NET) {
        embi_net_fetch_abort(handle);
        return;
    }
    emb_request *req = handle;
    if (req == nullptr || req->aborted) {
        return;
    }
    /* The operation that called this stops waiting: the request forgets it, leaves the live list, and the handler is
     * asked to give up. The handler still completes it once; that completion is dropped. */
    req->aborted = true;
    req->op = nullptr;
    emb_request_cancel_fn cancel = req->cancel;
    void *cancel_user = req->cancel_user;
    bool done = atomic_load_explicit(&req->completed, memory_order_acquire) != 0;
    req_unlink(req); /* the handler's reference keeps the request alive */
    if (cancel != nullptr && !done) {
        cancel(cancel_user, req);
    }
}

static void hop_hold(void *user, void *handle, bool on)
{
    (void)user;
    if (handle != nullptr && ((embi_hop_tag *)handle)->kind == EMBI_HOP_NET) {
        embi_net_fetch_hold(handle, on);
    }
}

static bool legacy_start(void *user, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                         const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    return hop_start(user, nullptr, op, handle, method, url, headers, nheaders, body, body_len);
}

void embi_rt_fetch_transport(emb_runtime *rt, embi_fetch_transport *t)
{
    t->user = rt;
    t->start = legacy_start;
    t->abort = hop_abort;
    t->hold = hop_hold;
}

static bool bound_start(void *user, embi_fetch_op *op, void **handle, embi_str method, embi_str url,
                        const embi_hdr_view *headers, uint32_t nheaders, const uint8_t *body, size_t body_len)
{
    embi_rt_binding *b = user;
    /* A page from the network cannot fetch local resources either (file:, app: and the embedder's other schemes; see
     * embi_netload_scheme_ok). Every hop comes here, redirects between local handlers included. This runs on the view's
     * thread while the view is alive, so its document can be read. */
    SDL_LockMutex(b->poster->m);
    emb_view *view = b->poster->view;
    SDL_UnlockMutex(b->poster->m);
    if (view != nullptr && !embi_netload_scheme_ok(view, url)) {
        return false;
    }
    return hop_start(b->rt, b->poster, op, handle, method, url, headers, nheaders, body, body_len);
}

embi_rt_binding *embi_rt_binding_new(emb_runtime *rt, emb_view *view)
{
    embi_rt_binding *b = embi_alloc(sizeof *b);
    b->rt = rt;
    b->poster = poster_new(view);
    atomic_init(&b->refs, 1);
    return b;
}

void embi_rt_binding_transport(embi_rt_binding *b, embi_fetch_transport *t)
{
    t->user = b;
    t->start = bound_start;
    t->abort = hop_abort;
    t->hold = hop_hold;
}

static void binding_unref(embi_rt_binding *b)
{
    if (atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1) {
        poster_unref(b->poster);
        embi_free(b, sizeof *b);
    }
}

void embi_rt_binding_free(embi_rt_binding *b)
{
    if (b == nullptr) {
        return;
    }
    poster_detach(b->poster);
    binding_unref(b);
}

bool embi_rt_net_claims(emb_runtime *rt, embi_str url)
{
#ifdef EMBER_NET
    embi_str scheme;
    if (rt == nullptr || rt->net == nullptr || !embi_url_scheme(url, &scheme)) {
        return false;
    }
    if (!embi_url_scheme_is(scheme, "http") && !embi_url_scheme_is(scheme, "https")) {
        return false;
    }
    return find_handler(rt, scheme) == nullptr;
#else
    (void)rt;
    (void)url;
    return false;
#endif
}

/* ------------------------------------------------------------------ the runtime-wide web API state */

/* A directory mounted under a scheme and host (emb_runtime_mount_dir): the files of dir are served as scheme://host/... */
typedef struct embi_mount {
    char *scheme; /* lowercase, SDL_malloc'd */
    char *host;   /* lowercase, SDL_malloc'd */
    char *dir;    /* forward slashes, no trailing slash, SDL_malloc'd */
    void *last;   /* the file served last: its bytes stay valid until the next request (the library reads them then) */
    struct embi_mount *next;
} embi_mount;

typedef struct rt_extra {
    emb_runtime *rt;
    embi_storage *storage;
    embi_blob_urls blobs;
    embi_mount *mounts;
    struct rt_extra *next;
} rt_extra;

static SDL_Mutex *g_extra_lock;
static rt_extra *g_extras;

static void extras_lock(void)
{
    /* Created once; the first runtime of a process is created on one thread before views exist on others. */
    if (g_extra_lock == nullptr) {
        g_extra_lock = SDL_CreateMutex();
    }
    SDL_LockMutex(g_extra_lock);
}

static rt_extra *extra_find(emb_runtime *rt, bool create)
{
    for (rt_extra *e = g_extras; e != nullptr; e = e->next) {
        if (e->rt == rt) {
            return e;
        }
    }
    if (!create) {
        return nullptr;
    }
    rt_extra *e = embi_alloc(sizeof *e);
    *e = (rt_extra){0};
    e->rt = rt;
    e->next = g_extras;
    g_extras = e;
    return e;
}

void embi_rt_extras_free(emb_runtime *rt)
{
    extras_lock();
    rt_extra **pp = &g_extras;
    while (*pp != nullptr && (*pp)->rt != rt) {
        pp = &(*pp)->next;
    }
    rt_extra *e = *pp;
    if (e != nullptr) {
        *pp = e->next;
    }
    SDL_UnlockMutex(g_extra_lock);
    if (e == nullptr) {
        return;
    }
    if (e->storage != nullptr) {
        embi_storage_close(e->storage);
    }
    embi_blob_urls_free(&e->blobs);
    for (embi_mount *mt = e->mounts; mt != nullptr;) {
        embi_mount *next = mt->next;
        SDL_free(mt->scheme);
        SDL_free(mt->host);
        SDL_free(mt->dir);
        SDL_free(mt->last);
        embi_free(mt, sizeof *mt);
        mt = next;
    }
    embi_free(e, sizeof *e);
}

static bool set_storage(emb_runtime *rt, embi_storage *st)
{
    if (rt == nullptr || st == nullptr) {
        if (st != nullptr) {
            embi_storage_close(st);
        }
        return false;
    }
    extras_lock();
    rt_extra *e = extra_find(rt, true);
    bool ok = e->storage == nullptr;
    if (ok) {
        e->storage = st;
    }
    SDL_UnlockMutex(g_extra_lock);
    if (!ok) {
        embi_storage_close(st);
    }
    return ok;
}

bool emb_runtime_set_storage(emb_runtime *rt, const char *org, const char *app)
{
    if (org == nullptr || app == nullptr) {
        return false;
    }
    return set_storage(rt, embi_storage_open(org, app));
}

bool emb_runtime_set_storage_dir(emb_runtime *rt, const char *dir)
{
    if (dir == nullptr) {
        return false;
    }
    embi_storage *st = embi_storage_open_dir(dir);
    return st != nullptr && set_storage(rt, st);
}

/* The storage backend of the runtime, opened memory-only on first use when the embedder named none. The pointer stays
 * valid until the runtime is destroyed. */
static embi_storage *rt_storage(emb_runtime *rt)
{
    extras_lock();
    rt_extra *e = extra_find(rt, true);
    if (e->storage == nullptr) {
        e->storage = embi_storage_open(nullptr, nullptr);
    }
    embi_storage *st = e->storage;
    SDL_UnlockMutex(g_extra_lock);
    return st;
}

static embi_blob_urls *rt_blobs(emb_runtime *rt)
{
    extras_lock();
    rt_extra *e = extra_find(rt, true);
    embi_blob_urls *b = &e->blobs;
    SDL_UnlockMutex(g_extra_lock);
    return b;
}

bool embi_rt_blob_lookup(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len, embi_sb *type)
{
    if (rt == nullptr || data == nullptr || len == nullptr) {
        return false;
    }
    extras_lock();
    rt_extra *e = extra_find(rt, false);
    embi_blob *b = e != nullptr ? embi_blob_url_resolve(&e->blobs, url) : nullptr;
    bool ok = b != nullptr;
    if (ok) {
        size_t n = 0;
        const uint8_t *src = embi_blob_bytes(b, &n);
        uint8_t *copy = SDL_malloc(n > 0 ? n : 1);
        if (copy == nullptr) {
            ok = false;
        } else {
            if (n > 0) {
                SDL_memcpy(copy, src, n);
            }
            *data = copy;
            *len = n;
            if (type != nullptr) {
                embi_sb_append(type, (embi_str){b->type.data, (uint32_t)b->type.len});
            }
        }
    }
    SDL_UnlockMutex(g_extra_lock);
    return ok;
}

/* ------------------------------------------------------------------ synchronous loads of the document side */

typedef struct load_res {
    bool done;
    bool ok;
    embi_response resp;
} load_res;

static void load_done(void *user, embi_fetch_op *op, bool ok)
{
    load_res *r = user;
    r->done = true;
    r->ok = ok;
    if (ok) {
        embi_fetch_op_take(op, &r->resp);
    }
}

bool embi_rt_load(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len, embi_sb *mime)
{
    embi_fetch_transport t;
    embi_rt_fetch_transport(rt, &t);
    embi_fetch_init in = {.method = EMBI_STR("GET"), .url = url, .redirect = EMBI_REDIRECT_FOLLOW};
    load_res r = {0};
    embi_fetch_op *op = embi_fetch_op_new(&t, &in, load_done, &r);
    if (op == nullptr) {
        return false;
    }
    /* A hop the handler completes later is not waited for: a document load is synchronous, so the op is freed (and
     * aborted) below and the load fails. */
    bool ok = r.done && r.ok && r.resp.status >= 200 && r.resp.status <= 299;
    if (ok) {
        size_t n = r.resp.body.bytes.len;
        uint8_t *buf = SDL_malloc(n > 0 ? n : 1);
        if (buf == nullptr) {
            ok = false;
        } else {
            if (n > 0) {
                SDL_memcpy(buf, r.resp.body.bytes.data, n);
            }
            *data = buf;
            *len = n;
            embi_str ct = embi_headers_first(&r.resp.headers, EMBI_STR("content-type"));
            if (ct.len > 0 && mime != nullptr) {
                embi_sb_append(mime, ct);
            }
        }
    }
    embi_fetch_op_free(op);
    embi_response_free(&r.resp);
    return ok;
}

/* ------------------------------------------------------------------ mounted directories */

/* The mount of scheme://host that holds the URL's path, copied into dir (cap bytes). False when none does. */
static bool mount_find(emb_runtime *rt, embi_str scheme, embi_str host, char *dir, size_t cap)
{
    bool found = false;
    extras_lock();
    rt_extra *e = extra_find(rt, false);
    for (embi_mount *mt = e != nullptr ? e->mounts : nullptr; mt != nullptr && !found; mt = mt->next) {
        if (embi_str_ieq(scheme, (embi_str){mt->scheme, (uint32_t)strlen(mt->scheme)}) &&
            embi_str_ieq(host, (embi_str){mt->host, (uint32_t)strlen(mt->host)}) && strlen(mt->dir) < cap) {
            SDL_strlcpy(dir, mt->dir, cap);
            found = true;
        }
    }
    SDL_UnlockMutex(g_extra_lock);
    return found;
}

/* Keeps data as the mount's last file (the previous one is freed). */
static void mount_keep(emb_runtime *rt, embi_str scheme, embi_str host, void *data)
{
    extras_lock();
    rt_extra *e = extra_find(rt, false);
    for (embi_mount *mt = e != nullptr ? e->mounts : nullptr; mt != nullptr; mt = mt->next) {
        if (embi_str_ieq(scheme, (embi_str){mt->scheme, (uint32_t)strlen(mt->scheme)}) &&
            embi_str_ieq(host, (embi_str){mt->host, (uint32_t)strlen(mt->host)})) {
            SDL_free(mt->last);
            mt->last = data;
            break;
        }
    }
    SDL_UnlockMutex(g_extra_lock);
}

/* A request path is safe to map under a directory: no backslash, no colon, no NUL, no ".." segment. */
static bool safe_mount_path(embi_str p)
{
    uint32_t seg = 0;
    for (uint32_t i = 0; i <= p.len; i++) {
        if (i == p.len || p.ptr[i] == '/') {
            if (i - seg == 2 && p.ptr[seg] == '.' && p.ptr[seg + 1] == '.') {
                return false;
            }
            seg = i + 1;
        } else if (p.ptr[i] == '\\' || p.ptr[i] == ':' || p.ptr[i] == '\0') {
            return false;
        }
    }
    return true;
}

/* The v2 handler of a mounted scheme: the request's host picks the directory, the path (decoded) names the file, and
 * "/" is index.html. A missing file is a 404 response; a request for a host or path the mount refuses fails the hop. */
static bool mount_serve(void *user, const emb_request_info *req, emb_response *out)
{
    emb_runtime *rt = user;
    if (req == nullptr || req->url == nullptr) {
        return false;
    }
    embi_url u = {0};
    embi_str in = {req->url, (uint32_t)req->url_len};
    if (!embi_url_parse(in, nullptr, &u) || !u.has_host) {
        embi_url_free(&u);
        return false;
    }
    embi_str scheme = {u.href, u.scheme_len};
    embi_str host = {u.href + u.host_off, u.host_len};
    char dir[1024];
    if (!mount_find(rt, scheme, host, dir, sizeof dir)) {
        embi_url_free(&u);
        return false;
    }
    embi_sb path = {nullptr, 0, 0};
    embi_pct_decode((embi_str){u.href + u.path_off, u.path_len}, &path);
    /* The mount's key is copied: scheme and host point into the parsed URL, which is freed here. */
    embi_sb skey = {nullptr, 0, 0};
    embi_sb hkey = {nullptr, 0, 0};
    embi_sb_append(&skey, scheme);
    embi_sb_append(&hkey, host);
    embi_url_free(&u);
    if (path.len == 0 || (path.len == 1 && path.data[0] == '/')) {
        embi_sb_clear(&path);
        embi_sb_append(&path, EMBI_STR("/index.html"));
    }
    bool ok = safe_mount_path((embi_str){path.data, path.len});
    char full[1200];
    if (ok) {
        int n = SDL_snprintf(full, sizeof full, "%s%.*s", dir, (int)path.len, path.data);
        ok = n > 0 && (size_t)n < sizeof full;
    }
    embi_sb_free(&path);
    if (!ok) {
        embi_sb_free(&skey);
        embi_sb_free(&hkey);
        return false;
    }
    size_t size = 0;
    void *bytes = SDL_LoadFile(full, &size);
    if (bytes == nullptr) {
        embi_sb_free(&skey);
        embi_sb_free(&hkey);
        out->status = 404;
        return true;
    }
    embi_str mime = embi_mime_from_path((embi_str){full, (uint32_t)SDL_strlen(full)});
    out->status = 200;
    out->mime = mime.len > 0 ? mime.ptr : nullptr;
    out->mime_len = mime.len;
    out->data = bytes;
    out->len = size;
    mount_keep(rt, (embi_str){skey.data, skey.len}, (embi_str){hkey.data, hkey.len}, bytes);
    embi_sb_free(&skey);
    embi_sb_free(&hkey);
    return true;
}

bool emb_runtime_mount_dir(emb_runtime *rt, const char *scheme, const char *host, const char *dir)
{
    if (rt == nullptr || scheme == nullptr || host == nullptr || dir == nullptr) {
        return false;
    }
    size_t dn = SDL_strlen(dir);
    if (dn == 0 || dn >= 1024 || SDL_strlen(host) == 0 || SDL_strlen(host) >= 128) {
        return false;
    }
    embi_mount *mt = embi_alloc(sizeof *mt);
    *mt = (embi_mount){0};
    mt->scheme = SDL_strdup(scheme);
    mt->host = SDL_strdup(host);
    mt->dir = SDL_malloc(dn + 1);
    for (size_t i = 0; i <= dn; i++) {
        mt->dir[i] = dir[i] == '\\' ? '/' : dir[i];
    }
    while (dn > 1 && mt->dir[dn - 1] == '/') {
        mt->dir[--dn] = '\0';
    }
    for (char *c = mt->scheme; *c != '\0'; c++) {
        *c = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
    }
    for (char *c = mt->host; *c != '\0'; c++) {
        *c = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
    }
    if (!emb_runtime_set_resource_handler_v2(rt, scheme, mount_serve, rt)) {
        SDL_free(mt->scheme);
        SDL_free(mt->host);
        SDL_free(mt->dir);
        embi_free(mt, sizeof *mt);
        return false;
    }
    extras_lock();
    rt_extra *e = extra_find(rt, true);
    mt->next = e->mounts;
    e->mounts = mt;
    SDL_UnlockMutex(g_extra_lock);
    return true;
}

/* ------------------------------------------------------------------ binding context */

static bool env_post(void *user, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    embi_rt_binding *b = user;
    return poster_post(b->poster, fn, drop, arg);
}

static void env_retain(void *user)
{
    embi_rt_binding *b = user;
    atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
}

static void env_unretain(void *user)
{
    binding_unref(user);
}

static void env_release(void *user)
{
    embi_rt_binding_free(user);
}

void embi_rt_bind_ctx(embi_bind_ctx *c, emb_view *view)
{
    embi_loader_env env = {0};
    if (view != nullptr && view->rt != nullptr) {
        embi_rt_binding *b = embi_rt_binding_new(view->rt, view);
        env.user = b;
        embi_rt_binding_transport(b, &env.transport);
        env.blobs = rt_blobs(view->rt);
        env.storage = rt_storage(view->rt);
        env.post = env_post;
        env.retain = env_retain;
        env.unretain = env_unretain;
        env.release = env_release;
#ifdef EMBER_NET
        b->net_host = (embi_netapi_host){view->rt->net, env_post, env_retain, env_unretain, b};
        embi_netapi_fill(&env.net, &b->net_host);
#endif
    }
    embi_loader_attach(c, &env);
    embi_frames_bind(c, view); /* IF1: contentWindow, parent, top and the frames' postMessage */
#if EMBER_CANVAS
    if (view != nullptr) {
        embi_canvas_env ce;
        embi_view_canvas_env(view, &ce);
        embi_canvas_bind_install(c, &ce);
    }
#endif
}

void embi_rt_unbind_ctx(embi_bind_ctx *c)
{
    embi_loader_detach(c);
}
