/*
 * EventSource (N1c): the event stream parser (HTML Standard 9.2.6) and the reconnecting connection.
 */
#include "sse.h"

#include "net_int.h"

#define DEFAULT_RETRY_MS 3000u
#define MIN_RETRY_MS 250u /* a server saying "retry: 0" must not make us reconnect in a tight loop */
#define MAX_RETRY_MS (24u * 3600u * 1000u)
#define MAX_EVENT (16u << 20)

/* ------------------------------------------------------------------ parser */

void embi_sse_parser_init(embi_sse_parser *p, size_t max, const char *last_event_id,
                          void (*on_event)(void *, const char *, const char *, size_t, const char *),
                          void (*on_retry)(void *, uint32_t), void *user)
{
    *p = (embi_sse_parser){0};
    p->max = max;
    p->on_event = on_event;
    p->on_retry = on_retry;
    p->user = user;
    if (last_event_id != nullptr) {
        (void)embi_buf_append(&p->idbuf, last_event_id, strlen(last_event_id));
    }
}

void embi_sse_parser_reset(embi_sse_parser *p)
{
    p->line.len = 0;
    p->data.len = 0;
    p->etype.len = 0;
    p->nhold = 0;
    p->bom_done = false;
    p->skip_lf = false;
}

void embi_sse_parser_free(embi_sse_parser *p)
{
    embi_buf_free(&p->line);
    embi_buf_free(&p->data);
    embi_buf_free(&p->etype);
    embi_buf_free(&p->idbuf);
}

/* A NUL-terminated copy of the bytes with invalid UTF-8 replaced by U+FFFD. */
static char *sanitize(const uint8_t *p, size_t n)
{
    embi_sb sb = {0};
    const char *s = (const char *)p, *end = p != nullptr ? s + n : s;
    while (s < end) {
        if ((uint8_t)*s < 0x80) {
            embi_sb_append_char(&sb, *s++);
        } else {
            embi_sb_append_cp(&sb, embi_utf8_decode(&s, end));
        }
    }
    char *out = embi_net_strndup(embi_sb_cstr(&sb), sb.len);
    embi_sb_free(&sb);
    return out;
}

static void dispatch(embi_sse_parser *p)
{
    if (p->data.len == 0) {
        p->etype.len = 0;
        return;
    }
    size_t dl = p->data.len;
    if (p->data.data[dl - 1] == '\n') {
        dl--;
    }
    char *data = sanitize(p->data.data, dl);
    char *type = p->etype.len > 0 ? sanitize(p->etype.data, p->etype.len) : embi_net_strdup("message");
    char *id = sanitize(p->idbuf.data, p->idbuf.len);
    if (p->on_event != nullptr) {
        p->on_event(p->user, type, data, strlen(data), id);
    }
    embi_net_mfree(data);
    embi_net_mfree(type);
    embi_net_mfree(id);
    p->data.len = 0;
    p->etype.len = 0;
}

static void end_line(embi_sse_parser *p)
{
    const uint8_t *l = p->line.data;
    size_t n = p->line.len;
    if (n == 0) {
        dispatch(p);
        return;
    }
    if (l[0] == ':') {
        p->line.len = 0;
        return;
    }
    const uint8_t *colon = memchr(l, ':', n);
    size_t nl = colon != nullptr ? (size_t)(colon - l) : n;
    const uint8_t *v = colon != nullptr ? colon + 1 : l + n;
    size_t vl = (size_t)(l + n - v);
    if (colon != nullptr && vl > 0 && *v == ' ') {
        v++;
        vl--;
    }
    if (nl == 4 && memcmp(l, "data", 4) == 0) {
        if (p->data.len + vl + 1 > p->max) {
            p->overflow = true;
        } else {
            (void)embi_buf_append(&p->data, v, vl);
            (void)embi_buf_append(&p->data, "\n", 1);
        }
    } else if (nl == 5 && memcmp(l, "event", 5) == 0) {
        p->etype.len = 0;
        (void)embi_buf_append(&p->etype, v, vl);
    } else if (nl == 2 && memcmp(l, "id", 2) == 0) {
        if (vl == 0 || memchr(v, 0, vl) == nullptr) {
            p->idbuf.len = 0;
            (void)embi_buf_append(&p->idbuf, v, vl);
        }
    } else if (nl == 5 && memcmp(l, "retry", 5) == 0 && vl > 0) {
        uint64_t ms = 0;
        bool digits = true;
        for (size_t i = 0; i < vl; i++) {
            if (v[i] < '0' || v[i] > '9') {
                digits = false;
                break;
            }
            ms = ms * 10 + (uint64_t)(v[i] - '0');
            if (ms > MAX_RETRY_MS) {
                ms = MAX_RETRY_MS;
            }
        }
        if (digits && p->on_retry != nullptr) {
            p->on_retry(p->user, (uint32_t)ms);
        }
    }
    p->line.len = 0;
}

static void feed_byte(embi_sse_parser *p, uint8_t b)
{
    if (p->skip_lf) {
        p->skip_lf = false;
        if (b == '\n') {
            return;
        }
    }
    if (b == '\r') {
        end_line(p);
        p->skip_lf = true;
    } else if (b == '\n') {
        end_line(p);
    } else if (p->line.len >= p->max) {
        p->overflow = true;
    } else {
        (void)embi_buf_append(&p->line, &b, 1);
    }
}

void embi_sse_parser_feed(embi_sse_parser *p, const uint8_t *d, size_t n)
{
    static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
    for (size_t i = 0; i < n && !p->overflow; i++) {
        uint8_t b = d[i];
        if (!p->bom_done) { /* a leading UTF-8 byte order mark is dropped */
            if (b == bom[p->nhold]) {
                p->nhold++;
                if (p->nhold == 3) {
                    p->bom_done = true;
                    p->nhold = 0;
                }
                continue;
            }
            p->bom_done = true;
            uint8_t held = p->nhold;
            p->nhold = 0;
            for (uint8_t k = 0; k < held; k++) {
                feed_byte(p, bom[k]);
            }
        }
        feed_byte(p, b);
    }
}

/* ------------------------------------------------------------------ connection */

struct embi_sse {
    SDL_AtomicInt refs; /* the caller's handle and one per request in flight */
    embi_net *n;
    char *url;
    bool creds;
    char *initiator;
    uint32_t retry_ms;
    embi_sse_callbacks cb;
    void *user;
    embi_net_post_fn post;
    void *post_user;
    /* the post thread only */
    bool closed;
    bool handle_dropped;
    bool announced;
    embi_net_req *cur;
    char *origin;
    embi_sse_parser parser;
};

static void sse_unref(embi_sse *s)
{
    if (SDL_AddAtomicInt(&s->refs, -1) != 1) {
        return;
    }
    embi_sse_callbacks cb = s->cb;
    void *user = s->user;
    embi_net_req_unref(s->cur);
    embi_sse_parser_free(&s->parser);
    embi_net_mfree(s->url);
    embi_net_mfree(s->initiator);
    embi_net_mfree(s->origin);
    embi_net_mfree(s);
    if (cb.release != nullptr) {
        cb.release(user);
    }
}

static void connect_now(embi_sse *s, uint32_t delay_ms);

static void sse_req_release(void *user)
{
    sse_unref(user);
}

static void fail_for_good(embi_sse *s, const char *msg)
{
    s->closed = true;
    if (s->cur != nullptr) {
        embi_net_abort(s->cur);
    }
    if (s->cb.on_error != nullptr) {
        s->cb.on_error(s->user, false, msg);
    }
}

static void sse_head(void *user, const embi_net_head *h)
{
    embi_sse *s = user;
    if (s->closed) {
        return;
    }
    char msg[200];
    if (h->status != 200) {
        SDL_snprintf(msg, sizeof msg, "EventSource's response has a status %u that is not 200", (unsigned)h->status);
        fail_for_good(s, msg);
        return;
    }
    embi_str ct = embi_headers_first(h->headers, EMBI_STR("content-type"));
    bool ok = false;
    if (ct.ptr != nullptr) {
        embi_str t = embi_header_trim(ct);
        uint32_t semi = 0;
        while (semi < t.len && t.ptr[semi] != ';') {
            semi++;
        }
        embi_str essence = embi_header_trim((embi_str){t.ptr, semi});
        ok = embi_str_ieq(essence, EMBI_STR("text/event-stream"));
    }
    if (!ok) {
        fail_for_good(s, "EventSource's response has a MIME type that is not \"text/event-stream\"");
        return;
    }
    embi_net_mfree(s->origin);
    s->origin = embi_net_strdup("null");
    embi_url u = {0};
    if (embi_url_parse((embi_str){h->url, (uint32_t)strlen(h->url)}, nullptr, &u)) {
        embi_sb o = {0};
        embi_url_origin(&u, &o);
        embi_net_mfree(s->origin);
        s->origin = embi_net_strndup(embi_sb_cstr(&o), o.len);
        embi_sb_free(&o);
        embi_url_free(&u);
    }
    s->announced = true;
    embi_sse_parser_reset(&s->parser);
    if (s->cb.on_open != nullptr) {
        s->cb.on_open(s->user);
    }
}

static void parser_event(void *user, const char *type, const char *data, size_t len, const char *id)
{
    embi_sse *s = user;
    if (!s->closed && s->cb.on_message != nullptr) {
        s->cb.on_message(s->user, type, data, len, id, s->origin != nullptr ? s->origin : "null");
    }
}

static void parser_retry(void *user, uint32_t ms)
{
    embi_sse *s = user;
    s->retry_ms = ms < MIN_RETRY_MS ? MIN_RETRY_MS : ms;
}

static void sse_data(void *user, const uint8_t *d, size_t n)
{
    embi_sse *s = user;
    if (s->closed || !s->announced) {
        return;
    }
    embi_sse_parser_feed(&s->parser, d, n);
    if (s->parser.overflow && !s->closed) {
        fail_for_good(s, "EventSource event too large");
    }
}

static void reconnect(embi_sse *s, const char *msg)
{
    s->announced = false;
    if (s->cb.on_error != nullptr) {
        s->cb.on_error(s->user, true, msg);
    }
    if (!s->closed) {
        connect_now(s, s->retry_ms);
    }
}

static void sse_end(void *user)
{
    embi_sse *s = user;
    if (!s->closed) {
        reconnect(s, "the event stream ended");
    }
}

static void sse_error(void *user, embi_net_err err, const char *msg)
{
    embi_sse *s = user;
    if (s->closed) {
        return;
    }
    if (err == EMBI_NET_ERR_URL || err == EMBI_NET_ERR_BLOCKED || err == EMBI_NET_ERR_REDIRECT || err == EMBI_NET_ERR_TLS ||
        err == EMBI_NET_ERR_DECODE) {
        fail_for_good(s, msg);
        return;
    }
    reconnect(s, msg);
}

static void connect_now(embi_sse *s, uint32_t delay_ms)
{
    embi_headers h = {0};
    embi_headers_add_raw(&h, EMBI_STR("accept"), EMBI_STR("text/event-stream"));
    embi_headers_add_raw(&h, EMBI_STR("cache-control"), EMBI_STR("no-store"));
    embi_sb id = {0};
    for (size_t i = 0; i < s->parser.idbuf.len; i++) { /* the id's UTF-8 bytes become the header value's bytes */
        uint8_t b = s->parser.idbuf.data[i];
        if (b < 0x80) {
            embi_sb_append_char(&id, (char)b);
        } else {
            embi_sb_append_cp(&id, b);
        }
    }
    if (id.len > 0) {
        embi_headers_add_raw(&h, EMBI_STR("last-event-id"), (embi_str){id.data, id.len});
    }
    embi_net_request r = {0};
    r.method = "GET";
    r.url = s->url;
    r.headers = &h;
    r.redirect = EMBI_NET_REDIRECT_FOLLOW;
    r.creds = s->creds ? EMBI_NET_CREDS_INCLUDE : EMBI_NET_CREDS_SAME_ORIGIN;
    r.initiator_origin = s->initiator;
    r.delay_ms = delay_ms;
    r.stream = true;
    r.cb = (embi_net_callbacks){sse_head, sse_data, sse_end, sse_error, sse_req_release};
    r.user = s;
    r.post = s->post;
    r.post_user = s->post_user;
    SDL_AddAtomicInt(&s->refs, 1);
    embi_net_req *old = s->cur;
    s->cur = embi_net_start(s->n, &r);
    embi_net_req_unref(old);
    embi_headers_free(&h);
    embi_sb_free(&id);
}

embi_sse *embi_sse_open(embi_net *n, const embi_sse_params *p)
{
    if (n == nullptr || p == nullptr || p->url == nullptr || p->post == nullptr) {
        return nullptr;
    }
    embi_sse *s = embi_net_zalloc(sizeof *s);
    SDL_SetAtomicInt(&s->refs, 1);
    s->n = n;
    s->url = embi_net_strdup(p->url);
    s->creds = p->with_credentials;
    s->initiator = p->initiator_origin != nullptr ? embi_net_strdup(p->initiator_origin) : nullptr;
    s->retry_ms = p->retry_ms != 0 ? p->retry_ms : DEFAULT_RETRY_MS;
    s->cb = p->cb;
    s->user = p->user;
    s->post = p->post;
    s->post_user = p->post_user;
    embi_sse_parser_init(&s->parser, MAX_EVENT, nullptr, parser_event, parser_retry, s);
    connect_now(s, 0);
    return s;
}

void embi_sse_close(embi_sse *s)
{
    if (s == nullptr || s->handle_dropped) {
        return;
    }
    s->handle_dropped = true;
    s->closed = true;
    if (s->cur != nullptr) {
        embi_net_abort(s->cur);
    }
    sse_unref(s);
}
