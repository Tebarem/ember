#pragma once
/*
 * Helpers of the N1c tests: a WebSocket server on top of the 127.0.0.1 test server of net_test.h (plain and TLS), whose
 * behavior is chosen by the request path, a task queue that stands for the receiving ("JS") thread, and a collector
 * of WebSocket events. Header-only; everything is static inline.
 */
#include "net_test.h"

#include "net/ws.h"
#include "net/ws_int.h"

/* ------------------------------------------------------------------ the receiving thread */

typedef struct tq_task {
    struct tq_task *next;
    void (*fn)(void *);
    void (*drop)(void *);
    void *arg;
} tq_task;

typedef struct tq {
    SDL_Mutex *m;
    SDL_Condition *cv;
    tq_task *head, *tail;
    bool dead;
} tq;

static inline tq *tq_new(void)
{
    tq *q = SDL_calloc(1, sizeof *q);
    q->m = SDL_CreateMutex();
    q->cv = SDL_CreateCondition();
    return q;
}

static inline bool tq_post(void *u, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    tq *q = u;
    SDL_LockMutex(q->m);
    if (q->dead) {
        SDL_UnlockMutex(q->m);
        return false;
    }
    tq_task *t = SDL_calloc(1, sizeof *t);
    t->fn = fn;
    t->drop = drop;
    t->arg = arg;
    if (q->tail != nullptr) {
        q->tail->next = t;
    } else {
        q->head = t;
    }
    q->tail = t;
    SDL_SignalCondition(q->cv);
    SDL_UnlockMutex(q->m);
    return true;
}

/* Runs one task, waiting up to ms for it. False when there was none. */
static inline bool tq_step(tq *q, int ms)
{
    SDL_LockMutex(q->m);
    if (q->head == nullptr && ms > 0) {
        SDL_WaitConditionTimeout(q->cv, q->m, ms);
    }
    tq_task *t = q->head;
    if (t != nullptr) {
        q->head = t->next;
        if (q->head == nullptr) {
            q->tail = nullptr;
        }
    }
    SDL_UnlockMutex(q->m);
    if (t == nullptr) {
        return false;
    }
    t->fn(t->arg);
    SDL_free(t);
    return true;
}

/* How many tasks wait in the queue. */
static inline int tq_len(tq *q)
{
    int n = 0;
    SDL_LockMutex(q->m);
    for (tq_task *t = q->head; t != nullptr; t = t->next) {
        n++;
    }
    SDL_UnlockMutex(q->m);
    return n;
}

/* Runs tasks until pred is true or ms passed. */
static inline bool tq_wait(tq *q, bool (*pred)(void *), void *arg, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    for (;;) {
        if (pred(arg)) {
            return true;
        }
        uint64_t now = SDL_GetTicks();
        if (now >= end) {
            return false;
        }
        (void)tq_step(q, (int)(end - now > 20 ? 20 : end - now));
    }
}

/* Runs tasks for ms (to see that nothing else arrives). */
static inline void tq_run_for(tq *q, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        (void)tq_step(q, 10);
    }
}

static inline void tq_free(tq *q)
{
    SDL_LockMutex(q->m);
    q->dead = true;
    tq_task *t = q->head;
    q->head = q->tail = nullptr;
    SDL_UnlockMutex(q->m);
    while (t != nullptr) {
        tq_task *n = t->next;
        t->drop(t->arg);
        SDL_free(t);
        t = n;
    }
    SDL_DestroyCondition(q->cv);
    SDL_DestroyMutex(q->m);
    SDL_free(q);
}

/* ------------------------------------------------------------------ the event collector */

enum { WC_OPEN = 1, WC_MESSAGE, WC_ERROR, WC_CLOSE, WC_RELEASE };

typedef struct wc_ev {
    int kind;
    bool binary, clean;
    uint16_t code;
    char *data;
    size_t len;
} wc_ev;

#define WC_MAX 512

typedef struct wc {
    tq *q;
    wc_ev ev[WC_MAX];
    int n;
    SDL_AtomicInt released;
    embi_net_req *req;
    int msgs, nonempty_msgs; /* every message event, also those past the stored ones */
    bool order_bad; /* an event after release, or after close */
    bool closed;
} wc;

static inline void wc_add(wc *w, int kind, const void *data, size_t len, bool binary, bool clean, uint16_t code)
{
    if (w->closed && kind != WC_RELEASE) {
        w->order_bad = true;
    }
    if (SDL_GetAtomicInt(&w->released) != 0) {
        w->order_bad = true;
    }
    if (kind == WC_MESSAGE) {
        w->msgs++;
        w->nonempty_msgs += len > 0;
        if (w->msgs > 400) {
            return; /* counted, not stored: room is kept for the close and release events */
        }
    }
    if (w->n >= WC_MAX) {
        return;
    }
    wc_ev *e = &w->ev[w->n++];
    e->kind = kind;
    e->binary = binary;
    e->clean = clean;
    e->code = code;
    e->len = len;
    e->data = SDL_malloc(len + 1);
    if (len > 0) {
        memcpy(e->data, data, len);
    }
    e->data[len] = 0;
    if (kind == WC_CLOSE) {
        w->closed = true;
    }
}

static inline void wc_on_open(void *u, const char *protocol)
{
    wc_add(u, WC_OPEN, protocol, strlen(protocol), false, false, 0);
}

static inline void wc_on_message(void *u, bool binary, const uint8_t *d, size_t n)
{
    wc_add(u, WC_MESSAGE, d, n, binary, false, 0);
}

static inline void wc_on_error(void *u, const char *msg)
{
    wc_add(u, WC_ERROR, msg, strlen(msg), false, false, 0);
}

static inline void wc_on_close(void *u, bool clean, uint16_t code, const char *reason)
{
    wc_add(u, WC_CLOSE, reason, strlen(reason), false, clean, code);
}

static inline void wc_on_release(void *u)
{
    wc *w = u;
    if (w->n < WC_MAX) {
        wc_ev *e = &w->ev[w->n++];
        e->kind = WC_RELEASE;
        e->data = SDL_calloc(1, 1);
    }
    SDL_SetAtomicInt(&w->released, 1);
}

static inline void wc_init(wc *w, tq *q)
{
    memset(w, 0, sizeof *w);
    w->q = q;
}

static inline void wc_free(wc *w)
{
    for (int i = 0; i < w->n; i++) {
        SDL_free(w->ev[i].data);
    }
    embi_net_req_unref(w->req);
    w->req = nullptr;
}

static inline embi_ws_request wc_request(wc *w, const char *url)
{
    embi_ws_request r = {0};
    r.url = url;
    r.cb = (embi_ws_callbacks){wc_on_open, wc_on_message, wc_on_error, wc_on_close, wc_on_release};
    r.user = w;
    r.post = tq_post;
    r.post_user = w->q;
    return r;
}

static inline int wc_count(const wc *w, int kind)
{
    int c = 0;
    for (int i = 0; i < w->n; i++) {
        c += w->ev[i].kind == kind;
    }
    return c;
}

static inline const wc_ev *wc_nth(const wc *w, int kind, int nth)
{
    for (int i = 0; i < w->n; i++) {
        if (w->ev[i].kind == kind && nth-- == 0) {
            return &w->ev[i];
        }
    }
    return nullptr;
}

typedef struct wc_pred {
    wc *w;
    int kind;
    int count;
} wc_pred;

static inline bool wc_pred_fn(void *a)
{
    wc_pred *p = a;
    return wc_count(p->w, p->kind) >= p->count;
}

/* Runs the queue until count events of kind arrived. */
static inline bool wc_wait(wc *w, int kind, int count, uint32_t ms)
{
    wc_pred p = {w, kind, count};
    return tq_wait(w->q, wc_pred_fn, &p, ms);
}

/* ------------------------------------------------------------------ the server */

typedef struct wsx {
    SDL_AtomicInt unmasked;     /* client frames without a mask */
    SDL_AtomicInt close_code;   /* the last close code a client sent (0: none yet, 1005: a close with no code) */
    SDL_AtomicInt disconnects;  /* connections that ended without a close frame */
    SDL_AtomicInt handshakes;
    SDL_AtomicInt sink_msgs;
    SDL_AtomicInt sink_bytes;
    SDL_AtomicInt pong_ok;
    SDL_AtomicInt pongs; /* pong frames received by /pingflood */
    SDL_Mutex *m;
    char close_reason[200];
    char req_cookie[256], req_proto[128], req_origin[128], req_key[64], req_version[16], req_extensions[64];
} wsx;

static inline void wsx_init(wsx *x)
{
    memset(x, 0, sizeof *x);
    x->m = SDL_CreateMutex();
}

static inline void wsx_free(wsx *x)
{
    SDL_DestroyMutex(x->m);
}

/* Sends a server frame (never masked). */
static inline bool wsx_send_ex(ts_conn *c, uint8_t first, const void *data, size_t len, bool mask)
{
    uint8_t hdr[14];
    size_t hl = 0;
    hdr[0] = first;
    uint8_t mb = mask ? 0x80 : 0;
    if (len < 126) {
        hdr[1] = (uint8_t)(mb | len);
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = (uint8_t)(mb | 126);
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hl = 4;
    } else {
        hdr[1] = (uint8_t)(mb | 127);
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint8_t)((uint64_t)len >> (8 * (7 - i)));
        }
        hl = 10;
    }
    if (mask) {
        hdr[hl++] = 1;
        hdr[hl++] = 2;
        hdr[hl++] = 3;
        hdr[hl++] = 4;
    }
    embi_buf b = {0};
    (void)embi_buf_append(&b, hdr, hl);
    for (size_t i = 0; i < len; i++) {
        uint8_t v = ((const uint8_t *)data)[i];
        if (mask) {
            v ^= (uint8_t)(i & 3) + 1;
        }
        (void)embi_buf_append(&b, &v, 1);
    }
    bool ok = ts_conn_write(c, b.data, b.len);
    embi_buf_free(&b);
    return ok;
}

static inline bool wsx_send(ts_conn *c, uint8_t op, bool fin, const void *data, size_t len)
{
    return wsx_send_ex(c, (uint8_t)((fin ? 0x80 : 0) | op), data, len, false);
}

static inline bool wsx_send_close(ts_conn *c, int code, const char *reason)
{
    uint8_t p[125];
    size_t n = 0;
    if (code >= 0) {
        p[0] = (uint8_t)(code >> 8);
        p[1] = (uint8_t)code;
        n = 2;
        if (reason != nullptr) {
            n += (size_t)SDL_snprintf((char *)p + 2, 120, "%s", reason);
        }
    }
    return wsx_send(c, 8, true, p, n);
}

typedef struct wsx_frame {
    uint8_t op;
    bool fin;
    uint8_t *data;
    size_t len;
} wsx_frame;

/* Reads one client frame (blocking up to ms). False on EOF, timeout, or server stop. */
static inline bool wsx_read(ts_conn *c, wsx *x, wsx_frame *f, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    for (;;) {
        if (c->in.len >= 2) {
            const uint8_t *p = c->in.data;
            size_t hl = 2;
            uint64_t len = p[1] & 0x7F;
            bool masked = (p[1] & 0x80) != 0;
            bool have = true;
            if (len == 126) {
                if (c->in.len < 4) {
                    have = false;
                } else {
                    len = (uint64_t)p[2] << 8 | p[3];
                    hl = 4;
                }
            } else if (len == 127) {
                if (c->in.len < 10) {
                    have = false;
                } else {
                    len = 0;
                    for (int i = 0; i < 8; i++) {
                        len = len << 8 | p[2 + i];
                    }
                    hl = 10;
                }
            }
            if (have) {
                size_t need = hl + (masked ? 4 : 0) + (size_t)len;
                if (c->in.len >= need) {
                    f->op = p[0] & 0x0F;
                    f->fin = (p[0] & 0x80) != 0;
                    f->len = (size_t)len;
                    f->data = SDL_malloc(f->len + 1);
                    const uint8_t *m = p + hl;
                    const uint8_t *src = p + hl + (masked ? 4 : 0);
                    for (size_t i = 0; i < f->len; i++) {
                        f->data[i] = masked ? (uint8_t)(src[i] ^ m[i & 3]) : src[i];
                    }
                    f->data[f->len] = 0;
                    if (!masked) {
                        SDL_AddAtomicInt(&x->unmasked, 1);
                    }
                    memmove(c->in.data, c->in.data + need, c->in.len - need);
                    c->in.len -= need;
                    return true;
                }
            }
        }
        if (SDL_GetTicks() >= end || SDL_GetAtomicInt(&c->srv->stop)) {
            return false;
        }
        if (!ts_conn_pull(c, 20)) {
            return false;
        }
    }
}

static inline void wsx_note_close(wsx *x, const wsx_frame *f)
{
    SDL_LockMutex(x->m);
    int code = f->len >= 2 ? f->data[0] << 8 | f->data[1] : 1005;
    SDL_snprintf(x->close_reason, sizeof x->close_reason, "%.*s", f->len > 2 ? (int)(f->len - 2) : 0, (const char *)f->data + 2);
    SDL_UnlockMutex(x->m);
    SDL_SetAtomicInt(&x->close_code, code);
}

/* The echo loop: text and binary frames come back as they are, pings are answered, a close is echoed. */
static inline void wsx_echo(ts_conn *c, wsx *x)
{
    wsx_frame f;
    uint8_t *acc = nullptr;
    size_t acc_len = 0;
    uint8_t acc_op = 0;
    while (wsx_read(c, x, &f, 20000)) {
        if (f.op == 8) {
            wsx_note_close(x, &f);
            wsx_send(c, 8, true, f.data, f.len);
            SDL_free(f.data);
            SDL_free(acc);
            return;
        }
        if (f.op == 9) {
            wsx_send(c, 10, true, f.data, f.len);
        } else if (f.op == 1 || f.op == 2 || f.op == 0) {
            if (f.op != 0) {
                acc_op = f.op;
            }
            acc = SDL_realloc(acc, acc_len + f.len + 1);
            memcpy(acc + acc_len, f.data, f.len);
            acc_len += f.len;
            if (f.fin) {
                wsx_send(c, acc_op, true, acc, acc_len);
                SDL_free(acc);
                acc = nullptr;
                acc_len = 0;
            }
        }
        SDL_free(f.data);
    }
    SDL_free(acc);
    SDL_AddAtomicInt(&x->disconnects, 1);
}

/* Reads until the client's close frame (or the end of the connection); records its code. */
static inline void wsx_wait_close(ts_conn *c, wsx *x, bool echo)
{
    wsx_frame f;
    while (wsx_read(c, x, &f, 5000)) {
        if (f.op == 8) {
            wsx_note_close(x, &f);
            if (echo) {
                wsx_send(c, 8, true, f.data, f.len);
            }
            SDL_free(f.data);
            return;
        }
        SDL_free(f.data);
    }
    SDL_AddAtomicInt(&x->disconnects, 1);
}

static inline bool path_is(const char *target, const char *p)
{
    size_t n = strlen(p);
    return strncmp(target, p, n) == 0 && (target[n] == 0 || target[n] == '?' || target[n] == '/');
}

/* The handler of the test server (user is a wsx). The paths: see the tests. */
static inline void wsx_handler(ts_conn *c, const ts_req *r, void *user)
{
    wsx *x = user;
    const char *t = r->target;
    c->close = true;
    const char *key = ts_req_header(r, "sec-websocket-key");
    if (path_is(t, "/plain") || key == nullptr) {
        ts_respond_str(c, 200, "", "not a websocket");
        return;
    }
    SDL_AddAtomicInt(&x->handshakes, 1);
    SDL_LockMutex(x->m);
    const char *v;
    SDL_snprintf(x->req_cookie, sizeof x->req_cookie, "%s", (v = ts_req_header(r, "cookie")) != nullptr ? v : "(none)");
    SDL_snprintf(x->req_proto, sizeof x->req_proto, "%s", (v = ts_req_header(r, "sec-websocket-protocol")) != nullptr ? v : "(none)");
    SDL_snprintf(x->req_origin, sizeof x->req_origin, "%s", (v = ts_req_header(r, "origin")) != nullptr ? v : "(none)");
    SDL_snprintf(x->req_version, sizeof x->req_version, "%s", (v = ts_req_header(r, "sec-websocket-version")) != nullptr ? v : "(none)");
    SDL_snprintf(x->req_extensions, sizeof x->req_extensions, "%s", (v = ts_req_header(r, "sec-websocket-extensions")) != nullptr ? v : "(none)");
    SDL_snprintf(x->req_key, sizeof x->req_key, "%s", key);
    SDL_UnlockMutex(x->m);
    if (path_is(t, "/status403")) {
        ts_respond_str(c, 403, "", "no");
        return;
    }
    if (path_is(t, "/redirect")) {
        ts_respond_str(c, 302, "Location: /echo\r\n", "");
        return;
    }
    char accept[32];
    embi_ws_accept_key(key, accept);
    if (path_is(t, "/badaccept")) {
        SDL_snprintf(accept, sizeof accept, "AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    }
    char extra[256] = "";
    if (path_is(t, "/proto")) {
        const char *p = ts_req_header(r, "sec-websocket-protocol");
        if (p != nullptr && strstr(p, "chat") != nullptr) {
            SDL_snprintf(extra, sizeof extra, "Sec-WebSocket-Protocol: chat\r\n");
        }
    } else if (path_is(t, "/badproto")) {
        SDL_snprintf(extra, sizeof extra, "Sec-WebSocket-Protocol: nope\r\n");
    } else if (path_is(t, "/twoprotos")) {
        SDL_snprintf(extra, sizeof extra, "Sec-WebSocket-Protocol: chat, other\r\n");
    } else if (path_is(t, "/ext")) {
        SDL_snprintf(extra, sizeof extra, "Sec-WebSocket-Extensions: permessage-deflate\r\n");
    } else if (path_is(t, "/setcookie")) {
        SDL_snprintf(extra, sizeof extra, "Set-Cookie: ws=1; Path=/\r\n");
    }
    const char *upgrade = path_is(t, "/noupgradehdr") ? "" : "Upgrade: websocket\r\n";
    const char *conn = path_is(t, "/noconnhdr") ? "" : "Connection: Upgrade\r\n";
    ts_printf(c, "HTTP/1.1 101 Switching Protocols\r\n%s%sSec-WebSocket-Accept: %s\r\n%s\r\n", upgrade, conn, accept, extra);

    if (path_is(t, "/hello")) {
        wsx_send(c, 1, true, "hello", 5);
        wsx_echo(c, x);
    } else if (path_is(t, "/fragment")) {
        wsx_send(c, 1, false, "Hel", 3);
        wsx_send(c, 9, true, "mid", 3);
        wsx_send(c, 0, false, "lo ", 3);
        wsx_send(c, 0, true, "World", 5);
        wsx_send(c, 2, false, "\x01\x02", 2);
        wsx_send(c, 0, false, "", 0);
        wsx_send(c, 0, true, "\x03", 1);
        wsx_wait_close(c, x, true);
    } else if (path_is(t, "/utf8split")) { /* U+20AC split over three frames, and U+1F600 over two */
        wsx_send(c, 1, false, "\xE2", 1);
        wsx_send(c, 0, false, "\x82", 1);
        wsx_send(c, 0, true, "\xAC", 1);
        wsx_send(c, 1, false, "\xF0\x9F", 2);
        wsx_send(c, 0, true, "\x98\x80", 2);
        wsx_wait_close(c, x, true);
    } else if (path_is(t, "/ping")) {
        wsx_send(c, 9, true, "hi!", 3);
        wsx_frame f;
        while (wsx_read(c, x, &f, 5000)) {
            if (f.op == 10) {
                if (f.len == 3 && memcmp(f.data, "hi!", 3) == 0) {
                    SDL_AddAtomicInt(&x->pong_ok, 1);
                }
                SDL_free(f.data);
                break;
            }
            SDL_free(f.data);
        }
        wsx_send(c, 10, true, "unsolicited", 11); /* a pong from the server is ignored by the client */
        wsx_send(c, 1, true, "after-ping", 10);
        wsx_wait_close(c, x, true);
    } else if (path_is(t, "/tiny")) { /* /tiny/N: N empty text messages in one write, then "end" */
        size_t n = (size_t)atoi(t + 6);
        uint8_t *b = SDL_malloc(n * 2 + 5);
        for (size_t i = 0; i < n; i++) {
            b[2 * i] = 0x81;
            b[2 * i + 1] = 0;
        }
        static const uint8_t end_frame[5] = {0x81, 3, 'e', 'n', 'd'};
        memcpy(b + n * 2, end_frame, 5);
        ts_conn_write(c, b, n * 2 + 5);
        SDL_free(b);
        wsx_wait_close(c, x, true);
    } else if (path_is(t, "/pingflood")) { /* /pingflood/N: N pings in one write, then "end"; counts the pongs */
        size_t n = (size_t)atoi(t + 11);
        uint8_t *b = SDL_malloc(n * 3 + 5);
        for (size_t i = 0; i < n; i++) {
            b[3 * i] = 0x89;
            b[3 * i + 1] = 1;
            b[3 * i + 2] = 'p';
        }
        static const uint8_t end_frame[5] = {0x81, 3, 'e', 'n', 'd'};
        memcpy(b + n * 3, end_frame, 5);
        ts_conn_write(c, b, n * 3 + 5);
        SDL_free(b);
        wsx_frame f;
        while (wsx_read(c, x, &f, 5000)) {
            if (f.op == 10) {
                SDL_AddAtomicInt(&x->pongs, 1);
            } else if (f.op == 8) {
                wsx_note_close(x, &f);
                wsx_send(c, 8, true, f.data, f.len);
                SDL_free(f.data);
                return;
            }
            SDL_free(f.data);
        }
    } else if (path_is(t, "/close")) { /* /close/CODE: the server closes first */
        int code = atoi(t + 7);
        wsx_send_close(c, code, "bye");
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/closenocode")) {
        wsx_send_close(c, -1, nullptr);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/closeraw")) { /* /closeraw/CODE sent as the close code even when it is not allowed */
        wsx_send_close(c, atoi(t + 10), "x");
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/close1byte")) {
        wsx_send(c, 8, true, "\x03", 1);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/closebadreason")) {
        wsx_send(c, 8, true, "\x03\xE8\xC3\x28", 4);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/badutf8")) {
        wsx_send(c, 1, true, "\xC3\x28", 2);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/badutf8frag")) { /* invalid in the first frame already: no need to wait for the end */
        wsx_send(c, 1, false, "ab\xFF", 3);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/truncutf8")) { /* a message that ends in the middle of a sequence */
        wsx_send(c, 1, true, "ab\xE2\x82", 4);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/overlong") || path_is(t, "/surrogate") || path_is(t, "/toolarge")) {
        const char *bad = path_is(t, "/overlong") ? "\xC0\x80" : path_is(t, "/surrogate") ? "\xED\xA0\x80" : "\xF4\x90\x80\x80";
        wsx_send(c, 1, true, bad, strlen(bad));
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/masked")) {
        wsx_send_ex(c, 0x81, "hi", 2, true);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/rsv")) {
        wsx_send_ex(c, 0xC1, "hi", 2, false);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/badopcode")) {
        wsx_send(c, 3, true, "hi", 2);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/ctlbig")) {
        uint8_t big[126] = {0};
        wsx_send(c, 9, true, big, sizeof big);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/ctlfrag")) {
        wsx_send(c, 9, false, "x", 1);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/contfirst")) {
        wsx_send(c, 0, true, "x", 1);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/newwhile")) {
        wsx_send(c, 1, false, "x", 1);
        wsx_send(c, 1, true, "y", 1);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/big")) { /* a text message bigger than the limit the test sets */
        char *b = SDL_malloc(4000);
        memset(b, 'a', 4000);
        wsx_send(c, 1, true, b, 4000);
        SDL_free(b);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/bigfrags")) { /* each frame is small, together they pass the limit */
        uint8_t all[8 * 102];
        for (int i = 0; i < 8; i++) { /* one write, so the client cannot close in the middle of it */
            all[i * 102] = i == 0 ? 0x02 : 0x00;
            all[i * 102 + 1] = 100;
            memset(all + i * 102 + 2, 'x', 100);
        }
        ts_conn_write(c, all, sizeof all);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/hugeframe")) { /* only a header that announces 4 GiB */
        uint8_t h[10] = {0x82, 127, 0, 0, 0, 1, 0, 0, 0, 0};
        ts_conn_write(c, h, sizeof h);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/lenmsb")) { /* the 64-bit length has its top bit set */
        uint8_t h[10] = {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 1};
        ts_conn_write(c, h, sizeof h);
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/drop")) {
        c->abrupt = true;
    } else if (path_is(t, "/hold")) {
        wsx_frame f;
        while (wsx_read(c, x, &f, 20000)) {
            SDL_free(f.data);
        }
        SDL_AddAtomicInt(&x->disconnects, 1);
    } else if (path_is(t, "/sink")) { /* counts what arrives, answers a close */
        wsx_frame f;
        while (wsx_read(c, x, &f, 20000)) {
            if (f.op == 8) {
                wsx_note_close(x, &f);
                wsx_send(c, 8, true, f.data, f.len);
                SDL_free(f.data);
                return;
            }
            if (f.op == 1 || f.op == 2) {
                SDL_AddAtomicInt(&x->sink_msgs, 1);
            }
            SDL_AddAtomicInt(&x->sink_bytes, (int)f.len);
            SDL_free(f.data);
        }
    } else if (path_is(t, "/send")) { /* /send/N: one binary message of N bytes with a known pattern, then waits */
        size_t n = (size_t)atoi(t + 6);
        uint8_t *b = SDL_malloc(n + 1);
        for (size_t i = 0; i < n; i++) {
            b[i] = (uint8_t)(i * 7 + 3);
        }
        wsx_send(c, 2, true, b, n);
        SDL_free(b);
        wsx_wait_close(c, x, true);
    } else if (path_is(t, "/flood")) { /* /flood/N: N small text messages, then a close */
        int n = atoi(t + 7);
        for (int i = 0; i < n; i++) {
            char m[32];
            int ml = SDL_snprintf(m, sizeof m, "m%d", i);
            wsx_send(c, 1, true, m, (size_t)ml);
        }
        wsx_send_close(c, 1000, "done");
        wsx_wait_close(c, x, false);
    } else if (path_is(t, "/info")) {
        char m[600];
        SDL_LockMutex(x->m);
        int ml = SDL_snprintf(m, sizeof m, "cookie=%s;origin=%s;proto=%s", x->req_cookie, x->req_origin, x->req_proto);
        SDL_UnlockMutex(x->m);
        wsx_send(c, 1, true, m, (size_t)ml);
        wsx_echo(c, x);
    } else { /* /echo and the handshake variants */
        wsx_echo(c, x);
    }
}

/* ------------------------------------------------------------------ setup */

typedef struct wts {
    ts_server *srv;
    wsx x;
    embi_net *net;
    tlog log;
    tq *q;
    char url[320];
    bool tls;
} wts;

static inline bool wts_start(wts *s, bool tls)
{
    memset(s, 0, sizeof *s);
    wsx_init(&s->x);
    s->tls = tls;
    s->net = tn_new(&s->log);
    if (tls) {
        tn_trust_test_ca(s->net);
    }
    s->srv = ts_start(wsx_handler, &s->x, tls ? "server.crt" : nullptr, tls ? "server.key" : nullptr);
    s->q = tq_new();
    return s->srv != nullptr;
}

static inline void wts_stop(wts *s)
{
    ts_stop(s->srv);
    tn_free(s->net, &s->log);
    tq_free(s->q);
    wsx_free(&s->x);
}

static inline const char *wts_url(wts *s, const char *path)
{
    SDL_snprintf(s->url, sizeof s->url, "%s://127.0.0.1:%d%s", s->tls ? "wss" : "ws", s->srv->port, path);
    return s->url;
}

/* Connects and waits for the open event; false if the connection did not open. */
static inline bool wts_open(wts *s, wc *w, const char *path)
{
    embi_ws_request r = wc_request(w, wts_url(s, path));
    w->req = embi_ws_connect(s->net, &r);
    return wc_wait(w, WC_OPEN, 1, 10000);
}
