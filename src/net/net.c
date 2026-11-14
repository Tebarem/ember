/*
 * The network service (N1a): lifecycle and thread, the request state machine (allowlist, DNS, pool, TLS, HTTP/1.1,
 * redirects, decoding, cookies), and event delivery. All socket work runs on the service's own thread; the public
 * entry points only queue work and set flags.
 */
#include "net_int.h"

#include "conn.h"
#include "loader/random.h"
#include "ws_int.h"

#include <stdarg.h>

#define POLL_MS 10u
#define DEFAULT_CONNECT_MS 30000u
#define DEFAULT_IDLE_MS 60000u
/* A post-mode request stops reading its socket while more than this many posted bytes are undelivered. */
#define BACKPRESSURE_BYTES (4 << 20)
#define DATA_COST 128 /* fixed cost charged per queued data event: many tiny events are as heavy as a few big ones */
#define ADDR_TTL_MS 30000u
#define HEAD_LIMIT 65536u
#define FILL_PER_STEP (256u * 1024u)

typedef enum job_state {
    ST_START,
    ST_ACQUIRE,
    ST_CONNECT,
    ST_SEND,
    ST_HEAD,
    ST_BODY,
    ST_WS, /* an open WebSocket (websocket.c) */
} job_state;

struct embi_net_origin {
    embi_net_origin *next;
    embi_net *svc;
    char *host;   /* as the URL serializes it (IPv6 in brackets) */
    int port;
    bool tls;
    NET_Address *addr;
    uint64_t addr_at;
    embi_net_conn *conns[EMBI_NET_MAX_PER_ORIGIN];
    uint32_t nconns;
};

struct embi_net {
    embi_net_log_fn log;
    void *log_user;
    SDL_Mutex *m;
    SDL_Condition *cv;
    SDL_Thread *thread;
    bool started;
    bool quit;
    embi_net_req *incoming_head, *incoming_tail;
    embi_net_pattern *allow;
    uint32_t nallow, allow_cap;
    char *user_agent;
    uint32_t connect_ms, idle_ms;
    SDL_Mutex *jar_m;
    embi_cookiejar jar;
    embi_net_roots roots;
    /* network thread only: */
    embi_net_req *jobs;
    embi_net_origin *origins;
    embi_net_tlsctx *tlsctx;
    bool dirty; /* a connection came free or a request ended: step again before waiting */
};

struct embi_net_req {
    SDL_AtomicInt refs;
    SDL_AtomicInt aborted;
    embi_net *svc;
    embi_net_req *next;
    /* the request, owned */
    char *method;
    embi_url url;
    embi_headers headers;
    uint8_t *body;
    size_t body_len;
    embi_net_redirect redirect;
    embi_net_creds creds;
    char *initiator;
    char *bad; /* why the request cannot start (reported by the first step) */
    embi_ws *ws; /* a WebSocket handshake and connection (N1c), nullptr for a plain request */
    uint32_t start_delay; /* milliseconds to wait before the first step (EventSource reconnection) */
    bool no_idle;         /* no idle timeout while the body streams */
    uint32_t timeout_ms;
    embi_net_callbacks cb;
    void *user;
    embi_net_post_fn post;
    void *post_user;
    /* progress (network thread) */
    job_state state;
    embi_net_origin *org;
    embi_net_conn *conn;
    uint32_t redirects;
    uint32_t retries;
    uint64_t t_start, t_conn, t_activity;
    bool conn_timer;
    bool got_bytes;
    bool finished;
    /* the response in progress */
    embi_http_head head;
    embi_http_body bodyst;
    embi_net_decoder *dec;
    uint64_t raw_bytes;
    SDL_AtomicInt queued; /* bytes of posted body data the consumer has not run yet (backpressure) */
    SDL_AtomicInt held;   /* the consumer asks the request to stop reading (embi_net_req_hold, N1b) */
    bool keepalive;
    bool delivered_head;
    bool head_request;
};

/* ------------------------------------------------------------------ helpers */

static void svc_log(embi_net *n, emb_log_level level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void svc_log(embi_net *n, emb_log_level level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n->log != nullptr) {
        n->log(n->log_user, level, buf);
    } else {
        SDL_Log("%s", buf);
    }
}

static int64_t now_wall_ms(void)
{
    SDL_Time t;
    if (!SDL_GetCurrentTime(&t)) {
        return 0;
    }
    return (int64_t)(t / 1000000);
}

static void req_unref(embi_net_req *r)
{
    if (SDL_AddAtomicInt(&r->refs, -1) != 1) {
        return;
    }
    embi_net_mfree(r->method);
    embi_url_free(&r->url);
    embi_headers_free(&r->headers);
    embi_net_mfree(r->body);
    embi_net_mfree(r->initiator);
    embi_net_mfree(r->bad);
    embi_http_head_free(&r->head);
    embi_net_decoder_free(r->dec);
    embi_ws_free(r->ws);
    embi_net_mfree(r);
}

void embi_net_req_unref(embi_net_req *r)
{
    if (r != nullptr) {
        req_unref(r);
    }
}

/* ------------------------------------------------------------------ events */

typedef enum ev_kind {
    EV_HEAD,
    EV_DATA,
    EV_END,
    EV_ERROR,
    EV_RELEASE,
} ev_kind;

typedef struct net_event {
    embi_net_req *req;
    ev_kind kind;
    bool owned;
    /* head */
    uint16_t status;
    const char *status_text;
    const embi_headers *headers;
    embi_headers headers_copy;
    const char *url;
    bool redirected, tls;
    uint32_t redirect_count;
    int64_t content_length;
    /* data */
    const uint8_t *data;
    size_t len;
    /* error */
    embi_net_err err;
    const char *msg;
} net_event;

static void event_free(net_event *e)
{
    if (e->owned) {
        embi_net_mfree((void *)e->status_text);
        embi_headers_free(&e->headers_copy);
        embi_net_mfree((void *)e->url);
        embi_net_mfree((void *)e->data);
        embi_net_mfree((void *)e->msg);
    }
}

/* Calls the callback for e on the current thread. */
static void invoke(embi_net_req *r, const net_event *e)
{
    switch (e->kind) {
    case EV_HEAD:
        if (r->cb.on_head != nullptr) {
            embi_net_head h = {
                .status = e->status,
                .status_text = e->status_text,
                .headers = e->headers,
                .url = e->url,
                .redirected = e->redirected,
                .redirect_count = e->redirect_count,
                .tls = e->tls,
                .content_length = e->content_length,
            };
            r->cb.on_head(r->user, &h);
        }
        break;
    case EV_DATA:
        if (r->cb.on_data != nullptr) {
            r->cb.on_data(r->user, e->data, e->len);
        }
        break;
    case EV_END:
        if (r->cb.on_end != nullptr) {
            r->cb.on_end(r->user);
        }
        break;
    case EV_ERROR:
        if (r->cb.on_error != nullptr) {
            r->cb.on_error(r->user, e->err, e->msg);
        }
        break;
    case EV_RELEASE:
        if (r->cb.release != nullptr) {
            r->cb.release(r->user);
        }
        break;
    }
}

static void event_run(void *arg)
{
    net_event *e = arg;
    embi_net_req *r = e->req;
    if (e->kind == EV_RELEASE || SDL_GetAtomicInt(&r->aborted) == 0) {
        invoke(r, e);
    }
    if (e->kind == EV_DATA) {
        SDL_AddAtomicInt(&r->queued, -(int)(e->len + DATA_COST));
    }
    event_free(e);
    embi_net_mfree(e);
    req_unref(r);
}

static void event_drop(void *arg)
{
    net_event *e = arg;
    embi_net_req *r = e->req;
    if (e->kind == EV_RELEASE) { /* the queue is gone: the user data are still ours to release */
        invoke(r, e);
    }
    if (e->kind == EV_DATA) {
        SDL_AddAtomicInt(&r->queued, -(int)(e->len + DATA_COST));
    }
    event_free(e);
    embi_net_mfree(e);
    req_unref(r);
}

/* Delivers e: inline on the network thread, or copied and posted. */
static void emit(embi_net_req *r, net_event *e)
{
    e->req = r;
    if (r->post == nullptr) {
        if (e->kind == EV_RELEASE || SDL_GetAtomicInt(&r->aborted) == 0) {
            invoke(r, e);
        }
        return;
    }
    net_event *c = embi_net_zalloc(sizeof *c);
    *c = *e;
    c->owned = true;
    c->status_text = e->status_text != nullptr ? embi_net_strdup(e->status_text) : nullptr;
    c->url = e->url != nullptr ? embi_net_strdup(e->url) : nullptr;
    c->msg = e->msg != nullptr ? embi_net_strdup(e->msg) : nullptr;
    c->headers_copy = (embi_headers){0};
    c->headers = nullptr;
    if (e->headers != nullptr) {
        for (uint32_t i = 0; i < e->headers->list.len; i++) {
            const embi_kv *kv = &e->headers->list.data[i];
            embi_headers_add_raw(&c->headers_copy, (embi_str){kv->name.data, kv->name.len}, (embi_str){kv->value.data, kv->value.len});
        }
        c->headers = &c->headers_copy;
    }
    if (e->data != nullptr && e->len > 0) {
        uint8_t *d = embi_net_zalloc(e->len);
        memcpy(d, e->data, e->len);
        c->data = d;
    } else {
        c->data = nullptr;
    }
    SDL_AddAtomicInt(&r->refs, 1);
    if (e->kind == EV_DATA) {
        SDL_AddAtomicInt(&r->queued, (int)(e->len + DATA_COST));
    }
    if (!r->post(r->post_user, event_run, event_drop, c)) {
        event_drop(c);
    }
}

static void emit_simple(embi_net_req *r, ev_kind kind)
{
    net_event e = {.kind = kind};
    emit(r, &e);
}

static void emit_error(embi_net_req *r, embi_net_err err, const char *msg)
{
    net_event e = {.kind = EV_ERROR, .err = err, .msg = msg};
    emit(r, &e);
}

/* A call queued like an event of the request (websocket.c): on the poster, or inline on the network thread. */
typedef struct call_ev {
    embi_net_req *r;
    void (*fn)(void *);
    void (*drop)(void *);
    void *arg;
    size_t bytes;
    bool always;
} call_ev;

static void call_finish(call_ev *e, bool run)
{
    embi_net_req *r = e->r;
    if (run && (e->always || SDL_GetAtomicInt(&r->aborted) == 0)) {
        e->fn(e->arg);
    } else {
        e->drop(e->arg);
    }
    SDL_AddAtomicInt(&r->queued, -(int)e->bytes);
    embi_net_mfree(e);
    req_unref(r);
}

static void call_run(void *arg)
{
    call_finish(arg, true);
}

static void call_drop(void *arg)
{
    call_finish(arg, false);
}

void embi_net_req_post_call(embi_net_req *r, void (*fn)(void *), void (*drop)(void *), void *arg, size_t bytes, bool always)
{
    if (r->post == nullptr) {
        if (always || SDL_GetAtomicInt(&r->aborted) == 0) {
            fn(arg);
        } else {
            drop(arg);
        }
        return;
    }
    call_ev *e = embi_net_zalloc(sizeof *e);
    *e = (call_ev){r, fn, drop, arg, bytes, always};
    SDL_AddAtomicInt(&r->refs, 1);
    SDL_AddAtomicInt(&r->queued, (int)bytes);
    if (!r->post(r->post_user, call_run, call_drop, e)) {
        call_drop(e);
    }
}

bool embi_net_req_congested(embi_net_req *r)
{
    return r->post != nullptr && SDL_GetAtomicInt(&r->queued) > BACKPRESSURE_BYTES;
}

bool embi_net_req_aborted(embi_net_req *r)
{
    return SDL_GetAtomicInt(&r->aborted) != 0;
}

embi_ws *embi_net_req_ws(embi_net_req *r)
{
    return r->ws;
}

/* ------------------------------------------------------------------ origins and the pool */

static int default_port(const embi_url *u)
{
    return u->scheme_len == 5 ? 443 : 80;
}

static bool url_is_https(const embi_url *u)
{
    return u->scheme_len == 5;
}

static bool url_is_http_or_https(const embi_url *u)
{
    return u->has_host && u->host_len > 0 &&
           ((u->scheme_len == 4 && memcmp(u->href, "http", 4) == 0) || (u->scheme_len == 5 && memcmp(u->href, "https", 5) == 0));
}

static int url_port(const embi_url *u)
{
    return u->port >= 0 ? u->port : default_port(u);
}

static embi_net_origin *origin_find(embi_net *n, const embi_url *u)
{
    embi_str host = {u->href + u->host_off, u->host_len};
    int port = url_port(u);
    bool tls = url_is_https(u);
    for (embi_net_origin *o = n->origins; o != nullptr; o = o->next) {
        if (o->port == port && o->tls == tls && strlen(o->host) == host.len && memcmp(o->host, host.ptr, host.len) == 0) {
            return o;
        }
    }
    embi_net_origin *o = embi_net_zalloc(sizeof *o);
    o->svc = n;
    o->host = embi_net_strndup(host.ptr, host.len);
    o->port = port;
    o->tls = tls;
    o->next = n->origins;
    n->origins = o;
    return o;
}

static void origin_drop_conn(embi_net_origin *o, embi_net_conn *c)
{
    o->svc->dirty = true;
    for (uint32_t i = 0; i < o->nconns; i++) {
        if (o->conns[i] == c) {
            o->conns[i] = o->conns[--o->nconns];
            break;
        }
    }
    embi_net_conn_close(c);
}

/* The name to resolve: bracketless IPv6, and localhost names are loopback v4 (so tests and local servers always end at
 * 127.0.0.1, whatever the resolver prefers). */
static void resolve_name(const char *host, char *out, size_t cap)
{
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[') {
        SDL_snprintf(out, cap, "%.*s", (int)(hl - 2), host + 1);
        return;
    }
    if (strcmp(host, "localhost") == 0 || (hl > 10 && strcmp(host + hl - 10, ".localhost") == 0)) {
        SDL_snprintf(out, cap, "127.0.0.1");
        return;
    }
    SDL_snprintf(out, cap, "%s", host);
}

/* ------------------------------------------------------------------ finishing a request */

static void list_remove(embi_net *n, embi_net_req *r)
{
    for (embi_net_req **pp = &n->jobs; *pp != nullptr; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            return;
        }
    }
}

static void conn_detach(embi_net_req *r, bool reusable)
{
    embi_net_conn *c = r->conn;
    if (c == nullptr) {
        return;
    }
    r->conn = nullptr;
    c->owner = nullptr;
    if (reusable && c->state == CONN_READY && !c->eof && c->rbuf.len == 0) {
        c->used = true;
        c->idle_since = SDL_GetTicks();
        c->org->svc->dirty = true;
        return;
    }
    origin_drop_conn(c->org, c);
}

/* Ends a request: how is EV_END, EV_ERROR, or anything else for an abort (no event but the release). */
static void job_finish(embi_net *n, embi_net_req *r, ev_kind how, embi_net_err err, const char *msg)
{
    if (r->finished) {
        return;
    }
    r->finished = true;
    if (how == EV_END) {
        emit_simple(r, EV_END);
    } else if (how == EV_ERROR) {
        emit_error(r, err, msg);
    }
    emit_simple(r, EV_RELEASE);
    conn_detach(r, false);
    list_remove(n, r);
    req_unref(r); /* the service's reference */
}

static void job_fail(embi_net *n, embi_net_req *r, embi_net_err err, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void job_fail(embi_net *n, embi_net_req *r, embi_net_err err, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    job_finish(n, r, EV_ERROR, err, buf);
}

/* ------------------------------------------------------------------ the request */

static bool token_ok(const char *s)
{
    if (s[0] == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("!#$%&'*+-.^_`|~", c) != nullptr)) {
            return false;
        }
    }
    return true;
}

static bool has_header(const embi_headers *h, const char *lower_name)
{
    return embi_headers_first(h, (embi_str){lower_name, (uint32_t)strlen(lower_name)}).ptr != nullptr;
}

static bool request_cookies_apply(embi_net *n, embi_net_req *r, const embi_url *u)
{
    if (r->creds == EMBI_NET_CREDS_OMIT) {
        return false;
    }
    if (r->creds == EMBI_NET_CREDS_SAME_ORIGIN) {
        if (r->initiator == nullptr) {
            return false;
        }
        embi_sb o = {0};
        embi_url_origin(u, &o);
        bool same = strlen(r->initiator) == o.len && memcmp(r->initiator, o.data, o.len) == 0;
        embi_sb_free(&o);
        return same;
    }
    (void)n;
    return true;
}

/* Serializes the request for the current URL into out. */
static bool build_request(embi_net *n, embi_net_req *r, embi_sb *out, char *err, size_t err_cap)
{
    const embi_url *u = &r->url;
    embi_str host = {u->href + u->host_off, u->host_len};
    embi_sb_append(out, (embi_str){r->method, (uint32_t)strlen(r->method)});
    embi_sb_append_char(out, ' ');
    embi_str path = {u->href + u->path_off, u->path_len};
    if (path.len == 0) {
        embi_sb_append_char(out, '/');
    } else {
        embi_sb_append(out, path);
    }
    if (u->has_query) {
        embi_sb_append_char(out, '?');
        embi_sb_append(out, (embi_str){u->href + u->query_off, u->query_len});
    }
    embi_sb_append(out, EMBI_STR(" HTTP/1.1\r\nHost: "));
    embi_sb_append(out, host);
    if (u->port >= 0 && u->port != default_port(u)) {
        embi_sb_append_char(out, ':');
        embi_sb_append_u64(out, (uint64_t)u->port);
    }
    embi_sb_append(out, EMBI_STR("\r\n"));
    if (!has_header(&r->headers, "user-agent")) {
        SDL_LockMutex(n->m);
        embi_sb_append(out, EMBI_STR("User-Agent: "));
        embi_sb_append(out, (embi_str){n->user_agent, (uint32_t)strlen(n->user_agent)});
        embi_sb_append(out, EMBI_STR("\r\n"));
        SDL_UnlockMutex(n->m);
    }
    if (r->ws == nullptr && !has_header(&r->headers, "accept-encoding")) {
        embi_sb_append(out, EMBI_STR("Accept-Encoding: gzip, deflate, br\r\n"));
    }
    if (!has_header(&r->headers, "accept")) {
        embi_sb_append(out, EMBI_STR("Accept: */*\r\n"));
    }
    for (uint32_t i = 0; i < r->headers.list.len; i++) {
        const embi_kv *kv = &r->headers.list.data[i];
        embi_str name = {kv->name.data, kv->name.len};
        if (embi_str_eq(name, EMBI_STR("host")) || embi_str_eq(name, EMBI_STR("content-length")) ||
            embi_str_eq(name, EMBI_STR("cookie")) || embi_str_eq(name, EMBI_STR("connection")) ||
            embi_str_eq(name, EMBI_STR("transfer-encoding")) || embi_str_eq(name, EMBI_STR("expect")) ||
            embi_str_eq(name, EMBI_STR("upgrade"))) {
            continue;
        }
        if (!embi_header_name_ok(name) || !embi_header_value_ok((embi_str){kv->value.data, kv->value.len})) {
            SDL_snprintf(err, err_cap, "invalid request header");
            return false;
        }
        embi_sb_append(out, name);
        embi_sb_append(out, EMBI_STR(": "));
        embi_header_value_to_wire((embi_str){kv->value.data, kv->value.len}, out);
        embi_sb_append(out, EMBI_STR("\r\n"));
    }
    if (request_cookies_apply(n, r, u)) {
        embi_sb cookies = {0};
        SDL_LockMutex(n->jar_m);
        embi_cookiejar_header(&n->jar, u, true, now_wall_ms(), &cookies);
        SDL_UnlockMutex(n->jar_m);
        if (cookies.len > 0) {
            embi_sb_append(out, EMBI_STR("Cookie: "));
            embi_sb_append(out, (embi_str){cookies.data, cookies.len});
            embi_sb_append(out, EMBI_STR("\r\n"));
        }
        embi_sb_free(&cookies);
    }
    if (r->ws != nullptr) {
        embi_ws_request_headers(r->ws, out);
    }
    bool body_method = strcmp(r->method, "POST") == 0 || strcmp(r->method, "PUT") == 0 || strcmp(r->method, "PATCH") == 0;
    if (r->body_len > 0 || body_method) {
        embi_sb_append(out, EMBI_STR("Content-Length: "));
        embi_sb_append_u64(out, r->body_len);
        embi_sb_append(out, EMBI_STR("\r\n"));
    }
    embi_sb_append(out, EMBI_STR("\r\n"));
    if (r->body_len > 0) {
        embi_sb_append(out, (embi_str){(const char *)r->body, (uint32_t)r->body_len});
    }
    return true;
}

/* ------------------------------------------------------------------ redirects */

static bool is_redirect_status(uint16_t s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

static void remove_header(embi_headers *h, const char *lower_name)
{
    (void)embi_headers_delete(h, (embi_str){lower_name, (uint32_t)strlen(lower_name)});
}

static void reset_response(embi_net_req *r)
{
    embi_http_head_free(&r->head);
    embi_net_decoder_free(r->dec);
    r->dec = nullptr;
    r->raw_bytes = 0;
    r->keepalive = false;
    r->got_bytes = false;
}

/* Handles a redirect response per the Fetch Standard. Returns true when the request continues (or failed) and false
 * when the response is final. */
static bool handle_redirect(embi_net *n, embi_net_req *r, bool conn_reusable)
{
    if (!is_redirect_status(r->head.status)) {
        return false;
    }
    embi_str loc = embi_headers_first(&r->head.headers, EMBI_STR("location"));
    if (loc.ptr == nullptr) {
        return false;
    }
    if (r->redirect == EMBI_NET_REDIRECT_MANUAL) {
        return false;
    }
    if (r->redirect == EMBI_NET_REDIRECT_ERROR) {
        job_fail(n, r, EMBI_NET_ERR_REDIRECT, "redirect mode is error and the server redirected to %.*s", (int)loc.len, loc.ptr);
        return true;
    }
    /* The Location header is bytes; the stored value holds one code point per byte. */
    embi_sb wire = {0};
    embi_header_value_to_wire(loc, &wire);
    embi_url next = {0};
    bool ok = embi_url_parse((embi_str){wire.data, wire.len}, &r->url, &next);
    embi_sb_free(&wire);
    if (!ok || !url_is_http_or_https(&next)) {
        embi_url_free(&next);
        job_fail(n, r, EMBI_NET_ERR_REDIRECT, "bad redirect target");
        return true;
    }
    if (r->redirects >= EMBI_NET_MAX_REDIRECTS) {
        embi_url_free(&next);
        job_fail(n, r, EMBI_NET_ERR_REDIRECT, "too many redirects");
        return true;
    }
    uint16_t status = r->head.status;
    /* Method and body rules (Fetch 4.4 step 12). */
    bool is_get_head = strcmp(r->method, "GET") == 0 || strcmp(r->method, "HEAD") == 0;
    if (((status == 301 || status == 302) && strcmp(r->method, "POST") == 0) || (status == 303 && !is_get_head)) {
        embi_net_mfree(r->method);
        r->method = embi_net_strdup("GET");
        embi_net_mfree(r->body);
        r->body = nullptr;
        r->body_len = 0;
        remove_header(&r->headers, "content-encoding");
        remove_header(&r->headers, "content-language");
        remove_header(&r->headers, "content-location");
        remove_header(&r->headers, "content-type");
    }
    /* A redirect to another origin drops the authorization header. */
    embi_sb o1 = {0}, o2 = {0};
    embi_url_origin(&r->url, &o1);
    embi_url_origin(&next, &o2);
    if (o1.len != o2.len || memcmp(o1.data, o2.data, o1.len) != 0) {
        remove_header(&r->headers, "authorization");
    }
    embi_sb_free(&o1);
    embi_sb_free(&o2);
    embi_url_free(&r->url);
    r->url = next;
    r->redirects++;
    reset_response(r);
    conn_detach(r, conn_reusable);
    r->state = ST_START;
    return true;
}

/* ------------------------------------------------------------------ response head */

static bool token_in_header(const embi_http_head *h, const char *name, const char *token)
{
    for (uint32_t i = 0; i < h->headers.list.len; i++) {
        const embi_kv *kv = &h->headers.list.data[i];
        if (embi_str_ieq((embi_str){kv->name.data, kv->name.len}, (embi_str){name, (uint32_t)strlen(name)}) &&
            embi_http_has_token((embi_str){kv->value.data, kv->value.len}, (embi_str){token, (uint32_t)strlen(token)})) {
            return true;
        }
    }
    return false;
}

static void store_cookies(embi_net *n, embi_net_req *r)
{
    if (!request_cookies_apply(n, r, &r->url)) {
        return;
    }
    int64_t now = now_wall_ms();
    SDL_LockMutex(n->jar_m);
    for (uint32_t i = 0; i < r->head.headers.list.len; i++) {
        const embi_kv *kv = &r->head.headers.list.data[i];
        if (kv->name.len == 10 && memcmp(kv->name.data, "set-cookie", 10) == 0) {
            embi_sb wire = {0};
            embi_header_value_to_wire((embi_str){kv->value.data, kv->value.len}, &wire);
            (void)embi_cookiejar_set(&n->jar, &r->url, (embi_str){wire.data, wire.len}, false, now);
            embi_sb_free(&wire);
        }
    }
    SDL_UnlockMutex(n->jar_m);
}

/* The connection of an open WebSocket leaves the pool (it counts against no origin and is never reused). */
static void origin_forget_conn(embi_net_origin *o, embi_net_conn *c)
{
    for (uint32_t i = 0; i < o->nconns; i++) {
        if (o->conns[i] == c) {
            o->conns[i] = o->conns[--o->nconns];
            break;
        }
    }
}

/* The response to a WebSocket handshake. Returns true: the request moved on (open, or failed). */
static bool ws_process_head(embi_net *n, embi_net_req *r)
{
    char err[300];
    store_cookies(n, r);
    if (r->head.status != 101) {
        job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "WebSocket handshake failed: unexpected response status %u",
                 (unsigned)r->head.status);
        return true;
    }
    if (!embi_ws_accept(r, r->ws, &r->head, err, sizeof err)) {
        job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "%s", err);
        return true;
    }
    origin_forget_conn(r->conn->org, r->conn);
    r->state = ST_WS;
    return true;
}

/* A complete response head is in r->head: decides redirect or final delivery. Returns true when the request moved on
 * (redirect, failure) and false when the body phase starts. */
static bool process_head(embi_net *n, embi_net_req *r)
{
    embi_body_kind kind;
    uint64_t length;
    if (r->ws != nullptr) {
        return ws_process_head(n, r);
    }
    if (!embi_http_body_kind(&r->head, r->head_request, &kind, &length)) {
        job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "invalid message framing (Content-Length)");
        return true;
    }
    r->keepalive = r->head.minor >= 1 ? !token_in_header(&r->head, "connection", "close")
                                      : token_in_header(&r->head, "connection", "keep-alive");
    if (kind == EMBI_BODY_CLOSE) {
        r->keepalive = false;
    }
    store_cookies(n, r);
    if (r->head.status == 101) {
        r->keepalive = false; /* upgrade responses are for N1c; here the connection just ends */
    }
    if (is_redirect_status(r->head.status) && r->redirect != EMBI_NET_REDIRECT_MANUAL &&
        embi_headers_first(&r->head.headers, EMBI_STR("location")).ptr != nullptr) {
        /* The body of a redirect is discarded; the connection is reused only when there is none to read. */
        bool reusable = r->keepalive && (kind == EMBI_BODY_NONE || (kind == EMBI_BODY_LENGTH && length == 0)) &&
                        r->conn->rbuf.len == 0;
        if (handle_redirect(n, r, reusable)) {
            return true;
        }
    }

    /* Final response: set up decoding and framing, deliver the head. */
    bool has_body = kind != EMBI_BODY_NONE;
    embi_str ce = embi_headers_first(&r->head.headers, EMBI_STR("content-encoding"));
    if (has_body && ce.ptr != nullptr) {
        embi_str t = embi_header_trim(ce);
        if (t.len > 0 && !embi_str_ieq(t, EMBI_STR("identity"))) {
            r->dec = embi_net_decoder_new(t);
            if (r->dec == nullptr) {
                job_fail(n, r, EMBI_NET_ERR_DECODE, "unsupported Content-Encoding: %.*s", (int)t.len, t.ptr);
                return true;
            }
        }
    }
    embi_http_body_init(&r->bodyst, kind, length);
    embi_sb url = {0};
    embi_sb_append(&url, (embi_str){r->url.href, r->url.href_len});
    /* The fragment is not part of a response URL record's serialization in Fetch; keep it as the URL has it. */
    net_event e = {
        .kind = EV_HEAD,
        .status = r->head.status,
        .status_text = r->head.reason != nullptr ? r->head.reason : "",
        .headers = &r->head.headers,
        .url = embi_sb_cstr(&url),
        .redirected = r->redirects > 0,
        .redirect_count = r->redirects,
        .tls = url_is_https(&r->url),
        .content_length = (kind == EMBI_BODY_LENGTH && r->dec == nullptr) ? (int64_t)length : -1,
    };
    emit(r, &e);
    embi_sb_free(&url);
    r->delivered_head = true;
    return false;
}

/* ------------------------------------------------------------------ body */

/* Feeds r->conn->rbuf to the body framing and decoder. Returns true when the request ended (success or failure). */
static bool consume_body(embi_net *n, embi_net_req *r, bool eof)
{
    embi_net_conn *c = r->conn;
    embi_buf decoded = {0};
    embi_buf framed = {0};
    bool ended = false;
    if (c->rbuf.len > 0) {
        size_t used = embi_http_body_feed(&r->bodyst, c->rbuf.data, c->rbuf.len, &framed);
        r->raw_bytes += framed.len;
        if (used < c->rbuf.len) {
            memmove(c->rbuf.data, c->rbuf.data + used, c->rbuf.len - used);
        }
        c->rbuf.len -= used;
        if (r->bodyst.bad) {
            embi_buf_free(&framed);
            job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "malformed chunked body");
            return true;
        }
        if (framed.len > 0) {
            if (r->dec != nullptr) {
                if (!embi_net_decoder_feed(r->dec, framed.data, framed.len, &decoded)) {
                    embi_buf_free(&framed);
                    embi_buf_free(&decoded);
                    job_fail(n, r, EMBI_NET_ERR_DECODE, "corrupt content encoding");
                    return true;
                }
            } else {
                decoded = framed;
                framed = (embi_buf){0};
            }
        }
        embi_buf_free(&framed);
    }
    if (r->dec != nullptr && embi_net_decoder_pending(r->dec)) {
        /* More decoded output waits inside the decoder: this step goes out now, the rest follows (drain_decoder). */
        n->dirty = true;
    } else if (r->bodyst.done) {
        if (r->dec != nullptr && r->raw_bytes > 0 && !embi_net_decoder_finish(r->dec, &decoded)) {
            embi_buf_free(&decoded);
            job_fail(n, r, EMBI_NET_ERR_DECODE, "truncated or corrupt content encoding");
            return true;
        }
        if (r->dec != nullptr && embi_net_decoder_pending(r->dec)) {
            n->dirty = true; /* Brotli: the decoded body follows in steps */
        } else {
            ended = true;
        }
    } else if (eof) {
        if (r->bodyst.kind == EMBI_BODY_CLOSE) {
            if (c->tls_unclean) {
                embi_buf_free(&decoded);
                job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "TLS connection closed without close_notify: the body may be truncated");
                return true;
            }
            if (r->dec != nullptr && r->raw_bytes > 0 && !embi_net_decoder_finish(r->dec, &decoded)) {
                embi_buf_free(&decoded);
                job_fail(n, r, EMBI_NET_ERR_DECODE, "truncated or corrupt content encoding");
                return true;
            }
            if (r->dec != nullptr && embi_net_decoder_pending(r->dec)) {
                n->dirty = true;
            } else {
                ended = true;
            }
        } else {
            embi_buf_free(&decoded);
            job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "connection closed before the whole body arrived");
            return true;
        }
    }
    if (decoded.len > 0) {
        net_event e = {.kind = EV_DATA, .data = decoded.data, .len = decoded.len};
        emit(r, &e);
        r->t_activity = SDL_GetTicks();
    }
    embi_buf_free(&decoded);
    if (ended) {
        bool reuse = r->keepalive && r->bodyst.kind != EMBI_BODY_CLOSE && !c->eof;
        /* Finish first (events), then return the connection to the pool for the next request. */
        embi_net_conn *cc = r->conn;
        r->conn = nullptr;
        cc->owner = nullptr;
        if (reuse && c->rbuf.len == 0 && cc->state == CONN_READY) {
            cc->used = true;
            cc->idle_since = SDL_GetTicks();
            n->dirty = true;
        } else {
            origin_drop_conn(cc->org, cc);
        }
        job_finish(n, r, EV_END, 0, nullptr);
        return true;
    }
    return false;
}

/* A request that failed on a reused connection before any response byte is repeated only when repeating is safe. */
static bool method_idempotent(const char *m)
{
    return strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0 || strcmp(m, "OPTIONS") == 0 || strcmp(m, "PUT") == 0 ||
           strcmp(m, "DELETE") == 0 || strcmp(m, "TRACE") == 0;
}

/* Whether the consumer of a post-mode request is more than BACKPRESSURE_BYTES behind. */
static bool over_limit(embi_net_req *r)
{
    return r->post != nullptr && (SDL_GetAtomicInt(&r->queued) > BACKPRESSURE_BYTES || SDL_GetAtomicInt(&r->held) != 0);
}

void embi_net_req_hold(embi_net_req *r, bool on)
{
    SDL_SetAtomicInt(&r->held, on ? 1 : 0);
}

/* Hands out the next step of output the decoder holds. True when the request failed. */
static bool drain_decoder(embi_net *n, embi_net_req *r)
{
    embi_buf out = {0};
    if (!embi_net_decoder_pump(r->dec, &out)) {
        embi_buf_free(&out);
        job_fail(n, r, EMBI_NET_ERR_DECODE, "corrupt content encoding");
        return true;
    }
    if (out.len > 0) {
        net_event e = {.kind = EV_DATA, .data = out.data, .len = out.len};
        emit(r, &e);
        r->t_activity = SDL_GetTicks();
    }
    embi_buf_free(&out);
    n->dirty = true;
    return false;
}

/* ------------------------------------------------------------------ the state machine */

static void step_job(embi_net *n, embi_net_req *r, uint64_t now, uint32_t connect_ms, uint32_t idle_ms);

static bool timed_out(uint64_t now, uint64_t since, uint32_t ms)
{
    return ms != 0 && now > since && now - since > ms;
}

static embi_net_conn *take_idle(embi_net_origin *o)
{
    for (uint32_t i = 0; i < o->nconns; i++) {
        embi_net_conn *c = o->conns[i];
        if (c->owner != nullptr || c->state != CONN_READY) {
            continue;
        }
        if (embi_net_conn_alive(c)) {
            return c;
        }
        origin_drop_conn(o, c);
        i--; /* the slot now holds another connection */
    }
    return nullptr;
}

static void step_job_once(embi_net *n, embi_net_req *r, uint64_t now, uint32_t connect_ms, uint32_t idle_ms);

/* A step can finish the request (job_finish drops the service's reference, and the caller's may be gone already)
 * and still look at r->finished afterwards, so the step holds a reference of its own. */
static void step_job(embi_net *n, embi_net_req *r, uint64_t now, uint32_t connect_ms, uint32_t idle_ms)
{
    SDL_AddAtomicInt(&r->refs, 1);
    step_job_once(n, r, now, connect_ms, idle_ms);
    req_unref(r);
}

static void step_job_once(embi_net *n, embi_net_req *r, uint64_t now, uint32_t connect_ms, uint32_t idle_ms)
{
    char err[300];
    err[0] = '\0';
    for (int guard = 0; guard < 64 && !r->finished; guard++) {
        if (SDL_GetAtomicInt(&r->aborted) != 0) {
            job_finish(n, r, EV_RELEASE, 0, nullptr);
            return;
        }
        if (timed_out(now, r->t_start, r->timeout_ms)) {
            job_fail(n, r, EMBI_NET_ERR_TIMEOUT, "request timed out");
            return;
        }
        if (r->ws != nullptr && r->state != ST_WS && embi_ws_close_early(r->ws)) {
            job_fail(n, r, EMBI_NET_ERR_CONNECT, "WebSocket is closed before the connection is established.");
            return;
        }
        switch (r->state) {
        case ST_START: {
            if (r->start_delay != 0 && now < r->t_start + r->start_delay) {
                return;
            }
            if (r->bad != nullptr) {
                job_fail(n, r, EMBI_NET_ERR_URL, "%s", r->bad);
                return;
            }
            const embi_url *u = &r->url;
            embi_str host = {u->href + u->host_off, u->host_len};
            int port = url_port(u);
            if (!embi_net_allowed(n, host, port)) {
                svc_log(n, EMB_LOG_WARN, "net: request to %.*s:%d blocked (host is not on the allowlist)", (int)host.len,
                        host.ptr, port);
                job_fail(n, r, EMBI_NET_ERR_BLOCKED, "host %.*s is not on the network allowlist", (int)host.len, host.ptr);
                return;
            }
            r->org = origin_find(n, u);
            r->head_request = strcmp(r->method, "HEAD") == 0;
            r->conn_timer = false;
            r->state = ST_ACQUIRE;
            break;
        }
        case ST_ACQUIRE: {
            embi_net_origin *o = r->org;
            embi_net_conn *c = r->ws != nullptr ? nullptr : take_idle(o);
            if (c != nullptr) {
                c->owner = r;
                r->conn = c;
                r->state = ST_SEND;
                break;
            }
            if (o->nconns >= EMBI_NET_MAX_PER_ORIGIN) {
                return; /* wait for a connection to come free */
            }
            if (!r->conn_timer) {
                r->conn_timer = true;
                r->t_conn = now;
            }
            if (timed_out(now, r->t_conn, connect_ms)) {
                job_fail(n, r, EMBI_NET_ERR_TIMEOUT, "connect timed out");
                return;
            }
            if (o->addr != nullptr && o->nconns == 0 && now > o->addr_at && now - o->addr_at > ADDR_TTL_MS &&
                NET_GetAddressStatus(o->addr) != NET_WAITING) {
                NET_UnrefAddress(o->addr);
                o->addr = nullptr;
            }
            if (o->addr == nullptr) {
                char name[300];
                resolve_name(o->host, name, sizeof name);
                o->addr = NET_ResolveHostname(name);
                o->addr_at = now;
                if (o->addr == nullptr) {
                    job_fail(n, r, EMBI_NET_ERR_DNS, "cannot resolve %s: %s", o->host, SDL_GetError());
                    return;
                }
            }
            NET_Status st = NET_GetAddressStatus(o->addr);
            if (st == NET_WAITING) {
                return;
            }
            if (st == NET_FAILURE) {
                SDL_snprintf(err, sizeof err, "cannot resolve %s: %s", o->host, SDL_GetError());
                NET_UnrefAddress(o->addr);
                o->addr = nullptr;
                job_fail(n, r, EMBI_NET_ERR_DNS, "%s", err);
                return;
            }
            c = embi_net_conn_open(o->addr, o->port, err, sizeof err);
            if (c == nullptr) {
                job_fail(n, r, EMBI_NET_ERR_CONNECT, "%s", err);
                return;
            }
            c->org = o;
            c->owner = r;
            o->conns[o->nconns++] = c;
            r->conn = c;
            r->state = ST_CONNECT;
            break;
        }
        case ST_CONNECT: {
            embi_net_conn *c = r->conn;
            if (timed_out(now, r->t_conn, connect_ms)) {
                job_fail(n, r, EMBI_NET_ERR_TIMEOUT, "connect timed out");
                return;
            }
            if (r->org->tls) { /* the client configuration is built here, on the network thread, when roots changed */
                embi_net_tlsctx *nc = embi_net_tlsctx_get(&n->roots, n->tlsctx, err, sizeof err);
                if (nc == nullptr) {
                    job_fail(n, r, EMBI_NET_ERR_TLS, "%s", err);
                    return;
                }
                if (nc != n->tlsctx) {
                    embi_net_tlsctx_unref(n->tlsctx);
                    n->tlsctx = nc;
                } else {
                    embi_net_tlsctx_unref(nc); /* the extra reference taken for us */
                }
            }
            embi_net_err code = EMBI_NET_ERR_CONNECT;
            if (!embi_net_conn_advance(c, r->org->tls, n->tlsctx, r->org->host, &code, err, sizeof err)) {
                job_fail(n, r, code, "%s", err);
                return;
            }
            if (c->state != CONN_READY) {
                return;
            }
            r->state = ST_SEND;
            break;
        }
        case ST_SEND: {
            embi_net_conn *c = r->conn;
            embi_sb req = {0};
            if (!build_request(n, r, &req, err, sizeof err)) {
                embi_sb_free(&req);
                job_fail(n, r, EMBI_NET_ERR_URL, "%s", err);
                return;
            }
            bool ok = embi_net_conn_write(c, req.data, req.len, err, sizeof err);
            embi_sb_free(&req);
            if (!ok) {
                if (c->used && r->retries < 2 && method_idempotent(r->method)) { /* a stale keep-alive connection: once more on another one */
                    r->retries++;
                    conn_detach(r, false);
                    r->state = ST_ACQUIRE;
                    break;
                }
                job_fail(n, r, EMBI_NET_ERR_IO, "%s", err);
                return;
            }
            r->t_activity = now;
            r->state = ST_HEAD;
            break;
        }
        case ST_HEAD: {
            embi_net_conn *c = r->conn;
            if (timed_out(now, r->t_activity, idle_ms)) {
                job_fail(n, r, EMBI_NET_ERR_TIMEOUT, "timed out waiting for the response");
                return;
            }
            size_t before = c->rbuf.len;
            fill_rc fr = embi_net_conn_fill(c, FILL_PER_STEP, err, sizeof err);
            if (c->rbuf.len > before) {
                r->t_activity = now;
                r->got_bytes = true;
            }
            for (;;) {
                size_t used = 0;
                embi_http_head h;
                embi_http_parse pr = embi_http_parse_head(c->rbuf.data, c->rbuf.len, &used, &h);
                if (pr == EMBI_HTTP_BAD) {
                    job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "malformed HTTP response");
                    return;
                }
                if (pr == EMBI_HTTP_NEED_MORE) {
                    break;
                }
                memmove(c->rbuf.data, c->rbuf.data + used, c->rbuf.len - used);
                c->rbuf.len -= used;
                if (h.status < 200 && h.status != 101) { /* an interim response (100, 102, 103) */
                    embi_http_head_free(&h);
                    continue;
                }
                embi_http_head_free(&r->head);
                r->head = h;
                if (process_head(n, r)) { /* a redirect (state is ST_START again) or a failure */
                    break;
                }
                r->state = ST_BODY;
                break;
            }
            if (r->finished) {
                return;
            }
            if (r->state != ST_HEAD) {
                break; /* BODY, or START after a redirect: go round again */
            }
            /* Still waiting for the head. */
            if (fr == FILL_EOF || fr == FILL_ERROR) {
                if (!r->got_bytes && c->used && r->retries < 2 && method_idempotent(r->method)) {
                    r->retries++;
                    conn_detach(r, false);
                    r->state = ST_ACQUIRE;
                    break;
                }
                if (fr == FILL_ERROR) {
                    job_fail(n, r, r->got_bytes ? EMBI_NET_ERR_IO : EMBI_NET_ERR_CONNECT, "%s", err);
                } else {
                    job_fail(n, r, r->got_bytes ? EMBI_NET_ERR_PROTOCOL : EMBI_NET_ERR_CONNECT,
                             "connection closed before the response head");
                }
                return;
            }
            if (c->rbuf.len > HEAD_LIMIT) {
                job_fail(n, r, EMBI_NET_ERR_PROTOCOL, "response head too large");
                return;
            }
            return;
        }
        case ST_WS:
            if (embi_ws_step(r, r->ws, r->conn, now)) {
                job_finish(n, r, EV_RELEASE, 0, nullptr);
            }
            return;
        case ST_BODY: {
            embi_net_conn *c = r->conn;
            /* Backpressure: while the consumer is behind (posted data not yet delivered), neither decode nor read
             * the socket, and the idle timer does not run. */
            if (over_limit(r)) {
                r->t_activity = now;
                return;
            }
            if (r->dec != nullptr && embi_net_decoder_pending(r->dec)) {
                if (drain_decoder(n, r) || embi_net_decoder_pending(r->dec)) {
                    return;
                }
            }
            if (!r->no_idle && timed_out(now, r->t_activity, idle_ms)) {
                job_fail(n, r, EMBI_NET_ERR_TIMEOUT, "timed out waiting for the response body");
                return;
            }
            size_t before = c->rbuf.len;
            fill_rc fr = embi_net_conn_fill(c, FILL_PER_STEP, err, sizeof err);
            if (c->rbuf.len > before) {
                r->t_activity = now;
            }
            if (consume_body(n, r, fr == FILL_EOF || c->eof)) {
                return;
            }
            if (fr == FILL_ERROR) {
                job_fail(n, r, EMBI_NET_ERR_IO, "%s", err);
            }
            return;
        }
        }
    }
}

/* ------------------------------------------------------------------ the thread */

/* Milliseconds until the first idle pooled connection is due to be closed; 0 when there is none. */
static uint32_t idle_wait_ms(embi_net *n, uint64_t now)
{
    uint64_t best = 0;
    for (embi_net_origin *o = n->origins; o != nullptr; o = o->next) {
        for (uint32_t i = 0; i < o->nconns; i++) {
            embi_net_conn *c = o->conns[i];
            if (c->owner == nullptr && c->state == CONN_READY) {
                uint64_t due = c->idle_since + n->idle_ms + 1;
                uint64_t left = due > now ? due - now : 1;
                if (best == 0 || left < best) {
                    best = left;
                }
            }
        }
    }
    return best > 60000 ? 60000 : (uint32_t)best;
}

static void reap_idle(embi_net *n, uint64_t now)
{
    for (embi_net_origin *o = n->origins; o != nullptr; o = o->next) {
        for (uint32_t i = 0; i < o->nconns;) {
            embi_net_conn *c = o->conns[i];
            if (c->owner == nullptr && c->state == CONN_READY && now > c->idle_since && now - c->idle_since > n->idle_ms) {
                origin_drop_conn(o, c);
            } else {
                i++;
            }
        }
    }
}

static int net_main(void *data)
{
    embi_net *n = data;
    bool inited = NET_Init();
    uint32_t cap = 0;
    NET_StreamSocket **socks = nullptr;
    for (;;) {
        SDL_LockMutex(n->m);
        while (!n->quit && n->incoming_head == nullptr && n->jobs == nullptr) {
            /* With no work the thread still wakes up to close pooled connections that outlived the idle timeout. */
            uint32_t wake = idle_wait_ms(n, SDL_GetTicks());
            if (wake == 0) {
                SDL_WaitCondition(n->cv, n->m);
            } else if (!SDL_WaitConditionTimeout(n->cv, n->m, (int32_t)wake)) {
                break;
            }
        }
        if (n->quit) {
            SDL_UnlockMutex(n->m);
            break;
        }
        while (n->incoming_head != nullptr) {
            embi_net_req *r = n->incoming_head;
            n->incoming_head = r->next;
            if (n->incoming_head == nullptr) {
                n->incoming_tail = nullptr;
            }
            r->next = nullptr;
            embi_net_req **pp = &n->jobs; /* append: older requests are served first */
            while (*pp != nullptr) {
                pp = &(*pp)->next;
            }
            *pp = r;
        }
        uint32_t connect_ms = n->connect_ms, idle_ms = n->idle_ms;
        SDL_UnlockMutex(n->m);

        uint64_t now = SDL_GetTicks();
        for (int round = 0; round < 4; round++) {
            n->dirty = false;
            for (embi_net_req *r = n->jobs; r != nullptr;) {
                embi_net_req *next = r->next; /* a step ends only its own request */
                step_job(n, r, now, connect_ms, idle_ms);
                r = next;
            }
            if (!n->dirty) {
                break;
            }
        }
        reap_idle(n, now);

        /* Wait for socket activity, or a short while when only DNS or the queue is pending. */
        uint32_t ns = 0;
        bool lagging = false;
        int32_t lag_ms = POLL_MS; /* 2 ms while a consumer drains its queue; a held request (embi_net_req_hold) waits for its reader */
        bool busy = false; /* a job without a socket that must be stepped soon (DNS, connect, queue) */
        bool socketless = false; /* busy because of a job without a socket (not only a lagging consumer) */
        uint64_t sleep_due = 0;
        for (embi_net_req *r = n->jobs; r != nullptr; r = r->next) {
            if (r->conn == nullptr || r->conn->sock == nullptr) {
                if (r->state == ST_START && r->start_delay != 0 && now < r->t_start + r->start_delay) {
                    uint64_t due = r->t_start + r->start_delay; /* a reconnection waiting out its delay */
                    if (sleep_due == 0 || due < sleep_due) {
                        sleep_due = due;
                    }
                } else {
                    busy = socketless = true;
                }
            }
            if (r->conn != nullptr && r->conn->sock != nullptr && over_limit(r)) {
                busy = lagging = true; /* the consumer is behind: its socket is not polled (it would be readable at once), re-check soon */
                if (SDL_GetAtomicInt(&r->queued) > BACKPRESSURE_BYTES) {
                    lag_ms = 2;
                }
            } else if (r->conn != nullptr && r->conn->sock != nullptr) {
                if (ns == cap) {
                    uint32_t ncap = cap ? cap * 2 : 16;
                    socks = embi_net_realloc(socks, sizeof *socks * ncap);
                    cap = ncap;
                }
                socks[ns++] = r->conn->sock;
            }
        }
        if (ns > 0) {
            if (NET_WaitUntilInputAvailable((void **)socks, (int)ns, lagging ? lag_ms : (int32_t)POLL_MS) < 0) {
                SDL_Delay(1);
            }
        } else if (n->jobs != nullptr) {
            SDL_LockMutex(n->m);
            if (!n->quit && n->incoming_head == nullptr) {
                uint64_t t = SDL_GetTicks();
                int32_t ms = socketless || !lagging ? 2 : lag_ms;
                if (!busy && sleep_due != 0) { /* only delayed starts: sleep until the first is due (a new request wakes us) */
                    uint64_t left = sleep_due > t ? sleep_due - t : 1;
                    /* At most 50 ms: embi_net_abort only sets a flag (it must stay safe after the service is gone), so an
                     * EventSource closed during its reconnection delay is noticed on the next pass. */
                    ms = left > 50 ? 50 : (int32_t)left;
                }
                SDL_WaitConditionTimeout(n->cv, n->m, ms);
            }
            SDL_UnlockMutex(n->m);
        }
    }
    /* Shutdown: every request ends (release only), every connection closes. */
    while (n->jobs != nullptr) {
        job_finish(n, n->jobs, EV_RELEASE, 0, nullptr);
    }
    SDL_LockMutex(n->m);
    while (n->incoming_head != nullptr) {
        embi_net_req *r = n->incoming_head;
        n->incoming_head = r->next;
        r->next = n->jobs;
        n->jobs = r;
    }
    n->incoming_tail = nullptr;
    SDL_UnlockMutex(n->m);
    while (n->jobs != nullptr) {
        job_finish(n, n->jobs, EV_RELEASE, 0, nullptr);
    }
    while (n->origins != nullptr) {
        embi_net_origin *o = n->origins;
        n->origins = o->next;
        while (o->nconns > 0) {
            embi_net_conn_close(o->conns[--o->nconns]);
        }
        if (o->addr != nullptr) {
            NET_UnrefAddress(o->addr);
        }
        embi_net_mfree(o->host);
        embi_net_mfree(o);
    }
    embi_net_tlsctx_unref(n->tlsctx);
    n->tlsctx = nullptr;
    embi_net_mfree(socks);
    if (inited) {
        NET_Quit();
    }
    return 0;
}

/* ------------------------------------------------------------------ public */

embi_net *embi_net_create(embi_net_log_fn log, void *log_user)
{
    embi_net *n = embi_net_zalloc(sizeof *n);
    n->log = log;
    n->log_user = log_user;
    n->m = SDL_CreateMutex();
    n->cv = SDL_CreateCondition();
    n->jar_m = SDL_CreateMutex();
    n->connect_ms = DEFAULT_CONNECT_MS;
    n->idle_ms = DEFAULT_IDLE_MS;
    n->user_agent = embi_net_strdup("Ember/" EMB_VERSION_STRING);
    embi_net_roots_init(&n->roots);
    embi_rng_set_entropy(embi_net_entropy); /* crypto.getRandomValues and randomUUID seed from the OS source too */
    return n;
}

void embi_net_destroy(embi_net *n)
{
    if (n == nullptr) {
        return;
    }
    SDL_LockMutex(n->m);
    n->quit = true;
    SDL_BroadcastCondition(n->cv);
    SDL_UnlockMutex(n->m);
    if (n->thread != nullptr) {
        SDL_WaitThread(n->thread, nullptr);
    } else {
        /* No thread ever ran: requests queued without one (thread creation failed) end here. */
        while (n->incoming_head != nullptr) {
            embi_net_req *r = n->incoming_head;
            n->incoming_head = r->next;
            r->next = nullptr;
            r->finished = true;
            emit_simple(r, EV_RELEASE);
            req_unref(r);
        }
    }
    for (uint32_t i = 0; i < n->nallow; i++) {
        embi_net_pattern_free(&n->allow[i]);
    }
    embi_net_mfree(n->allow);
    embi_net_mfree(n->user_agent);
    embi_cookiejar_free(&n->jar);
    embi_net_roots_free(&n->roots);
    SDL_DestroyMutex(n->jar_m);
    SDL_DestroyCondition(n->cv);
    SDL_DestroyMutex(n->m);
    embi_net_mfree(n);
}

bool embi_net_allow_add(embi_net *n, const char *pattern)
{
    embi_net_pattern p;
    if (n == nullptr || !embi_net_pattern_parse(pattern, &p)) {
        return false;
    }
    SDL_LockMutex(n->m);
    if (n->nallow == n->allow_cap) {
        uint32_t ncap = n->allow_cap ? n->allow_cap * 2 : 8;
        n->allow = embi_net_realloc(n->allow, sizeof *n->allow * ncap);
        n->allow_cap = ncap;
    }
    n->allow[n->nallow++] = p;
    SDL_UnlockMutex(n->m);
    return true;
}

bool embi_net_allowed(embi_net *n, embi_str host, int port)
{
    bool ok = false;
    SDL_LockMutex(n->m);
    for (uint32_t i = 0; i < n->nallow && !ok; i++) {
        ok = embi_net_pattern_match(&n->allow[i], host, port);
    }
    SDL_UnlockMutex(n->m);
    return ok;
}

bool embi_net_add_root(embi_net *n, const void *pem_or_der, size_t len)
{
    return n != nullptr && embi_net_roots_add(&n->roots, pem_or_der, len);
}

void embi_net_use_bundled_roots(embi_net *n, bool on)
{
    if (n != nullptr) {
        embi_net_roots_use_bundled(&n->roots, on);
    }
}

void embi_net_set_user_agent(embi_net *n, const char *ua)
{
    if (n == nullptr || ua == nullptr) {
        return;
    }
    char *copy = embi_net_strdup(ua);
    SDL_LockMutex(n->m);
    char *old = n->user_agent;
    n->user_agent = copy;
    SDL_UnlockMutex(n->m);
    embi_net_mfree(old);
}

void embi_net_set_timeouts(embi_net *n, uint32_t connect_ms, uint32_t idle_ms)
{
    SDL_LockMutex(n->m);
    if (connect_ms != 0) {
        n->connect_ms = connect_ms;
    }
    if (idle_ms != 0) {
        n->idle_ms = idle_ms;
    }
    SDL_UnlockMutex(n->m);
}

embi_net_req *embi_net_start(embi_net *n, const embi_net_request *q)
{
    return embi_net_start_ex(n, q, nullptr);
}

embi_net_req *embi_net_start_ex(embi_net *n, const embi_net_request *q, embi_ws *ws)
{
    if (n == nullptr || q == nullptr) {
        embi_ws_free(ws);
        return nullptr;
    }
    embi_net_req *r = embi_net_zalloc(sizeof *r);
    r->ws = ws;
    r->start_delay = q->delay_ms;
    r->no_idle = q->stream;
    SDL_SetAtomicInt(&r->refs, 2); /* the caller's and the service's */
    r->svc = n;
    r->method = embi_net_strdup(q->method != nullptr ? q->method : "GET");
    r->redirect = q->redirect;
    r->creds = q->creds;
    r->initiator = q->initiator_origin != nullptr ? embi_net_strdup(q->initiator_origin) : nullptr;
    r->timeout_ms = q->timeout_ms;
    r->cb = q->cb;
    r->user = q->user;
    r->post = q->post;
    r->post_user = q->post_user;
    if (q->headers != nullptr) {
        for (uint32_t i = 0; i < q->headers->list.len; i++) {
            const embi_kv *kv = &q->headers->list.data[i];
            embi_headers_add_raw(&r->headers, (embi_str){kv->name.data, kv->name.len}, (embi_str){kv->value.data, kv->value.len});
        }
    }
    bool bodyless = strcmp(r->method, "GET") == 0 || strcmp(r->method, "HEAD") == 0;
    if (!bodyless && q->body != nullptr && q->body_len > 0) {
        r->body = embi_net_zalloc(q->body_len);
        memcpy(r->body, q->body, q->body_len);
        r->body_len = q->body_len;
    }
    r->t_start = SDL_GetTicks();
    r->state = ST_START;
    /* The URL and method are validated here; a bad one ends the request with an error event from the network thread
     * like any other failure. */
    if (q->url == nullptr || !embi_url_parse((embi_str){q->url, (uint32_t)strlen(q->url)}, nullptr, &r->url)) {
        r->bad = embi_net_strdup("not a valid URL");
    } else if (!url_is_http_or_https(&r->url)) {
        r->bad = embi_net_strdup("only http and https URLs can be fetched over the network");
    } else if (!token_ok(r->method)) {
        r->bad = embi_net_strdup("invalid method");
    }

    SDL_LockMutex(n->m);
    bool ok = !n->quit;
    if (ok && !n->started) {
        n->thread = SDL_CreateThread(net_main, "ember-net", n);
        n->started = n->thread != nullptr;
        ok = n->started;
    }
    if (ok) {
        r->next = nullptr;
        if (n->incoming_tail != nullptr) {
            n->incoming_tail->next = r;
        } else {
            n->incoming_head = r;
        }
        n->incoming_tail = r;
        SDL_SignalCondition(n->cv);
    }
    SDL_UnlockMutex(n->m);
    if (!ok) {
        /* Shutting down, or no thread: the request ends right here with an error and its release. */
        r->finished = true;
        emit_error(r, EMBI_NET_ERR_CONNECT, "the network service is not available");
        emit_simple(r, EV_RELEASE);
        req_unref(r); /* the service's reference */
    }
    return r;
}

void embi_net_abort(embi_net_req *r)
{
    if (r != nullptr) {
        SDL_SetAtomicInt(&r->aborted, 1);
    }
}

void embi_net_cookie_get(embi_net *n, const char *url, embi_sb *out)
{
    embi_url u = {0};
    if (n == nullptr || !embi_url_parse((embi_str){url, (uint32_t)strlen(url)}, nullptr, &u)) {
        return;
    }
    SDL_LockMutex(n->jar_m);
    embi_cookiejar_header(&n->jar, &u, false, now_wall_ms(), out);
    SDL_UnlockMutex(n->jar_m);
    embi_url_free(&u);
}

void embi_net_cookie_set(embi_net *n, const char *url, const char *cookie)
{
    embi_url u = {0};
    if (n == nullptr || !embi_url_parse((embi_str){url, (uint32_t)strlen(url)}, nullptr, &u)) {
        return;
    }
    SDL_LockMutex(n->jar_m);
    (void)embi_cookiejar_set(&n->jar, &u, (embi_str){cookie, (uint32_t)strlen(cookie)}, true, now_wall_ms());
    SDL_UnlockMutex(n->jar_m);
    embi_url_free(&u);
}
