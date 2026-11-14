/*
 * Network loads of a view (N1d, N1f). See netload.h for the model: one record per URL and document, filled by a request on
 * the network thread; the document side never waits for one (the view's thread does not sleep on the network): it asks, gets
 * "pending" and is told by a task on the view's thread when the record completes.
 *
 * Threads. The network thread runs the callbacks of a request (no post function is given to embi_net_start): they only
 * touch their record under its mutex and, when somebody asked to be told, post one task to the view's event queue. The
 * view's thread owns the table (the list, the request handles, the generation); a record's other fields are shared and
 * guarded by its mutex. A record is reference counted: the table (or the main-document slot), the request (until its
 * release callback) and every posted task hold one each.
 */
#include "embed/netload.h"

#include "embed/embed_int.h"

#ifdef EMBER_NET

#include "embed/doc.h"
#include "embed/resource_int.h"
#include "embed/script_int.h"
#include "embed/url.h"
#include "loader/loader.h"
#include "net/net.h"

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define NL_MAX_BODY ((size_t)256 << 20)
#define NL_WAIT_MS 30000u

/* The time a script, module or stylesheet may take to arrive (a request timeout; settable for tests). The document load does
 * not sleep on it: the request fails with a timeout, the arrival task reports it and the load goes on without the resource. */
static unsigned g_wait_ms = NL_WAIT_MS;

void embi_netload_set_wait_ms(unsigned ms)
{
    g_wait_ms = ms > 0 ? ms : NL_WAIT_MS;
}

/* ------------------------------------------------------------------ poster (to the view's thread) */

typedef struct nl_poster {
    SDL_Mutex *m;
    emb_view *view; /* nullptr once the view is gone */
    atomic_int refs;
    atomic_int all_undelivered; /* the same for every record (the main document and the fonts too): embi_netload_quiet */
    atomic_int undelivered; /* arrival tasks posted for the load's records and not run (or dropped) yet */
} nl_poster;

static void poster_unref(nl_poster *p)
{
    if (atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) == 1) {
        SDL_DestroyMutex(p->m);
        embi_free(p, sizeof *p);
    }
}

static bool poster_post(nl_poster *p, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    SDL_LockMutex(p->m);
    bool ok = p->view != nullptr && embi_view_post_task_ex(p->view, fn, arg, drop);
    SDL_UnlockMutex(p->m);
    return ok;
}

/* ------------------------------------------------------------------ records */

enum { REC_PENDING = 0, REC_OK = 1, REC_FAIL = 2 };

typedef struct nrec nrec;
struct nrec {
    atomic_int refs;
    SDL_Mutex *m;
    nl_poster *poster;
    /* Shared (guarded by m). */
    int state;
    unsigned notify;     /* EMBI_NL_* the document is told about on completion */
    bool main;           /* the main document: a non-2xx response is a result too */
    bool too_big;
    bool refused;        /* refused by the scheme check: no request, only the error event of its <img> */
    bool font;           /* a font: it does not hold the load event */
    bool capped;         /* the request has the per-load timeout (a script, module or stylesheet), not the long one */
    bool taken;          /* the body was handed to its consumer and freed: a new get starts the request again */
    uint16_t status;
    embi_net_err err;
    char msg[160];
    embi_sb body;
    embi_sb mime;
    embi_sb final_url;
    /* The view's thread only. */
    struct embi_netload *nl; /* nullptr once the record left the table */
    uint32_t gen;
    embi_net_req *req;
    char *frag; /* the main document's #fragment (with the #), kept on the document URL; nullptr */
    char *url; /* the request's URL (SDL_malloc'd, NUL-terminated) */
    uint32_t url_len;
    nrec *next;
};

struct embi_netload {
    emb_view *view;
    nl_poster *poster;
    nrec *recs;
    nrec *nav; /* the main-document request in flight */
    uint32_t gen;
    uint32_t pending;
};

static void rec_unref(nrec *r)
{
    if (atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    embi_sb_free(&r->body);
    embi_sb_free(&r->mime);
    embi_sb_free(&r->final_url);
    poster_unref(r->poster);
    SDL_DestroyMutex(r->m);
    SDL_free(r->url);
    SDL_free(r->frag);
    embi_free(r, sizeof *r);
}

static void arrived_task(void *arg);
/* Whether the record's arrival counts for the load event (the main document and the fonts do not). */
static bool counts_for_load(const nrec *r)
{
    return !r->main && !r->font;
}

static void count_arrival(nrec *r)
{
    atomic_fetch_add_explicit(&r->poster->all_undelivered, 1, memory_order_relaxed);
    if (counts_for_load(r)) {
        atomic_fetch_add_explicit(&r->poster->undelivered, 1, memory_order_relaxed);
    }
}

static void arrived_drop(void *arg)
{
    nrec *r = arg;
    atomic_fetch_sub_explicit(&r->poster->all_undelivered, 1, memory_order_relaxed);
    if (counts_for_load(r)) {
        atomic_fetch_sub_explicit(&r->poster->undelivered, 1, memory_order_relaxed);
    }
    rec_unref(r);
}

/* Tells the view's thread that the record completed. The arrival counts as undelivered until its task runs, so the load event
 * does not fire between a record completing and its sibling's arrival being handled. A record that is reachable by another
 * thread (the table) is counted by its finisher inside the lock, before its state leaves PENDING (`counted`); one that is not
 * yet shared is counted here. */
static void rec_post_arrival(nrec *r, bool counted)
{
    atomic_fetch_add_explicit(&r->refs, 1, memory_order_relaxed);
    if (!counted) {
        count_arrival(r);
    }
    if (!poster_post(r->poster, arrived_task, arrived_drop, r)) {
        arrived_drop(r);
    }
}

/* ------------------------------------------------------------------ request callbacks (network thread) */

static void cb_head(void *user, const embi_net_head *h)
{
    nrec *r = user;
    SDL_LockMutex(r->m);
    r->status = h->status;
    embi_str ct = embi_headers_first(h->headers, EMBI_STR("content-type"));
    if (ct.len > 0) {
        embi_sb_append(&r->mime, ct);
    }
    if (h->url != nullptr) {
        embi_sb_append(&r->final_url, (embi_str){h->url, (uint32_t)strlen(h->url)});
    }
    SDL_UnlockMutex(r->m);
}

static void cb_data(void *user, const uint8_t *data, size_t len)
{
    nrec *r = user;
    SDL_LockMutex(r->m);
    if (!r->too_big) {
        if (r->body.len + len > NL_MAX_BODY) {
            r->too_big = true;
        } else {
            embi_sb_append(&r->body, (embi_str){(const char *)data, (uint32_t)len});
        }
    }
    SDL_UnlockMutex(r->m);
}

static void rec_finish(nrec *r, int state)
{
    SDL_LockMutex(r->m);
    if (r->state != REC_PENDING) {
        SDL_UnlockMutex(r->m);
        return;
    }
    bool post = r->notify != 0 || r->main;
    if (post) { /* counted before the state is visible: embi_netload_busy never sees a gap */
        count_arrival(r);
    }
    r->state = state;
    SDL_UnlockMutex(r->m);
    if (post) {
        rec_post_arrival(r, true);
    }
}

static void cb_end(void *user)
{
    nrec *r = user;
    bool ok = !r->too_big && (r->main || (r->status >= 200 && r->status <= 299));
    if (r->too_big) {
        SDL_snprintf(r->msg, sizeof r->msg, "the response is larger than %u MB", (unsigned)(NL_MAX_BODY >> 20));
    } else if (!ok) {
        SDL_snprintf(r->msg, sizeof r->msg, "HTTP status %u", (unsigned)r->status);
    }
    rec_finish(r, ok ? REC_OK : REC_FAIL);
}

static void cb_error(void *user, embi_net_err err, const char *message)
{
    nrec *r = user;
    SDL_LockMutex(r->m);
    r->err = err;
    SDL_snprintf(r->msg, sizeof r->msg, "%s", message != nullptr ? message : "network error");
    SDL_UnlockMutex(r->m);
    rec_finish(r, REC_FAIL);
}

static void cb_release(void *user)
{
    rec_unref(user);
}

/* ------------------------------------------------------------------ the table */

static embi_netload *nl_get(emb_view *v)
{
    if (v->netload == nullptr) {
        embi_netload *nl = embi_alloc(sizeof *nl);
        *nl = (embi_netload){0};
        nl->view = v;
        nl->poster = embi_alloc(sizeof *nl->poster);
        nl->poster->m = SDL_CreateMutex();
        nl->poster->view = v;
        atomic_init(&nl->poster->refs, 1);
        atomic_init(&nl->poster->undelivered, 0);
        atomic_init(&nl->poster->all_undelivered, 0);
        v->netload = nl;
    }
    return v->netload;
}

/* "scheme://host[:port]" of the document, for requests that are same-origin only. */
static void doc_origin(emb_view *v, embi_sb *out)
{
    embi_str du = v->doc != nullptr ? embi_doc_url(v->doc) : (embi_str){"", 0};
    embi_url u = {0};
    if (du.len > 0 && embi_url_parse(du, nullptr, &u)) {
        embi_url_origin(&u, out);
    }
    embi_url_free(&u);
}

/* Starts the request of a new record. */
static nrec *rec_new(embi_netload *nl, embi_str url, bool main);

static nrec *rec_start(embi_netload *nl, embi_str url, unsigned kind, bool main)
{
    emb_view *v = nl->view;
    nrec *r = rec_new(nl, url, main);
    /* Every completion is reported to the view's thread: the document load may be waiting for it (a script, a module, a
     * render-blocking sheet), and an image's failure is an error event, however early it comes. */
    r->notify = main ? 0 : (EMBI_NL_SCRIPT | (kind & EMBI_NL_IMAGE));
    r->capped = !main && (kind & (EMBI_NL_IMAGE | EMBI_NL_FONT)) == 0;
    r->font = !main && (kind & EMBI_NL_FONT) != 0;
    embi_sb origin = {nullptr, 0, 0};
    bool same_origin = kind == EMBI_NL_FONT; /* a font is fetched in CORS anonymous mode: cookies for its own origin only */
    if (!main) {
        doc_origin(v, &origin); /* the initiator of every subresource request */
    }
    embi_net_request rq = {0};
    rq.method = "GET";
    rq.url = r->url;
    rq.redirect = EMBI_NET_REDIRECT_FOLLOW;
    rq.creds = same_origin ? EMBI_NET_CREDS_SAME_ORIGIN : EMBI_NET_CREDS_INCLUDE;
    rq.initiator_origin = origin.len > 0 ? embi_sb_cstr(&origin) : nullptr;
    rq.timeout_ms = r->capped ? g_wait_ms : 120000;
    rq.cb = (embi_net_callbacks){cb_head, cb_data, cb_end, cb_error, cb_release};
    rq.user = r;
    r->req = embi_net_start(v->rt->net, &rq);
    embi_sb_free(&origin);
    if (r->req == nullptr) {
        /* No service: the request never ran, so no release will come. */
        r->state = REC_FAIL;
        SDL_snprintf(r->msg, sizeof r->msg, "no network service");
        if (r->notify != 0) {
            rec_post_arrival(r, false);
        }
        rec_unref(r);
    }
    return r;
}

static nrec *rec_new(embi_netload *nl, embi_str url, bool main)
{
    nrec *r = embi_alloc(sizeof *r);
    *r = (nrec){0};
    atomic_init(&r->refs, 2); /* the owner (table or main slot), the request until its release */
    r->m = SDL_CreateMutex();
    atomic_fetch_add_explicit(&nl->poster->refs, 1, memory_order_relaxed);
    r->poster = nl->poster;
    r->main = main;
    r->nl = nl;
    r->gen = nl->gen;
    r->url = SDL_malloc((size_t)url.len + 1);
    SDL_memcpy(r->url, url.ptr, url.len);
    r->url[url.len] = '\0';
    r->url_len = url.len;
    return r;
}

/* Drops a record from the table: the request is aborted and its handle released. */
static void rec_drop(nrec *r)
{
    if (r->req != nullptr) {
        embi_net_abort(r->req);
        embi_net_req_unref(r->req);
        r->req = nullptr;
    }
    r->nl = nullptr;
    rec_unref(r);
}

static embi_str strip_fragment(embi_str url)
{
    for (uint32_t i = 0; i < url.len; i++) {
        if (url.ptr[i] == '#') {
            url.len = i;
            break;
        }
    }
    return url;
}

static nrec *rec_find(embi_netload *nl, embi_str url)
{
    for (nrec *r = nl->recs; r != nullptr; r = r->next) {
        if (r->url_len == url.len && memcmp(r->url, url.ptr, url.len) == 0) {
            return r;
        }
    }
    return nullptr;
}

static nrec *rec_get(embi_netload *nl, embi_str url, unsigned kind, bool restart)
{
    nrec *r = rec_find(nl, url);
    if (r != nullptr && r->taken && restart) {
        /* Its body went to the consumer and was freed: asked for again, the request is made again. */
        for (nrec **pp = &nl->recs; *pp != nullptr; pp = &(*pp)->next) {
            if (*pp == r) {
                *pp = r->next;
                break;
            }
        }
        r->next = nullptr;
        rec_drop(r);
        r = nullptr;
    }
    if (r == nullptr) {
        r = rec_start(nl, url, kind, false);
        r->next = nl->recs;
        nl->recs = r;
    }
    return r;
}

/* ------------------------------------------------------------------ public */

bool embi_netload_claims(emb_view *v, embi_str url)
{
    return v != nullptr && embi_rt_net_claims(v->rt, url);
}

static bool url_blocked(emb_view *v, embi_str url);

bool embi_netload_allowed(emb_view *v, embi_str url)
{
    return embi_netload_claims(v, url) && !url_blocked(v, url);
}

/* Whether the allowlist refuses the URL's host: such a load fails at once (it is not "pending"), so a consumer does not
 * wait for an arrival that only reports the failure. The request still starts, once, for the network service's warning. */
static bool url_blocked(emb_view *v, embi_str url)
{
    embi_url u = {0};
    if (!embi_url_parse(url, nullptr, &u)) {
        return false;
    }
    int port = u.port >= 0 ? (int)u.port : (u.scheme_len == 5 ? 443 : 80);
    bool blocked = u.has_host && !embi_net_allowed(v->rt->net, (embi_str){u.href + u.host_off, u.host_len}, port);
    embi_url_free(&u);
    return blocked;
}

embi_nl_result embi_netload_get(emb_view *v, embi_str url, unsigned kind, uint8_t **data, size_t *len, embi_sb *mime)
{
    embi_netload *nl = nl_get(v);
    bool blocked = url_blocked(v, url);
    nrec *r = rec_get(nl, strip_fragment(url), kind, true);
    if (blocked) {
        SDL_LockMutex(r->m);
        if (r->state == REC_PENDING) {
            r->notify |= kind; /* the failure is reported to the document (an image's error event) */
        }
        SDL_UnlockMutex(r->m);
        return EMBI_NL_FAIL;
    }
    SDL_LockMutex(r->m);
    if (r->state == REC_PENDING) {
        r->notify |= kind;
        SDL_UnlockMutex(r->m);
        if (kind != 0) {
            nl->pending++;
        }
        return EMBI_NL_PENDING;
    }
    bool ok = r->state == REC_OK;
    if (ok) {
        size_t n = r->body.len;
        uint8_t *buf = SDL_malloc(n > 0 ? n : 1);
        if (buf == nullptr) {
            ok = false;
        } else {
            if (n > 0) {
                SDL_memcpy(buf, r->body.data, n);
            }
            *data = buf;
            *len = n;
            if (mime != nullptr && r->mime.len > 0) {
                embi_sb_append(mime, (embi_str){r->mime.data, r->mime.len});
            }
            /* An image's consumer (the image cache) keeps its own copy: the record's is freed so a page of big images does
             * not hold every image twice. Fonts, sheets and scripts stay for the document's life: the same file used
             * again (several @font-face rules of one font file) is not downloaded again. */
            if ((kind & EMBI_NL_IMAGE) != 0) {
                embi_sb_free(&r->body);
                r->taken = true;
            }
        }
    }
    SDL_UnlockMutex(r->m);
    return ok ? EMBI_NL_OK : EMBI_NL_FAIL;
}

embi_nl_result embi_netload_state(emb_view *v, embi_str url)
{
    embi_netload *nl = nl_get(v);
    embi_str u = strip_fragment(url);
    if (url_blocked(v, u)) {
        return EMBI_NL_FAIL;
    }
    nrec *r = rec_find(nl, u);
    if (r == nullptr) {
        r = rec_get(nl, u, 0, false); /* nobody asked for it yet: the download starts now */
    }
    SDL_LockMutex(r->m);
    int st = r->state;
    SDL_UnlockMutex(r->m);
    return st == REC_PENDING ? EMBI_NL_PENDING : st == REC_OK ? EMBI_NL_OK : EMBI_NL_FAIL;
}

bool embi_netload_busy(emb_view *v)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    if (nl == nullptr) {
        return false;
    }
    /* The records first, the counter second: a finisher counts its arrival before its state leaves PENDING, so a record seen
     * complete here has its arrival in the counter unless that arrival was handled already. */
    bool busy = false;
    for (nrec *r = nl->recs; r != nullptr && !busy; r = r->next) {
        if (r->font) {
            continue;
        }
        SDL_LockMutex(r->m);
        busy = r->state == REC_PENDING;
        SDL_UnlockMutex(r->m);
    }
    return busy || atomic_load_explicit(&nl->poster->undelivered, memory_order_relaxed) > 0;
}

bool embi_netload_quiet(emb_view *v)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    if (nl == nullptr) {
        return true;
    }
    for (nrec *r = nl->recs; r != nullptr; r = r->next) { /* records before the counter, as in embi_netload_busy */
        SDL_LockMutex(r->m);
        bool pending = r->state == REC_PENDING;
        SDL_UnlockMutex(r->m);
        if (pending) {
            return false;
        }
    }
    return atomic_load_explicit(&nl->poster->all_undelivered, memory_order_relaxed) == 0;
}

void embi_netload_prefetch(emb_view *v, embi_str url)
{
    embi_netload *nl = nl_get(v);
    embi_str u = strip_fragment(url);
    if (rec_find(nl, u) == nullptr && !url_blocked(v, u)) {
        (void)rec_get(nl, u, 0, false);
    }
}

bool embi_netload_image_pending(emb_view *v, embi_str url)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    nrec *r = nl != nullptr ? rec_find(nl, strip_fragment(url)) : nullptr;
    if (r == nullptr) {
        return false;
    }
    SDL_LockMutex(r->m);
    bool pending = r->state == REC_PENDING;
    SDL_UnlockMutex(r->m);
    return pending;
}

/* A document from the network does not load local resources: file:, app: and the other schemes of the embedder are refused (one
 * warning); http, https, data, blob and local: (an installed font, not a URL) stay. */
bool embi_netload_scheme_ok(emb_view *v, embi_str url)
{
    if (!v->net_doc) { /* the embedder's own document (whatever its base URL) loads what it likes */
        return true;
    }
    embi_str us;
    if (!embi_url_scheme(url, &us) || embi_url_scheme_is(us, "http") || embi_url_scheme_is(us, "https") ||
        embi_url_scheme_is(us, "data") || embi_url_scheme_is(us, "blob") || embi_url_scheme_is(us, "local")) {
        return true;
    }
    embi_rt_log(v->rt, EMB_LOG_WARN, "net: a page from the network cannot load %.*s (local resources are refused)",
                (int)(url.len > 200 ? 200 : url.len), url.ptr);
    return false;
}

/* A load the document refused (a local scheme from an http(s) page): the failure is recorded so the image's error event fires. */
void embi_netload_refuse_image(emb_view *v, embi_str url)
{
    embi_netload *nl = nl_get(v);
    embi_str u = strip_fragment(url);
    if (rec_find(nl, u) != nullptr) {
        return;
    }
    nrec *r = rec_new(nl, u, false);
    atomic_fetch_sub_explicit(&r->refs, 1, memory_order_relaxed); /* no request: the table is the only owner */
    r->state = REC_FAIL;
    r->notify = EMBI_NL_IMAGE;
    r->refused = true;
    SDL_snprintf(r->msg, sizeof r->msg, "refused");
    r->next = nl->recs;
    nl->recs = r;
    rec_post_arrival(r, false);
}

uint32_t embi_netload_pending_count(emb_view *v)
{
    return v != nullptr && v->netload != nullptr ? v->netload->pending : 0;
}

/* ------------------------------------------------------------------ the main document */

static void html_escape(embi_sb *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '<':
            embi_sb_append(out, EMBI_STR("&lt;"));
            break;
        case '>':
            embi_sb_append(out, EMBI_STR("&gt;"));
            break;
        case '&':
            embi_sb_append(out, EMBI_STR("&amp;"));
            break;
        case '"':
            embi_sb_append(out, EMBI_STR("&quot;"));
            break;
        default:
            embi_sb_append(out, (embi_str){s + i, 1});
            break;
        }
    }
}

/* The error state of a main document that did not load: a small page that names the URL and the reason. */
void embi_netload_show_error(emb_view *v, const char *url, const char *msg)
{
    embi_sb h = {nullptr, 0, 0};
    embi_sb_append(&h, EMBI_STR("<!doctype html><meta charset=utf-8><title>Cannot load page</title>"
                                "<body style=\"font:16px sans-serif;margin:2em;color:#333\">"
                                "<h1>This page cannot be loaded</h1><p>"));
    html_escape(&h, url, strlen(url));
    embi_sb_append(&h, EMBI_STR("</p><p>"));
    html_escape(&h, msg, strlen(msg));
    embi_sb_append(&h, EMBI_STR("</p>"));
    (void)emb_view_load_html(v, h.data, h.len, "about:blank");
    embi_sb_free(&h);
}

/* Whether the Content-Type of the main document is one that parses as HTML (none at all is taken as HTML). */
static bool html_type(const embi_sb *mime)
{
    if (mime->len == 0) {
        return true;
    }
    embi_mime mt = {0};
    bool ok = false;
    if (embi_mime_parse((embi_str){mime->data, mime->len}, &mt)) {
        embi_sb ess = {nullptr, 0, 0};
        embi_mime_essence(&mt, &ess);
        ok = embi_str_ieq((embi_str){ess.data, ess.len}, EMBI_STR("text/html")) ||
             embi_str_ieq((embi_str){ess.data, ess.len}, EMBI_STR("application/xhtml+xml"));
        embi_sb_free(&ess);
    }
    embi_mime_free(&mt);
    return ok;
}

static void nav_done(embi_netload *nl, nrec *r)
{
    emb_view *v = nl->view;
    nl->nav = nullptr;
    SDL_LockMutex(r->m);
    int state = r->state;
    embi_sb body = r->body;
    embi_sb mime = r->mime;
    embi_sb fin = r->final_url;
    embi_net_err err = r->err;
    char msg[sizeof r->msg];
    SDL_memcpy(msg, r->msg, sizeof msg);
    memset(&r->body, 0, sizeof r->body); /* the page takes the buffers */
    memset(&r->mime, 0, sizeof r->mime);
    memset(&r->final_url, 0, sizeof r->final_url);
    SDL_UnlockMutex(r->m);
    char *url = SDL_strdup(r->url);
    char *frag = r->frag != nullptr ? SDL_strdup(r->frag) : nullptr;
    if (r->req != nullptr) {
        embi_net_req_unref(r->req);
        r->req = nullptr;
    }
    r->nl = nullptr;
    rec_unref(r); /* the main slot's reference */
    bool fail = state != REC_OK;
    if (!fail && !html_type(&mime)) {
        fail = true;
        err = EMBI_NET_ERR_PROTOCOL;
        SDL_snprintf(msg, sizeof msg, "the server sent %.100s, not an HTML document", embi_sb_cstr(&mime));
        embi_rt_log(v->rt, EMB_LOG_WARN, "cannot load %s: %s", url, msg);
    }
    if (!fail) {
        /* The document's URL is the final URL of the response, with the #fragment of the one asked for. */
        embi_sb base = {nullptr, 0, 0};
        embi_str f = fin.len > 0 ? (embi_str){fin.data, fin.len} : (embi_str){url, (uint32_t)strlen(url)};
        embi_sb_append(&base, f);
        if (frag != nullptr && memchr(base.data, '#', base.len) == nullptr) {
            embi_sb_append(&base, (embi_str){frag, (uint32_t)strlen(frag)});
        }
        v->net_doc_next = true; /* the page is from the network: emb_view_load_html takes the flag */
        bool loaded = embi_view_load_bytes(v, (const uint8_t *)body.data, body.len, &mime, embi_sb_cstr(&base));
        v->net_doc_next = false; /* a load that failed before parsing leaves it raised */
        if (!loaded) {
            embi_rt_log(v->rt, EMB_LOG_WARN, "cannot load %s: the page could not be laid out", url);
            embi_netload_show_error(v, url, "the page could not be laid out");
        }
        embi_sb_free(&base);
    } else {
        if (state != REC_OK && err != EMBI_NET_ERR_BLOCKED) { /* the network service warned about a blocked host already */
            embi_rt_log(v->rt, EMB_LOG_WARN, "cannot load %s: %s", url, msg);
        }
        embi_netload_show_error(v, url, msg);
    }
    embi_sb_free(&body);
    embi_sb_free(&mime);
    embi_sb_free(&fin);
    SDL_free(url);
    SDL_free(frag);
}

static void cancel_nav(embi_netload *nl)
{
    nrec *r = nl->nav;
    if (r != nullptr) {
        nl->nav = nullptr;
        rec_drop(r);
    }
}

bool embi_netload_navigate(emb_view *v, embi_str url)
{
    if (v == nullptr || !embi_netload_claims(v, url)) {
        return false;
    }
    embi_netload *nl = nl_get(v);
    cancel_nav(nl);
    embi_str bare = strip_fragment(url);
    nl->nav = rec_start(nl, bare, 0, true);
    if (bare.len < url.len) { /* the #fragment stays on the document's URL, it is not sent */
        nl->nav->frag = SDL_malloc((size_t)url.len - bare.len + 1);
        SDL_memcpy(nl->nav->frag, url.ptr + bare.len, url.len - bare.len);
        nl->nav->frag[url.len - bare.len] = 0;
    }
    return true;
}

static void arrived_task(void *arg)
{
    nrec *r = arg;
    embi_netload *nl = r->nl;
    bool recheck = false;
    atomic_fetch_sub_explicit(&r->poster->all_undelivered, 1, memory_order_relaxed);
    if (counts_for_load(r)) { /* this arrival is being handled now; a sibling's still counts until its own task runs */
        atomic_fetch_sub_explicit(&r->poster->undelivered, 1, memory_order_relaxed);
    }
    if (nl != nullptr) {
        if (r == nl->nav) {
            nav_done(nl, r); /* may replace the document, and so reset the table */
        } else if (r->gen == nl->gen && nl->view->doc != nullptr) {
            SDL_LockMutex(r->m);
            unsigned kinds = r->notify;
            bool ok = r->state == REC_OK;
            bool timed_out = r->state == REC_FAIL && r->capped && r->err == EMBI_NET_ERR_TIMEOUT;
            SDL_UnlockMutex(r->m);
            if (timed_out) {
                embi_rt_log(nl->view->rt, EMB_LOG_WARN, "net: gave up waiting for %s after %u ms", r->url, g_wait_ms);
            }
            if (r->refused) { /* nothing was fetched: the layout stands, only the <img> hears about it */
                embi_doc_image_failed(nl->view->doc, (embi_str){r->url, r->url_len});
                recheck = true;
            } else {
                embi_doc_net_arrived(nl->view->doc, kinds, (embi_str){r->url, r->url_len}, ok);
            }
        } else {
            recheck = true;
        }
    } else {
        recheck = true;
    }
    if (recheck && counts_for_load(r) && r->poster->view != nullptr) {
        /* A stale arrival (a dropped record, or an earlier document's): its count may be the last thing the load gate waited
         * for, and nothing else will look at the gate. */
        embi_doc_recheck_gate(r->poster->view->doc);
    }
    rec_unref(r);
}

void embi_netload_reset(emb_view *v)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    if (nl == nullptr) {
        return;
    }
    cancel_nav(nl);
    nrec *r = nl->recs;
    nl->recs = nullptr;
    while (r != nullptr) {
        nrec *next = r->next;
        r->next = nullptr;
        rec_drop(r);
        r = next;
    }
    nl->gen++;
}

void embi_netload_detach(emb_view *v)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    if (nl != nullptr) {
        SDL_LockMutex(nl->poster->m);
        nl->poster->view = nullptr;
        SDL_UnlockMutex(nl->poster->m);
    }
}

void embi_netload_free(emb_view *v)
{
    embi_netload *nl = v != nullptr ? v->netload : nullptr;
    if (nl == nullptr) {
        return;
    }
    embi_netload_detach(v);
    embi_netload_reset(v);
    poster_unref(nl->poster);
    embi_free(nl, sizeof *nl);
    v->netload = nullptr;
}

#else /* !EMBER_NET */

bool embi_netload_claims(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return false;
}

embi_nl_result embi_netload_get(emb_view *v, embi_str url, unsigned kind, uint8_t **data, size_t *len, embi_sb *mime)
{
    (void)v;
    (void)url;
    (void)kind;
    (void)data;
    (void)len;
    (void)mime;
    return EMBI_NL_FAIL;
}

embi_nl_result embi_netload_state(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return EMBI_NL_FAIL;
}

bool embi_netload_quiet(emb_view *v)
{
    (void)v;
    return true;
}

bool embi_netload_busy(emb_view *v)
{
    (void)v;
    return false;
}

void embi_netload_prefetch(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
}

uint32_t embi_netload_pending_count(emb_view *v)
{
    (void)v;
    return 0;
}

bool embi_netload_allowed(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return false;
}

bool embi_netload_image_pending(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return false;
}

void embi_netload_refuse_image(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
}

bool embi_netload_scheme_ok(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return true;
}

void embi_netload_detach(emb_view *v)
{
    (void)v;
}

bool embi_netload_navigate(emb_view *v, embi_str url)
{
    (void)v;
    (void)url;
    return false;
}

void embi_netload_show_error(emb_view *v, const char *url, const char *msg)
{
    (void)v;
    (void)url;
    (void)msg;
}

void embi_netload_reset(emb_view *v)
{
    (void)v;
}

void embi_netload_free(emb_view *v)
{
    (void)v;
}

#endif
