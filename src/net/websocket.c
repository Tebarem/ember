/*
 * WebSocket client (N1c, RFC 6455): the opening handshake, framing with client masking, fragmentation, the control
 * frames and the closing handshake, over the connection the request state machine of net.c hands over after the 101
 * response. Everything here runs on the network thread, except the thread-safe entry points (send, close, sent) and
 * the event callbacks, which run where the request's post function delivers.
 *
 * Memory is bounded: an incoming message may not exceed the request's limit (frames are accumulated as they arrive and
 * a frame that would pass the limit fails the connection with 1009), control frames are 125 bytes at most, and the
 * network thread stops reading while the receiving thread is more than a few MB behind (embi_net_req_congested).
 */
#include "ws_int.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define DEFAULT_MAX_MESSAGE (32u << 20)
#define CLOSE_WAIT_MS 5000u
#define EV_COST 256 /* charged to the backpressure counter per queued event (the event, its copy and the queue item) */
#define FINISH_DRAIN_MS 15 /* the network thread is shared: give the socket only a moment to send the last frames */
#define WRITE_AHEAD (1 << 20) /* more than this waiting in the socket's send queue: encode no more frames this pass */
#define FILL_MAX (256u * 1024u)

/* ------------------------------------------------------------------ SHA-1 (the accept key only) */

static uint32_t rol(uint32_t v, int s)
{
    return (v << s) | (v >> (32 - s));
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

/* Hashes up to 119 bytes (two blocks with padding): all a handshake key needs. */
static void sha1_short(const uint8_t *d, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[128] = {0};
    memcpy(buf, d, n);
    buf[n] = 0x80;
    size_t total = n + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) {
        buf[total - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    }
    for (size_t o = 0; o < total; o += 64) {
        sha1_block(h, buf + o);
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

/* The Sec-WebSocket-Accept value for a key (RFC 6455 4.2.2): 28 characters and a NUL. */
void embi_ws_accept_key(const char *key, char out[32])
{
    uint8_t in[80];
    size_t kl = strlen(key);
    memcpy(in, key, kl);
    memcpy(in + kl, WS_GUID, sizeof WS_GUID - 1);
    uint8_t dg[20];
    sha1_short(in, kl + sizeof WS_GUID - 1, dg);
    embi_sb b = {0};
    embi_base64_encode(dg, 20, &b);
    SDL_snprintf(out, 32, "%.*s", (int)b.len, b.data);
    embi_sb_free(&b);
}

/* ------------------------------------------------------------------ UTF-8 validation (incremental DFA) */

static uint8_t u8class(uint8_t b)
{
    if (b < 0x80) return 0;
    if (b < 0x90) return 1;
    if (b < 0xA0) return 9;
    if (b < 0xC0) return 7;
    if (b < 0xC2) return 8;
    if (b < 0xE0) return 2;
    if (b == 0xE0) return 10;
    if (b == 0xED) return 4;
    if (b < 0xF0) return 3;
    if (b == 0xF0) return 11;
    if (b < 0xF4) return 6;
    if (b == 0xF4) return 5;
    return 8;
}

/* Bjoern Hoehrmann's UTF-8 DFA: states are multiples of 12, 0 accepts, 12 rejects. */
static const uint8_t k_u8next[108] = {
    0,  12, 24, 36, 60, 96, 84, 12, 12, 12, 48, 72, /**/ 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 0,  12, 12, 12, 12, 12, 0,  12, 0,  12, 12, /**/ 12, 24, 12, 12, 12, 12, 12, 24, 12, 24, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 24, 12, 12, 12, 12, /**/ 12, 24, 12, 12, 12, 12, 12, 12, 12, 24, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 36, 12, 36, 12, 12, /**/ 12, 36, 12, 12, 12, 12, 12, 36, 12, 36, 12, 12,
    12, 36, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
};

#define U8_ACCEPT 0u
#define U8_REJECT 12u

static uint32_t u8feed(uint32_t state, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n && state != U8_REJECT; i++) {
        state = k_u8next[state + u8class(p[i])];
    }
    return state;
}

/* ------------------------------------------------------------------ the object */

typedef struct ws_item {
    struct ws_item *next;
    uint8_t op; /* 1 text, 2 binary, 8 close */
    size_t len;
    uint8_t *data;
} ws_item;

typedef enum rstate {
    RS_HEADER,
    RS_PAYLOAD,
} rstate;

struct embi_ws {
    char key[32];
    char **protos;
    uint32_t nprotos;
    char *origin;
    embi_ws_callbacks cb;
    void *user;
    uint32_t max_msg;
    /* shared with the caller's thread */
    SDL_Mutex *m;
    ws_item *qh, *qt;
    bool close_req;
    uint64_t sent;
    /* network thread */
    bool opened;
    rstate rs;
    uint8_t op;
    bool fin;
    uint64_t remain;
    bool in_msg, msg_binary;
    uint32_t u8;
    embi_buf msg;
    uint8_t ctl[125];
    size_t ctl_len;
    uint8_t pong[125]; /* the latest ping's payload, answered once per pass (a ping flood costs one pong) */
    size_t pong_len;
    bool pong_pending;
    bool close_sent, close_recv;
    uint64_t close_sent_at;
};

void embi_ws_free(embi_ws *w)
{
    if (w == nullptr) {
        return;
    }
    while (w->qh != nullptr) {
        ws_item *it = w->qh;
        w->qh = it->next;
        embi_net_mfree(it->data);
        embi_net_mfree(it);
    }
    for (uint32_t i = 0; i < w->nprotos; i++) {
        embi_net_mfree(w->protos[i]);
    }
    embi_net_mfree(w->protos);
    embi_net_mfree(w->origin);
    embi_buf_free(&w->msg);
    if (w->m != nullptr) {
        SDL_DestroyMutex(w->m);
    }
    embi_net_mfree(w);
}

/* ------------------------------------------------------------------ events */

typedef enum we_kind {
    WE_OPEN,
    WE_MESSAGE,
    WE_ERROR,
    WE_CLOSE,
} we_kind;

typedef struct ws_ev {
    embi_ws *w;
    we_kind kind;
    bool binary, clean;
    uint16_t code;
    uint8_t *data; /* always NUL-terminated (one byte past len) */
    size_t len;
} ws_ev;

static void ev_free(ws_ev *e)
{
    embi_net_mfree(e->data);
    embi_net_mfree(e);
}

static void ev_run(void *arg)
{
    ws_ev *e = arg;
    embi_ws *w = e->w;
    switch (e->kind) {
    case WE_OPEN:
        if (w->cb.on_open != nullptr) {
            w->cb.on_open(w->user, (const char *)e->data);
        }
        break;
    case WE_MESSAGE:
        if (w->cb.on_message != nullptr) {
            w->cb.on_message(w->user, e->binary, e->data, e->len);
        }
        break;
    case WE_ERROR:
        if (w->cb.on_error != nullptr) {
            w->cb.on_error(w->user, (const char *)e->data);
        }
        break;
    case WE_CLOSE:
        if (w->cb.on_close != nullptr) {
            w->cb.on_close(w->user, e->clean, e->code, (const char *)e->data);
        }
        break;
    }
    ev_free(e);
}

static void ev_drop(void *arg)
{
    ev_free(arg);
}

static void ev_post(embi_net_req *r, embi_ws *w, we_kind kind, const void *data, size_t len, bool binary, bool clean, uint16_t code)
{
    ws_ev *e = embi_net_zalloc(sizeof *e);
    e->w = w;
    e->kind = kind;
    e->binary = binary;
    e->clean = clean;
    e->code = code;
    e->len = len;
    e->data = embi_net_zalloc(len + 1);
    if (len > 0) {
        memcpy(e->data, data, len);
    }
    embi_net_req_post_call(r, ev_run, ev_drop, e, (kind == WE_MESSAGE ? len : 0) + EV_COST, false);
}

/* The request's own callbacks: a failure before the connection opened, and the release. They run on the receiving
 * thread. */
static void net_error(void *user, embi_net_err err, const char *msg)
{
    (void)err;
    embi_ws *w = user;
    if (w->cb.on_error != nullptr) {
        w->cb.on_error(w->user, msg);
    }
    if (w->cb.on_close != nullptr) {
        w->cb.on_close(w->user, false, 1006, "");
    }
}

static void net_release(void *user)
{
    embi_ws *w = user;
    if (w->cb.release != nullptr) {
        w->cb.release(w->user);
    }
}

/* ------------------------------------------------------------------ creation and the handshake */

static bool starts_with_ci(const char *s, const char *prefix)
{
    return SDL_strncasecmp(s, prefix, strlen(prefix)) == 0;
}

embi_net_req *embi_ws_connect(embi_net *n, const embi_ws_request *q)
{
    if (n == nullptr || q == nullptr || q->url == nullptr) {
        return nullptr;
    }
    embi_ws *w = embi_net_zalloc(sizeof *w);
    w->m = SDL_CreateMutex();
    w->cb = q->cb;
    w->user = q->user;
    w->max_msg = q->max_message != 0 ? q->max_message : DEFAULT_MAX_MESSAGE;
    if (q->nprotocols > 0 && q->protocols != nullptr) {
        w->protos = embi_net_zalloc(sizeof *w->protos * q->nprotocols);
        for (uint32_t i = 0; i < q->nprotocols; i++) {
            w->protos[w->nprotos++] = embi_net_strdup(q->protocols[i]);
        }
    }
    w->origin = q->origin != nullptr ? embi_net_strdup(q->origin) : nullptr;
    uint8_t raw[16];
    if (!embi_net_entropy(raw, sizeof raw)) {
        for (size_t i = 0; i < sizeof raw; i++) {
            raw[i] = (uint8_t)SDL_rand_bits();
        }
    }
    embi_sb kb = {0};
    embi_base64_encode(raw, sizeof raw, &kb);
    SDL_snprintf(w->key, sizeof w->key, "%.*s", (int)kb.len, kb.data);
    embi_sb_free(&kb);

    /* The service speaks http and https; ws and wss are the same hosts and ports. */
    const char *u = q->url;
    char *http = nullptr;
    if (starts_with_ci(u, "ws://")) {
        size_t l = strlen(u);
        http = embi_net_zalloc(l + 3);
        SDL_snprintf(http, l + 3, "http://%s", u + 5);
    } else if (starts_with_ci(u, "wss://")) {
        size_t l = strlen(u);
        http = embi_net_zalloc(l + 3);
        SDL_snprintf(http, l + 3, "https://%s", u + 6);
    }
    embi_net_request nr = {0};
    nr.method = "GET";
    nr.url = http != nullptr ? http : u;
    nr.redirect = EMBI_NET_REDIRECT_ERROR;
    nr.creds = EMBI_NET_CREDS_INCLUDE;
    nr.cb.on_error = net_error;
    nr.cb.release = net_release;
    nr.user = w;
    nr.post = q->post;
    nr.post_user = q->post_user;
    embi_net_req *r = embi_net_start_ex(n, &nr, w);
    embi_net_mfree(http);
    return r;
}

void embi_ws_request_headers(embi_ws *w, embi_sb *out)
{
    embi_sb_append(out, EMBI_STR("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "));
    embi_sb_append(out, (embi_str){w->key, (uint32_t)strlen(w->key)});
    embi_sb_append(out, EMBI_STR("\r\nSec-WebSocket-Version: 13\r\n"));
    if (w->nprotos > 0) {
        embi_sb_append(out, EMBI_STR("Sec-WebSocket-Protocol: "));
        for (uint32_t i = 0; i < w->nprotos; i++) {
            if (i > 0) {
                embi_sb_append(out, EMBI_STR(", "));
            }
            embi_sb_append(out, (embi_str){w->protos[i], (uint32_t)strlen(w->protos[i])});
        }
        embi_sb_append(out, EMBI_STR("\r\n"));
    }
    if (w->origin != nullptr) {
        embi_sb_append(out, EMBI_STR("Origin: "));
        embi_sb_append(out, (embi_str){w->origin, (uint32_t)strlen(w->origin)});
        embi_sb_append(out, EMBI_STR("\r\n"));
    }
}

/* The value of the only header called name; false when there are two or more. found says whether there was one. */
static bool header_once(const embi_http_head *h, const char *name, embi_str *value, bool *found)
{
    *found = false;
    uint32_t nl = (uint32_t)strlen(name);
    for (uint32_t i = 0; i < h->headers.list.len; i++) {
        const embi_kv *kv = &h->headers.list.data[i];
        if (kv->name.len == nl && memcmp(kv->name.data, name, nl) == 0) {
            if (*found) {
                return false;
            }
            *found = true;
            *value = embi_header_trim((embi_str){kv->value.data, kv->value.len});
        }
    }
    return true;
}

bool embi_ws_accept(embi_net_req *r, embi_ws *w, const embi_http_head *h, char *err, size_t err_cap)
{
    embi_str up = {0}, conn = {0}, acc = {0}, ext = {0}, proto = {0};
    bool fu, fc, fa, fe, fp;
    if (!header_once(h, "upgrade", &up, &fu) || !header_once(h, "connection", &conn, &fc) ||
        !header_once(h, "sec-websocket-accept", &acc, &fa) || !header_once(h, "sec-websocket-extensions", &ext, &fe) ||
        !header_once(h, "sec-websocket-protocol", &proto, &fp)) {
        SDL_snprintf(err, err_cap, "WebSocket handshake failed: a header appears twice");
        return false;
    }
    if (!fu || !embi_str_ieq(up, EMBI_STR("websocket"))) {
        SDL_snprintf(err, err_cap, "WebSocket handshake failed: 'Upgrade' header is missing or is not 'websocket'");
        return false;
    }
    if (!fc || !embi_http_has_token(conn, EMBI_STR("upgrade"))) {
        SDL_snprintf(err, err_cap, "WebSocket handshake failed: 'Connection' header is missing or is not 'Upgrade'");
        return false;
    }
    char expect[32];
    embi_ws_accept_key(w->key, expect);
    if (!fa || acc.len != strlen(expect) || memcmp(acc.ptr, expect, acc.len) != 0) {
        SDL_snprintf(err, err_cap, "WebSocket handshake failed: 'Sec-WebSocket-Accept' header is missing or does not match the key");
        return false;
    }
    if (fe && ext.len > 0) {
        SDL_snprintf(err, err_cap, "WebSocket handshake failed: the server selected an extension the client did not offer");
        return false;
    }
    const char *chosen = "";
    if (fp) {
        bool ok = false;
        for (uint32_t i = 0; i < w->nprotos && !ok; i++) {
            ok = strlen(w->protos[i]) == proto.len && memcmp(w->protos[i], proto.ptr, proto.len) == 0;
            if (ok) {
                chosen = w->protos[i];
            }
        }
        if (!ok) {
            SDL_snprintf(err, err_cap, "WebSocket handshake failed: the server selected a subprotocol the client did not offer");
            return false;
        }
    }
    w->opened = true;
    ev_post(r, w, WE_OPEN, chosen, strlen(chosen), false, false, 0);
    return true;
}

bool embi_ws_close_early(embi_ws *w)
{
    SDL_LockMutex(w->m);
    bool c = w->close_req;
    SDL_UnlockMutex(w->m);
    return c && !w->opened;
}

/* ------------------------------------------------------------------ thread-safe entry points */

static void queue_item(embi_ws *w, uint8_t op, const void *data, size_t len)
{
    ws_item *it = embi_net_zalloc(sizeof *it);
    it->op = op;
    it->len = len;
    it->data = embi_net_zalloc(len + 1);
    if (len > 0) {
        memcpy(it->data, data, len);
    }
    if (w->qt != nullptr) {
        w->qt->next = it;
    } else {
        w->qh = it;
    }
    w->qt = it;
}

bool embi_ws_send(embi_net_req *r, bool binary, const void *data, size_t len)
{
    embi_ws *w = embi_net_req_ws(r);
    if (w == nullptr) {
        return false;
    }
    SDL_LockMutex(w->m);
    bool ok = !w->close_req;
    if (ok) {
        queue_item(w, binary ? 2 : 1, data, len);
    }
    SDL_UnlockMutex(w->m);
    return ok;
}

void embi_ws_close(embi_net_req *r, uint16_t code, const char *reason, size_t reason_len)
{
    embi_ws *w = embi_net_req_ws(r);
    if (w == nullptr) {
        return;
    }
    SDL_LockMutex(w->m);
    if (!w->close_req) {
        w->close_req = true;
        uint8_t payload[125];
        size_t n = 0;
        if (code != 0) {
            payload[0] = (uint8_t)(code >> 8);
            payload[1] = (uint8_t)code;
            n = 2;
            if (reason_len > 123) {
                reason_len = 123;
            }
            if (reason_len > 0) {
                memcpy(payload + 2, reason, reason_len);
                n += reason_len;
            }
        }
        queue_item(w, 8, payload, n);
    }
    SDL_UnlockMutex(w->m);
}

uint64_t embi_ws_sent(embi_net_req *r)
{
    embi_ws *w = embi_net_req_ws(r);
    if (w == nullptr) {
        return 0;
    }
    SDL_LockMutex(w->m);
    uint64_t s = w->sent;
    SDL_UnlockMutex(w->m);
    return s;
}

/* ------------------------------------------------------------------ frames */

static bool write_frame(embi_net_conn *c, uint8_t op, const uint8_t *p, size_t n)
{
    uint8_t hdr[14];
    size_t hl = 0;
    hdr[0] = (uint8_t)(0x80 | op);
    if (n < 126) {
        hdr[1] = (uint8_t)(0x80 | n);
        hl = 2;
    } else if (n <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)n;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i)));
        }
        hl = 10;
    }
    uint8_t mask[4];
    if (!embi_net_entropy(mask, sizeof mask)) {
        uint32_t v = SDL_rand_bits();
        memcpy(mask, &v, 4);
    }
    memcpy(hdr + hl, mask, 4);
    hl += 4;
    embi_buf b = {0};
    (void)embi_buf_reserve(&b, hl + n);
    (void)embi_buf_append(&b, hdr, hl);
    for (size_t i = 0; i < n; i++) {
        b.data[b.len + i] = (uint8_t)(p[i] ^ mask[i & 3]);
    }
    b.len += n;
    char err[120];
    bool ok = embi_net_conn_write(c, b.data, b.len, err, sizeof err);
    embi_buf_free(&b);
    return ok;
}

/* Ends the connection: queues the events and gives the socket a moment to send what it holds. Always true. */
static bool finish(embi_net_req *r, embi_ws *w, embi_net_conn *c, bool clean, uint16_t code, const uint8_t *reason, size_t rlen,
                   const char *fail_msg)
{
    if (fail_msg != nullptr) {
        ev_post(r, w, WE_ERROR, fail_msg, strlen(fail_msg), false, false, 0);
    }
    ev_post(r, w, WE_CLOSE, reason, rlen, false, clean, code);
    if (c->sock != nullptr && NET_GetStreamSocketPendingWrites(c->sock) > 0) {
        NET_WaitUntilStreamSocketDrained(c->sock, FINISH_DRAIN_MS);
    }
    return true;
}

/* A protocol failure: a close frame with code (best effort) and the error and close(1006) events. */
static bool fail(embi_net_req *r, embi_ws *w, embi_net_conn *c, uint16_t code, const char *msg)
{
    if (!w->close_sent) {
        uint8_t p[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        (void)write_frame(c, 8, p, 2);
        w->close_sent = true;
    }
    return finish(r, w, c, false, 1006, nullptr, 0, msg);
}

static bool close_code_ok(uint16_t c)
{
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

static bool on_control(embi_net_req *r, embi_ws *w, embi_net_conn *c)
{
    if (w->op == 9) { /* ping: a pong with the same payload, unless we are already closing */
        if (!w->close_sent) {
            memcpy(w->pong, w->ctl, w->ctl_len);
            w->pong_len = w->ctl_len;
            w->pong_pending = true;
        }
        return false;
    }
    if (w->op == 10) {
        return false;
    }
    /* close */
    uint16_t code = 1005;
    const uint8_t *reason = nullptr;
    size_t rlen = 0;
    if (w->ctl_len == 1) {
        return fail(r, w, c, 1002, "WebSocket protocol error: a close frame with a one byte payload");
    }
    if (w->ctl_len >= 2) {
        code = (uint16_t)(w->ctl[0] << 8 | w->ctl[1]);
        if (!close_code_ok(code)) {
            return fail(r, w, c, 1002, "WebSocket protocol error: invalid close code");
        }
        reason = w->ctl + 2;
        rlen = w->ctl_len - 2;
        if (u8feed(U8_ACCEPT, reason, rlen) != U8_ACCEPT) {
            return fail(r, w, c, 1007, "WebSocket protocol error: the close reason is not valid UTF-8");
        }
    }
    w->close_recv = true;
    if (!w->close_sent) { /* echo the status code */
        (void)write_frame(c, 8, w->ctl, w->ctl_len >= 2 ? 2 : 0);
        w->close_sent = true;
    }
    return finish(r, w, c, true, code, reason, rlen, nullptr);
}

/* Parses the frames in c->rbuf. True when the connection is over. */
static bool parse(embi_net_req *r, embi_ws *w, embi_net_conn *c)
{
    size_t pos = 0;
    bool over = false;
    for (;;) {
        const uint8_t *p = c->rbuf.data + pos;
        size_t avail = c->rbuf.len - pos;
        if (w->rs == RS_HEADER) {
            if (avail < 2 || embi_net_req_congested(r)) { /* the rest stays in the read buffer until the consumer catches up */
                break;
            }
            bool fin = (p[0] & 0x80) != 0;
            unsigned rsv = p[0] & 0x70;
            uint8_t op = p[0] & 0x0F;
            bool masked = (p[1] & 0x80) != 0;
            uint64_t len = p[1] & 0x7F;
            size_t hl = 2;
            if (len == 126) {
                if (avail < 4) {
                    break;
                }
                len = (uint64_t)p[2] << 8 | p[3];
                hl = 4;
            } else if (len == 127) {
                if (avail < 10) {
                    break;
                }
                len = 0;
                for (int i = 0; i < 8; i++) {
                    len = len << 8 | p[2 + i];
                }
                hl = 10;
                if ((len >> 63) != 0) {
                    over = fail(r, w, c, 1002, "WebSocket protocol error: invalid frame length");
                    break;
                }
            }
            const char *bad = nullptr;
            uint16_t bad_code = 1002;
            if (rsv != 0) {
                bad = "WebSocket protocol error: reserved bits are set";
            } else if (masked) {
                bad = "WebSocket protocol error: a server frame is masked";
            } else if (op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op > 10) {
                bad = "WebSocket protocol error: unknown opcode";
            } else if (op >= 8 && (!fin || len > 125)) {
                bad = "WebSocket protocol error: a control frame is fragmented or longer than 125 bytes";
            } else if (op == 0 && !w->in_msg) {
                bad = "WebSocket protocol error: a continuation frame without a message to continue";
            } else if ((op == 1 || op == 2) && w->in_msg) {
                bad = "WebSocket protocol error: a new message started before the last one ended";
            } else if (op < 8 && (uint64_t)w->msg.len + len > w->max_msg) {
                bad = "WebSocket message too big";
                bad_code = 1009;
            }
            if (bad != nullptr) {
                over = fail(r, w, c, bad_code, bad);
                break;
            }
            pos += hl;
            w->op = op;
            w->fin = fin;
            w->remain = len;
            w->ctl_len = 0;
            w->rs = RS_PAYLOAD;
            if (op == 1 || op == 2) {
                w->in_msg = true;
                w->msg_binary = op == 2;
                w->msg.len = 0;
                w->u8 = U8_ACCEPT;
            }
            continue;
        }
        size_t take = avail < w->remain ? avail : (size_t)w->remain;
        if (take > 0) {
            if (w->op >= 8) {
                memcpy(w->ctl + w->ctl_len, p, take);
                w->ctl_len += take;
            } else {
                (void)embi_buf_append(&w->msg, p, take);
                if (!w->msg_binary) {
                    w->u8 = u8feed(w->u8, p, take);
                    if (w->u8 == U8_REJECT) {
                        over = fail(r, w, c, 1007, "WebSocket protocol error: invalid UTF-8 in a text message");
                        break;
                    }
                }
            }
            pos += take;
            w->remain -= take;
        }
        if (w->remain > 0) {
            break;
        }
        w->rs = RS_HEADER;
        if (w->op >= 8) {
            if (on_control(r, w, c)) {
                over = true;
                break;
            }
        } else if (w->fin) {
            w->in_msg = false;
            if (!w->msg_binary && w->u8 != U8_ACCEPT) {
                over = fail(r, w, c, 1007, "WebSocket protocol error: invalid UTF-8 in a text message");
                break;
            }
            if (!w->close_sent) { /* messages that arrive while closing are dropped */
                ev_post(r, w, WE_MESSAGE, w->msg.data, w->msg.len, w->msg_binary, false, 0);
            }
            w->msg.len = 0;
            if (w->msg.cap > (1u << 20)) {
                embi_buf_free(&w->msg);
            }
        }
    }
    if (!over && pos > 0) {
        memmove(c->rbuf.data, c->rbuf.data + pos, c->rbuf.len - pos);
        c->rbuf.len -= pos;
    }
    return over;
}

/* Sends what the caller queued. True when the connection is over. */
static bool flush_queue(embi_net_req *r, embi_ws *w, embi_net_conn *c, uint64_t now)
{
    while (c->sock != nullptr && NET_GetStreamSocketPendingWrites(c->sock) < WRITE_AHEAD) {
        SDL_LockMutex(w->m);
        ws_item *it = w->qh;
        if (it != nullptr) {
            w->qh = it->next;
            if (w->qh == nullptr) {
                w->qt = nullptr;
            }
        }
        SDL_UnlockMutex(w->m);
        if (it == nullptr) {
            break;
        }
        bool ok = true;
        if (!w->close_sent) {
            ok = write_frame(c, it->op, it->data, it->len);
            if (ok) {
                if (it->op == 8) {
                    w->close_sent = true;
                    w->close_sent_at = now;
                } else {
                    SDL_LockMutex(w->m);
                    w->sent += it->len;
                    SDL_UnlockMutex(w->m);
                }
            }
        }
        embi_net_mfree(it->data);
        embi_net_mfree(it);
        if (!ok) {
            return finish(r, w, c, false, 1006, nullptr, 0, nullptr);
        }
    }
    return false;
}

bool embi_ws_step(embi_net_req *r, embi_ws *w, embi_net_conn *c, uint64_t now)
{
    char err[200];
    err[0] = '\0';
    fill_rc fr = FILL_NONE;
    if (!embi_net_req_congested(r)) {
        fr = embi_net_conn_fill(c, FILL_MAX, err, sizeof err);
    }
    if (parse(r, w, c)) {
        return true;
    }
    if (fr == FILL_EOF || fr == FILL_ERROR) { /* the transport ended without the closing handshake */
        return finish(r, w, c, false, 1006, nullptr, 0, nullptr);
    }
    if (w->pong_pending && !w->close_sent && c->sock != nullptr && NET_GetStreamSocketPendingWrites(c->sock) < WRITE_AHEAD) {
        w->pong_pending = false; /* only the latest ping is answered; none while the send queue is full */
        if (!write_frame(c, 10, w->pong, w->pong_len)) {
            return finish(r, w, c, false, 1006, nullptr, 0, nullptr);
        }
    }
    if (flush_queue(r, w, c, now)) {
        return true;
    }
    if (w->close_sent && !w->close_recv && now > w->close_sent_at && now - w->close_sent_at > CLOSE_WAIT_MS) {
        return finish(r, w, c, false, 1006, nullptr, 0, nullptr);
    }
    return false;
}
