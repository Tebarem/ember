#pragma once
/*
 * Helpers of the N1 network tests: a 127.0.0.1-only HTTP and TLS test server (SDL_net + Mbed TLS server side) and a
 * blocking client wrapper over embi_net_start. Everything binds and connects to 127.0.0.1 only. Header-only; every
 * function is static inline so a test executable that does not use one does not warn.
 */
#include "emb_test.h"

#include "net/net_int.h"

#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------------ server */

#define TS_MAX_HDRS 48

typedef struct ts_hdr {
    char name[64];
    char value[512];
} ts_hdr;

typedef struct ts_req {
    char method[16];
    char target[1024];
    ts_hdr h[TS_MAX_HDRS];
    int nh;
    uint8_t *body;
    size_t body_len;
} ts_req;

typedef struct ts_server ts_server;

typedef struct ts_conn {
    ts_server *srv;
    NET_StreamSocket *sock;
    mbedtls_ssl_context ssl;
    bool tls;
    bool close; /* the handler asks to close after this response */
    bool abrupt; /* ... and over TLS without close_notify */
    bool eof;
    embi_buf in;
} ts_conn;

typedef void (*ts_handler)(ts_conn *c, const ts_req *req, void *user);

struct ts_server {
    NET_Server *srv;
    NET_Address *addr;
    int port;
    SDL_Thread *thread;
    SDL_AtomicInt stop;
    SDL_AtomicInt live; /* connection threads running */
    SDL_AtomicInt accepted;
    SDL_AtomicInt requests;
    SDL_AtomicInt max_live;
    ts_handler handler;
    void *user;
    bool tls;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
};

static inline const char *ts_req_header(const ts_req *r, const char *name)
{
    for (int i = 0; i < r->nh; i++) {
        if (SDL_strcasecmp(r->h[i].name, name) == 0) {
            return r->h[i].value;
        }
    }
    return nullptr;
}

static inline int ts_bio_send(void *p, const unsigned char *buf, size_t len)
{
    ts_conn *c = p;
    return NET_WriteToStreamSocket(c->sock, buf, (int)len) ? (int)len : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static inline int ts_bio_recv(void *p, unsigned char *buf, size_t len)
{
    ts_conn *c = p;
    int n = NET_ReadFromStreamSocket(c->sock, buf, (int)len);
    if (n < 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : n;
}

/* Reads available bytes into c->in. Waits up to wait_ms. Returns false at end of stream or error. */
static inline bool ts_conn_pull(ts_conn *c, int wait_ms)
{
    uint8_t tmp[8192];
    for (int attempt = 0; attempt < 2; attempt++) {
        int n;
        if (c->tls) {
            n = mbedtls_ssl_read(&c->ssl, tmp, sizeof tmp);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                n = 0;
            } else if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                n = 0;
            } else if (n <= 0) {
                return false;
            }
        } else {
            n = NET_ReadFromStreamSocket(c->sock, tmp, sizeof tmp);
            if (n < 0) {
                return false;
            }
        }
        if (n > 0) {
            (void)embi_buf_append(&c->in, tmp, (size_t)n);
            return true;
        }
        if (attempt == 0) {
            void *s = c->sock;
            NET_WaitUntilInputAvailable(&s, 1, wait_ms);
        }
    }
    return true;
}

static inline bool ts_conn_write(ts_conn *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    if (!c->tls) {
        while (len > 0) {
            int chunk = len > 65536 ? 65536 : (int)len;
            if (!NET_WriteToStreamSocket(c->sock, p, chunk)) {
                return false;
            }
            p += chunk;
            len -= (size_t)chunk;
        }
        return true;
    }
    while (len > 0) {
        int n = mbedtls_ssl_write(&c->ssl, p, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            SDL_Delay(1);
            continue;
        }
        if (n < 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static inline void ts_printf(ts_conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void ts_printf(ts_conn *c, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        (void)ts_conn_write(c, buf, (size_t)n);
    }
}

/* A complete response: extra is zero or more "Name: value\r\n" lines. */
static inline void ts_respond(ts_conn *c, int status, const char *extra, const void *body, size_t len)
{
    ts_printf(c, "HTTP/1.1 %d Status\r\nContent-Length: %u\r\n%s\r\n", status, (unsigned)len, extra != nullptr ? extra : "");
    if (len > 0) {
        (void)ts_conn_write(c, body, len);
    }
}

static inline void ts_respond_str(ts_conn *c, int status, const char *extra, const char *body)
{
    ts_respond(c, status, extra, body, strlen(body));
}

/* Parses one request out of c->in. 1 = parsed, 0 = need more, -1 = bad. */
static inline int ts_parse(ts_conn *c, ts_req *r)
{
    const char *d = (const char *)c->in.data;
    size_t n = c->in.len, end = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n') {
            end = i + 4;
            break;
        }
    }
    if (end == 0) {
        return 0;
    }
    memset(r, 0, sizeof *r);
    size_t pos = 0;
    size_t le = pos;
    while (le + 1 < end && !(d[le] == '\r' && d[le + 1] == '\n')) {
        le++;
    }
    if (sscanf(d, "%15s %1023s", r->method, r->target) != 2) {
        return -1;
    }
    pos = le + 2;
    while (pos + 2 < end && r->nh < TS_MAX_HDRS) {
        le = pos;
        while (le + 1 < end && !(d[le] == '\r' && d[le + 1] == '\n')) {
            le++;
        }
        const char *colon = memchr(d + pos, ':', le - pos);
        if (colon == nullptr) {
            return -1;
        }
        ts_hdr *h = &r->h[r->nh++];
        size_t nl = (size_t)(colon - (d + pos));
        SDL_snprintf(h->name, sizeof h->name, "%.*s", (int)nl, d + pos);
        const char *v = colon + 1;
        while (v < d + le && (*v == ' ' || *v == '\t')) {
            v++;
        }
        SDL_snprintf(h->value, sizeof h->value, "%.*s", (int)(d + le - v), v);
        pos = le + 2;
    }
    /* The body: Content-Length or chunked. */
    size_t blen = 0;
    const char *cl = ts_req_header(r, "content-length");
    const char *te = ts_req_header(r, "transfer-encoding");
    if (cl != nullptr) {
        blen = (size_t)SDL_strtoull(cl, nullptr, 10);
        if (n < end + blen) {
            return 0;
        }
        r->body = SDL_malloc(blen + 1);
        memcpy(r->body, d + end, blen);
        r->body[blen] = 0;
        r->body_len = blen;
        end += blen;
    } else if (te != nullptr && SDL_strcasestr(te, "chunked") != nullptr) {
        embi_http_body b;
        embi_http_body_init(&b, EMBI_BODY_CHUNKED, 0);
        embi_buf out = {0};
        size_t used = embi_http_body_feed(&b, (const uint8_t *)d + end, n - end, &out);
        if (!b.done) {
            embi_buf_free(&out);
            return 0;
        }
        r->body = SDL_malloc(out.len + 1);
        memcpy(r->body, out.data, out.len);
        r->body[out.len] = 0;
        r->body_len = out.len;
        embi_buf_free(&out);
        end += used;
    }
    memmove(c->in.data, c->in.data + end, c->in.len - end);
    c->in.len -= end;
    return 1;
}

static inline int ts_serve_conn(void *arg)
{
    ts_conn *c = arg;
    ts_server *s = c->srv;
    int live = SDL_AddAtomicInt(&s->live, 1) + 1;
    int mx;
    while (live > (mx = SDL_GetAtomicInt(&s->max_live))) {
        if (SDL_CompareAndSwapAtomicInt(&s->max_live, mx, live)) {
            break;
        }
    }
    bool ok = true;
    if (c->tls) {
        mbedtls_ssl_init(&c->ssl);
        ok = mbedtls_ssl_setup(&c->ssl, &s->conf) == 0;
        if (ok) {
            mbedtls_ssl_set_bio(&c->ssl, c, ts_bio_send, ts_bio_recv, nullptr);
            uint64_t t0 = SDL_GetTicks();
            for (;;) {
                int rc = mbedtls_ssl_handshake(&c->ssl);
                if (rc == 0) {
                    break;
                }
                if ((rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) || SDL_GetAtomicInt(&s->stop) ||
                    SDL_GetTicks() - t0 > 5000) {
                    ok = false; /* a client that rejects the certificate ends here */
                    break;
                }
                void *sk = c->sock;
                NET_WaitUntilInputAvailable(&sk, 1, 20);
            }
        }
    }
    uint64_t idle = SDL_GetTicks();
    while (ok && !SDL_GetAtomicInt(&s->stop) && !c->close) {
        ts_req r;
        int pr = ts_parse(c, &r);
        if (pr < 0) {
            break;
        }
        if (pr == 1) {
            SDL_AddAtomicInt(&s->requests, 1);
            s->handler(c, &r, s->user);
            SDL_free(r.body);
            idle = SDL_GetTicks();
            continue;
        }
        if (!ts_conn_pull(c, 20)) {
            break;
        }
        if (SDL_GetTicks() - idle > 15000) {
            break;
        }
    }
    if (c->tls && ok && !c->abrupt) {
        (void)mbedtls_ssl_close_notify(&c->ssl);
    }
    NET_WaitUntilStreamSocketDrained(c->sock, 1000);
    if (c->tls) {
        mbedtls_ssl_free(&c->ssl);
    }
    NET_DestroyStreamSocket(c->sock);
    embi_buf_free(&c->in);
    SDL_free(c);
    SDL_AddAtomicInt(&s->live, -1);
    return 0;
}

static inline int ts_accept_loop(void *arg)
{
    ts_server *s = arg;
    while (!SDL_GetAtomicInt(&s->stop)) {
        void *sv = s->srv;
        NET_WaitUntilInputAvailable(&sv, 1, 20);
        NET_StreamSocket *cs = nullptr;
        while (NET_AcceptClient(s->srv, &cs) && cs != nullptr) {
            ts_conn *c = SDL_calloc(1, sizeof *c);
            c->srv = s;
            c->sock = cs;
            c->tls = s->tls;
            SDL_AddAtomicInt(&s->accepted, 1);
            SDL_Thread *t = SDL_CreateThread(ts_serve_conn, "ts-conn", c);
            if (t != nullptr) {
                SDL_DetachThread(t);
            }
            cs = nullptr;
        }
    }
    return 0;
}

static inline char *ts_read_file(const char *name, size_t *len)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/tests/data/net/%s", EMBER_SOURCE_DIR, name);
    size_t n = 0;
    void *d = SDL_LoadFile(path, &n);
    if (d == nullptr) {
        return nullptr;
    }
    char *copy = SDL_malloc(n + 1);
    memcpy(copy, d, n);
    copy[n] = 0;
    SDL_free(d);
    *len = n;
    return copy;
}

/* Starts a server on a free 127.0.0.1 port. crt and key name files in tests/data/net for a TLS server, or nullptr. */
static inline ts_server *ts_start(ts_handler h, void *user, const char *crt, const char *key)
{
    NET_Init();
    ts_server *s = SDL_calloc(1, sizeof *s);
    s->handler = h;
    s->user = user;
    s->addr = NET_ResolveHostname("127.0.0.1");
    if (s->addr == nullptr || NET_WaitUntilResolved(s->addr, 5000) != NET_SUCCESS) {
        return nullptr;
    }
    if (crt != nullptr) {
        embi_net_tls_global_init();
        mbedtls_ssl_config_init(&s->conf);
        mbedtls_x509_crt_init(&s->crt);
        mbedtls_pk_init(&s->pk);
        size_t cl = 0, kl = 0;
        char *cd = ts_read_file(crt, &cl);
        char *kd = ts_read_file(key, &kl);
        bool ok = cd != nullptr && kd != nullptr && mbedtls_x509_crt_parse(&s->crt, (unsigned char *)cd, cl + 1) == 0 &&
                  mbedtls_pk_parse_key(&s->pk, (unsigned char *)kd, kl + 1, nullptr, 0) == 0 &&
                  mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
                  mbedtls_ssl_conf_own_cert(&s->conf, &s->crt, &s->pk) == 0;
        SDL_free(cd);
        SDL_free(kd);
        if (!ok) {
            return nullptr;
        }
        s->tls = true;
    }
    int base = 20000 + (int)(SDL_GetPerformanceCounter() % 30000);
    for (int i = 0; i < 400 && s->srv == nullptr; i++) {
        s->port = 20000 + (base - 20000 + i * 7) % 40000;
        s->srv = NET_CreateServer(s->addr, (Uint16)s->port, 0);
    }
    if (s->srv == nullptr) {
        return nullptr;
    }
    s->thread = SDL_CreateThread(ts_accept_loop, "ts-accept", s);
    return s;
}

static inline void ts_stop(ts_server *s)
{
    if (s == nullptr) {
        return;
    }
    SDL_SetAtomicInt(&s->stop, 1);
    SDL_WaitThread(s->thread, nullptr);
    for (int i = 0; i < 600 && SDL_GetAtomicInt(&s->live) > 0; i++) {
        SDL_Delay(10);
    }
    NET_DestroyServer(s->srv);
    NET_UnrefAddress(s->addr);
    if (s->tls) {
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_x509_crt_free(&s->crt);
        mbedtls_pk_free(&s->pk);
    }
    SDL_free(s);
    NET_Quit();
}

static inline void ts_url(const ts_server *s, char *out, size_t cap, const char *path)
{
    SDL_snprintf(out, cap, "%s://127.0.0.1:%d%s", s->tls ? "https" : "http", s->port, path);
}

/* ------------------------------------------------------------------ client */

typedef struct tc {
    SDL_Mutex *m;
    SDL_Condition *cv;
    bool released, ended, errored, got_head;
    int status;
    char status_text[64];
    char url[600];
    embi_headers headers;
    embi_buf body;
    embi_net_err err;
    char msg[400];
    bool redirected, tls;
    uint32_t redirects;
    int64_t content_length;
    int data_events;
    int order; /* 1 head seen, 2 data seen, 3 end/error seen, 4 released: checked in the callbacks */
    bool order_ok;
    bool abort_after_data; /* test hook: abort from the first data callback */
    bool drop_body;        /* count the body bytes (body_total) without keeping them */
    uint64_t body_total;
    embi_net_req *req;
    bool started; /* tc_request built a request for it: tc_free waits for its release */
} tc;

static inline void tc_head(void *u, const embi_net_head *h)
{
    tc *t = u;
    SDL_LockMutex(t->m);
    if (t->order != 0) {
        t->order_ok = false;
    }
    t->order = 1;
    t->got_head = true;
    t->status = h->status;
    SDL_snprintf(t->status_text, sizeof t->status_text, "%s", h->status_text);
    SDL_snprintf(t->url, sizeof t->url, "%s", h->url);
    for (uint32_t i = 0; i < h->headers->list.len; i++) {
        const embi_kv *kv = &h->headers->list.data[i];
        embi_headers_add_raw(&t->headers, (embi_str){kv->name.data, kv->name.len}, (embi_str){kv->value.data, kv->value.len});
    }
    t->redirected = h->redirected;
    t->redirects = h->redirect_count;
    t->tls = h->tls;
    t->content_length = h->content_length;
    SDL_UnlockMutex(t->m);
}

static inline void tc_data(void *u, const uint8_t *d, size_t n)
{
    tc *t = u;
    SDL_LockMutex(t->m);
    if (t->order < 1 || t->order > 2) {
        t->order_ok = false;
    }
    t->order = 2;
    if (!t->drop_body) {
        (void)embi_buf_append(&t->body, d, n);
    }
    t->body_total += n;
    t->data_events++;
    SDL_UnlockMutex(t->m);
    if (t->abort_after_data && t->req != nullptr) {
        embi_net_abort(t->req);
    }
}

static inline void tc_end(void *u)
{
    tc *t = u;
    SDL_LockMutex(t->m);
    if (t->order < 1 || t->order > 2) {
        t->order_ok = false;
    }
    t->order = 3;
    t->ended = true;
    SDL_BroadcastCondition(t->cv);
    SDL_UnlockMutex(t->m);
}

static inline void tc_error(void *u, embi_net_err err, const char *msg)
{
    tc *t = u;
    SDL_LockMutex(t->m);
    if (t->order > 2) {
        t->order_ok = false;
    }
    t->order = 3;
    t->errored = true;
    t->err = err;
    SDL_snprintf(t->msg, sizeof t->msg, "%s", msg);
    SDL_BroadcastCondition(t->cv);
    SDL_UnlockMutex(t->m);
}

static inline void tc_release(void *u)
{
    tc *t = u;
    SDL_LockMutex(t->m);
    t->order = 4;
    t->released = true;
    SDL_BroadcastCondition(t->cv);
    SDL_UnlockMutex(t->m);
}

static inline void tc_init(tc *t)
{
    memset(t, 0, sizeof *t);
    t->m = SDL_CreateMutex();
    t->cv = SDL_CreateCondition();
    t->order_ok = true;
}

static inline void tc_free(tc *t)
{
    /* The release callback runs after end or error, on the network thread: wait for it before the mutex goes, or it
     * locks a destroyed mutex (seen once in 50 runs of http_pool_limit_is_six_per_origin under ASan). */
    if (t->started) {
        uint64_t end = SDL_GetTicks() + 15000;
        SDL_LockMutex(t->m);
        while (!t->released && SDL_GetTicks() < end) {
            SDL_WaitConditionTimeout(t->cv, t->m, 50);
        }
        SDL_UnlockMutex(t->m);
    }
    embi_headers_free(&t->headers);
    embi_buf_free(&t->body);
    SDL_DestroyCondition(t->cv);
    SDL_DestroyMutex(t->m);
}

static inline embi_net_request tc_request(tc *t, const char *method, const char *url)
{
    embi_net_request r = {0};
    r.method = method;
    r.url = url;
    r.redirect = EMBI_NET_REDIRECT_FOLLOW;
    r.creds = EMBI_NET_CREDS_INCLUDE;
    r.cb = (embi_net_callbacks){tc_head, tc_data, tc_end, tc_error, tc_release};
    r.user = t;
    t->started = true;
    return r;
}

/* Waits until the request ended or failed (not necessarily released). */
static inline bool tc_wait_done(tc *t, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    SDL_LockMutex(t->m);
    while (!t->ended && !t->errored && !t->released) {
        uint64_t now = SDL_GetTicks();
        if (now >= end) {
            break;
        }
        SDL_WaitConditionTimeout(t->cv, t->m, (int)(end - now));
    }
    bool ok = t->ended || t->errored;
    SDL_UnlockMutex(t->m);
    return ok;
}

static inline bool tc_wait_released(tc *t, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    SDL_LockMutex(t->m);
    while (!t->released) {
        uint64_t now = SDL_GetTicks();
        if (now >= end) {
            break;
        }
        SDL_WaitConditionTimeout(t->cv, t->m, (int)(end - now));
    }
    bool ok = t->released;
    SDL_UnlockMutex(t->m);
    return ok;
}

/* Runs a request to completion; the handle is dropped. */
static inline bool tc_run(embi_net *n, tc *t, embi_net_request *r, uint32_t ms)
{
    t->req = embi_net_start(n, r);
    bool ok = tc_wait_done(t, ms) && tc_wait_released(t, 2000);
    embi_net_req_unref(t->req);
    t->req = nullptr;
    return ok;
}

static inline bool tc_hdr_is(const tc *t, const char *name, const char *value)
{
    embi_str v = embi_headers_first(&t->headers, (embi_str){name, (uint32_t)strlen(name)});
    return v.ptr != nullptr && v.len == strlen(value) && memcmp(v.ptr, value, v.len) == 0;
}

static inline bool tc_body_is(const tc *t, const char *s)
{
    return t->body.len == strlen(s) && (t->body.len == 0 || memcmp(t->body.data, s, t->body.len) == 0);
}

/* A service with a log capture: warnings are counted and the last one kept. */
typedef struct tlog {
    SDL_AtomicInt warns;
    char last[512];
    SDL_Mutex *m;
} tlog;

static inline void tlog_fn(void *u, emb_log_level level, const char *msg)
{
    tlog *l = u;
    if (level == EMB_LOG_WARN) {
        SDL_LockMutex(l->m);
        SDL_snprintf(l->last, sizeof l->last, "%s", msg);
        SDL_UnlockMutex(l->m);
        SDL_AddAtomicInt(&l->warns, 1);
    }
}

static inline embi_net *tn_new(tlog *l)
{
    l->m = SDL_CreateMutex();
    embi_net *n = embi_net_create(tlog_fn, l);
    embi_net_allow_add(n, "127.0.0.1");
    embi_net_allow_add(n, "localhost");
    return n;
}

static inline void tn_free(embi_net *n, tlog *l)
{
    embi_net_destroy(n);
    SDL_DestroyMutex(l->m);
}

static inline void tn_trust_test_ca(embi_net *n)
{
    size_t len = 0;
    char *ca = ts_read_file("ca.crt", &len);
    EMB_CHECK(embi_net_add_root(n, ca, len)); /* also as the very first call of the process */
    SDL_free(ca);
}
