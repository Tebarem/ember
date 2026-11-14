/*
 * EventSource tests (N1c): the event stream parser on vectors (whole and one byte at a time), and the connection
 * against the 127.0.0.1 test server: events, event types, ids, origin, reconnection with retry and Last-Event-ID,
 * failures (content type, status), credentials, close, and shutdown.
 */
#include "ws_test.h"

#include "net/sse.h"

/* ------------------------------------------------------------------ parser vectors */

typedef struct pv {
    embi_sb log;
    int retries;
    uint32_t last_retry;
} pv;

static void pv_event(void *u, const char *type, const char *data, size_t len, const char *id)
{
    pv *p = u;
    embi_sb_append_char(&p->log, '[');
    embi_sb_append(&p->log, (embi_str){type, (uint32_t)strlen(type)});
    embi_sb_append_char(&p->log, '|');
    embi_sb_append(&p->log, (embi_str){data, (uint32_t)len});
    embi_sb_append_char(&p->log, '|');
    embi_sb_append(&p->log, (embi_str){id, (uint32_t)strlen(id)});
    embi_sb_append_char(&p->log, ']');
}

static void pv_retry(void *u, uint32_t ms)
{
    pv *p = u;
    p->retries++;
    p->last_retry = ms;
}

/* Feeds the chunks (NULL-terminated list) and returns the event log; split feeds one byte at a time. */
static char *run_vector(const char *const *chunks, bool split, int *retries, uint32_t *last_retry, size_t max)
{
    pv p = {0};
    embi_sse_parser ps;
    embi_sse_parser_init(&ps, max, nullptr, pv_event, pv_retry, &p);
    for (; *chunks != nullptr; chunks++) {
        size_t n = strlen(*chunks);
        if (split) {
            for (size_t i = 0; i < n; i++) {
                embi_sse_parser_feed(&ps, (const uint8_t *)*chunks + i, 1);
            }
        } else {
            embi_sse_parser_feed(&ps, (const uint8_t *)*chunks, n);
        }
    }
    char *out = SDL_strdup(embi_sb_cstr(&p.log));
    if (retries != nullptr) {
        *retries = p.retries;
    }
    if (last_retry != nullptr) {
        *last_retry = p.last_retry;
    }
    embi_sb_free(&p.log);
    embi_sse_parser_free(&ps);
    return out;
}

static void expect_vector(const char *const *chunks, const char *want)
{
    for (int split = 0; split < 2; split++) {
        char *got = run_vector(chunks, split != 0, nullptr, nullptr, 1 << 20);
        if (strcmp(got, want) != 0) {
            char msg[600];
            SDL_snprintf(msg, sizeof msg, "vector %s (split=%d): got %s want %s", chunks[0], split, got, want);
            emb_test_report_fail(__FILE__, __LINE__, msg);
        }
        SDL_free(got);
    }
}

#define VEC(name, want, ...)                                                                   \
    EMB_TEST(parse_##name)                                                                     \
    {                                                                                          \
        const char *const chunks[] = {__VA_ARGS__, nullptr};                                   \
        expect_vector(chunks, want);                                                           \
    }

VEC(simple, "[message|a|]", "data: a\n\n")
VEC(no_space_after_colon, "[message|x|]", "data:x\n\n")
VEC(only_one_space_is_removed, "[message| x|]", "data:  x\n\n")
VEC(crlf, "[message|a|]", "data: a\r\n\r\n")
VEC(cr_only, "[message|a|]", "data: a\r\r")
VEC(mixed_line_ends, "[message|a\nb|]", "data: a\r", "data: b\n\r\n")
VEC(cr_then_lf_in_the_next_chunk, "[message|a|]", "data: a\r", "\n\n")
VEC(multi_line_data, "[message|a\nb\nc|]", "data: a\ndata: b\ndata: c\n\n")
VEC(empty_data_line_makes_an_empty_event, "[message||]", "data\n\n")
VEC(empty_data_value_twice, "[message|\n|]", "data:\ndata:\n\n")
VEC(comment_lines_are_ignored, "[message|a|]", ": a comment\ndata: a\n:another\n\n")
VEC(event_type, "[foo|a|]", "event: foo\ndata: a\n\n")
VEC(event_type_does_not_stick, "[foo|a|][message|b|]", "event: foo\ndata: a\n\ndata: b\n\n")
VEC(an_event_without_data_is_not_dispatched, "", "event: foo\n\n")
VEC(blank_lines_alone_dispatch_nothing, "", "\n\n\n")
VEC(id_field, "[message|a|5]", "id: 5\ndata: a\n\n")
VEC(id_persists_to_the_next_event, "[message|a|5][message|b|5]", "id: 5\ndata: a\n\ndata: b\n\n")
VEC(id_without_a_value_resets, "[message|a|5][message|b|]", "id: 5\ndata: a\n\nid\ndata: b\n\n")
VEC(unknown_fields_are_ignored, "[message|a|]", "foo: bar\nbaz\ndata: a\n\n")
VEC(field_names_are_case_sensitive, "[message|a|]", "Data: x\nDATA: y\ndata: a\n\n")
VEC(bom_is_dropped, "[message|a|]", "\xEF\xBB\xBF" "data: a\n\n")
VEC(bom_split_over_chunks, "[message|a|]", "\xEF", "\xBB", "\xBF" "data: a\n\n")
VEC(a_bom_lookalike_is_not_dropped, "[message|a|]", "\xEF\xBB" "data: x\ndata: a\n\n")
VEC(invalid_utf8_becomes_fffd, "[message|\xEF\xBF\xBD|]", "data: \xFF\n\n")
VEC(valid_utf8_passes, "[message|\xE2\x82\xAC|]", "data: \xE2\x82\xAC\n\n")
VEC(a_half_event_is_discarded, "[message|a|]", "data: a\n\ndata: half\n")
VEC(a_half_line_is_discarded, "[message|a|]", "data: a\n\ndata: ha")
VEC(two_events_in_one_chunk, "[message|a|][message|b|]", "data: a\n\ndata: b\n\n")
VEC(a_colon_in_the_value, "[message|a:b|]", "data: a:b\n\n")
VEC(a_line_that_is_only_a_colon, "[message|a|]", ":\ndata: a\n\n")

EMB_TEST(parse_an_id_with_nul_is_ignored)
{
    pv p = {0};
    embi_sse_parser ps;
    embi_sse_parser_init(&ps, 1 << 20, "keep", pv_event, pv_retry, &p);
    static const uint8_t in[] = {'i', 'd', ':', ' ', 'a', 0, 'b', 10, 'd', 'a', 't', 'a', ':', ' ', 'a', 10, 10};
    embi_sse_parser_feed(&ps, in, sizeof in);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&p.log), "[message|a|keep]"); /* the previous id stays */
    embi_sb_free(&p.log);
    embi_sse_parser_free(&ps);
}

EMB_TEST(parse_retry)
{
    const char *const c1[] = {"retry: 1500\ndata: a\n\n", nullptr};
    int n;
    uint32_t ms;
    char *got = run_vector(c1, false, &n, &ms, 1 << 20);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(ms, 1500);
    SDL_free(got);
    const char *const c2[] = {"retry: abc\nretry:\nretry: 12x\nretry: -5\nretry: 1 2\n\n", nullptr};
    got = run_vector(c2, true, &n, &ms, 1 << 20);
    EMB_CHECK_EQ_INT(n, 0); /* only digits count, and an empty value does not */
    SDL_free(got);
    const char *const c3[] = {"retry: 99999999999999999999\n", nullptr};
    got = run_vector(c3, false, &n, &ms, 1 << 20);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(ms <= 24u * 3600u * 1000u);
    SDL_free(got);
}

EMB_TEST(parse_limits_bound_memory)
{
    pv p = {0};
    embi_sse_parser ps;
    embi_sse_parser_init(&ps, 32, nullptr, pv_event, pv_retry, &p);
    const char *l = "data: this line is far longer than thirty-two bytes\n\n";
    embi_sse_parser_feed(&ps, (const uint8_t *)l, strlen(l));
    EMB_CHECK(ps.overflow);
    EMB_CHECK_EQ_INT(p.log.len, 0);
    embi_sb_free(&p.log);
    embi_sse_parser_free(&ps);
    /* many short data lines add up */
    embi_sse_parser_init(&ps, 32, nullptr, pv_event, pv_retry, &p);
    for (int i = 0; i < 10 && !ps.overflow; i++) {
        embi_sse_parser_feed(&ps, (const uint8_t *)"data: 0123456\n", 14);
    }
    EMB_CHECK(ps.overflow);
    embi_sb_free(&p.log);
    embi_sse_parser_free(&ps);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

/* ------------------------------------------------------------------ the connection */

typedef struct ssx {
    SDL_AtomicInt attempts;
    SDL_AtomicInt disconnects;
    SDL_Mutex *m;
    char ids[8][64]; /* Last-Event-ID of each attempt */
    char cookies[8][64];
    char accept[64], cache[64];
} ssx;

static void ssx_handler(ts_conn *c, const ts_req *r, void *user)
{
    ssx *x = user;
    const char *t = r->target;
    c->close = true;
    if (strncmp(t, "/redirect", 9) == 0) {
        ts_respond_str(c, 302, "Location: /basic\r\n", "");
        return;
    }
    if (strncmp(t, "/wrongtype", 10) == 0) {
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "data: x\n\n");
        return;
    }
    if (strncmp(t, "/notfound", 9) == 0) {
        ts_respond_str(c, 404, "Content-Type: text/event-stream\r\n", "data: x\n\n");
        return;
    }
    if (strncmp(t, "/nocontent", 10) == 0) {
        ts_respond_str(c, 204, "", "");
        return;
    }
    int attempt = SDL_AddAtomicInt(&x->attempts, 1) + 1;
    const char *v;
    SDL_LockMutex(x->m);
    if (attempt <= 8) {
        SDL_snprintf(x->ids[attempt - 1], sizeof x->ids[0], "%s", (v = ts_req_header(r, "last-event-id")) != nullptr ? v : "(none)");
        SDL_snprintf(x->cookies[attempt - 1], sizeof x->cookies[0], "%s", (v = ts_req_header(r, "cookie")) != nullptr ? v : "(none)");
    }
    SDL_snprintf(x->accept, sizeof x->accept, "%s", (v = ts_req_header(r, "accept")) != nullptr ? v : "(none)");
    SDL_snprintf(x->cache, sizeof x->cache, "%s", (v = ts_req_header(r, "cache-control")) != nullptr ? v : "(none)");
    SDL_UnlockMutex(x->m);
    const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nConnection: close\r\n\r\n";
    ts_conn_write(c, head, strlen(head));
    if (strncmp(t, "/basic", 6) == 0) {
        const char *body = "retry: 3000\nid: 1\ndata: one\n\nevent: custom\nid: 2\ndata: two\ndata: lines\n\n: keep\n\ndata: \xFF\n\n";
        ts_conn_write(c, body, strlen(body));
    } else if (strncmp(t, "/reconnect", 10) == 0) {
        if (attempt == 1) { /* an event, a half event, and the connection ends */
            const char *body = "retry: 40\nid: 7\ndata: first\n\ndata: half";
            ts_conn_write(c, body, strlen(body));
            return;
        }
        const char *body = attempt == 2 ? "data: second\n\ndata: never-closed" : "data: third\n\n";
        ts_conn_write(c, body, strlen(body));
        if (attempt == 2) {
            return; /* ends again: another reconnect */
        }
    } else if (strncmp(t, "/retry0", 7) == 0) { /* a hostile server: reconnect at once, forever */
        const char *body = "retry: 0\ndata: x\n\n";
        ts_conn_write(c, body, strlen(body));
        return;
    } else if (strncmp(t, "/slow", 5) == 0) {
        ts_conn_write(c, "data: before\n\n", 14);
        SDL_Delay(900);
        ts_conn_write(c, "data: after\n\n", 13);
    } else if (strncmp(t, "/bom", 4) == 0) {
        const char *body = "\xEF\xBB\xBF" "data: bom\n\n";
        ts_conn_write(c, body, strlen(body));
    } else {
        ts_conn_write(c, "data: hi\n\n", 10);
    }
    while (ts_conn_pull(c, 20) && !SDL_GetAtomicInt(&c->srv->stop)) {
    }
    SDL_AddAtomicInt(&x->disconnects, 1);
}

enum { SC_OPEN = 1, SC_MSG, SC_ERROR, SC_FATAL, SC_RELEASE };

typedef struct sc_ev {
    int kind;
    char type[40], data[200], id[40], origin[80], msg[200];
} sc_ev;

typedef struct sc {
    tq *q;
    sc_ev ev[64];
    int n;
    SDL_AtomicInt released;
    embi_sse *es;
    bool close_in_message; /* test hook: close() from inside the first message callback */
} sc;

static sc_ev *sc_add(sc *s, int kind)
{
    if (s->n >= 64) {
        return &s->ev[63];
    }
    sc_ev *e = &s->ev[s->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}

static void sc_open(void *u)
{
    sc_add(u, SC_OPEN);
}

static void sc_msg(void *u, const char *type, const char *data, size_t len, const char *id, const char *origin)
{
    (void)len;
    sc *s = u;
    sc_ev *e = sc_add(s, SC_MSG);
    SDL_snprintf(e->type, sizeof e->type, "%s", type);
    SDL_snprintf(e->data, sizeof e->data, "%s", data);
    SDL_snprintf(e->id, sizeof e->id, "%s", id);
    SDL_snprintf(e->origin, sizeof e->origin, "%s", origin);
    if (s->close_in_message && s->es != nullptr) {
        embi_sse_close(s->es);
        s->es = nullptr;
    }
}

static void sc_err(void *u, bool reconnecting, const char *msg)
{
    sc_ev *e = sc_add(u, reconnecting ? SC_ERROR : SC_FATAL);
    SDL_snprintf(e->msg, sizeof e->msg, "%s", msg);
}

static void sc_release(void *u)
{
    sc *s = u;
    SDL_SetAtomicInt(&s->released, 1);
}

static int sc_count(const sc *s, int kind)
{
    int c = 0;
    for (int i = 0; i < s->n; i++) {
        c += s->ev[i].kind == kind;
    }
    return c;
}

static const sc_ev *sc_nth(const sc *s, int kind, int nth)
{
    for (int i = 0; i < s->n; i++) {
        if (s->ev[i].kind == kind && nth-- == 0) {
            return &s->ev[i];
        }
    }
    return nullptr;
}

typedef struct sc_pred {
    sc *s;
    int kind, count;
} sc_pred;

static bool sc_pred_fn(void *a)
{
    sc_pred *p = a;
    return sc_count(p->s, p->kind) >= p->count;
}

static bool sc_wait(sc *s, int kind, int count, uint32_t ms)
{
    sc_pred p = {s, kind, count};
    return tq_wait(s->q, sc_pred_fn, &p, ms);
}

static bool sc_released_fn(void *a)
{
    return SDL_GetAtomicInt(&((sc *)a)->released) != 0;
}

typedef struct sts {
    ts_server *srv;
    ssx x;
    embi_net *net;
    tlog log;
    tq *q;
    char url[300];
} sts;

static bool sts_start(sts *s)
{
    memset(s, 0, sizeof *s);
    s->x.m = SDL_CreateMutex();
    s->net = tn_new(&s->log);
    s->srv = ts_start(ssx_handler, &s->x, nullptr, nullptr);
    s->q = tq_new();
    return s->srv != nullptr;
}

static void sts_stop(sts *s)
{
    ts_stop(s->srv);
    tn_free(s->net, &s->log);
    tq_free(s->q);
    SDL_DestroyMutex(s->x.m);
}

static const char *sts_url(sts *s, const char *path)
{
    SDL_snprintf(s->url, sizeof s->url, "http://127.0.0.1:%d%s", s->srv->port, path);
    return s->url;
}

static embi_sse *sts_open(sts *s, sc *c, const char *path, uint32_t retry_ms, bool creds, const char *initiator)
{
    memset(c, 0, sizeof *c);
    c->q = s->q;
    embi_sse_params p = {0};
    p.url = sts_url(s, path);
    p.with_credentials = creds;
    p.initiator_origin = initiator;
    p.retry_ms = retry_ms;
    p.cb = (embi_sse_callbacks){sc_open, sc_msg, sc_err, sc_release};
    p.user = c;
    p.post = tq_post;
    p.post_user = s->q;
    c->es = embi_sse_open(s->net, &p);
    return c->es;
}

EMB_TEST(events_types_ids_and_origin)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/basic", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 3, 8000));
    EMB_CHECK_EQ_INT(c.ev[0].kind, SC_OPEN);
    const sc_ev *e = sc_nth(&c, SC_MSG, 0);
    EMB_CHECK_EQ_STR(e->type, "message");
    EMB_CHECK_EQ_STR(e->data, "one");
    EMB_CHECK_EQ_STR(e->id, "1");
    char origin[80];
    SDL_snprintf(origin, sizeof origin, "http://127.0.0.1:%d", s.srv->port);
    EMB_CHECK_EQ_STR(e->origin, origin);
    e = sc_nth(&c, SC_MSG, 1);
    EMB_CHECK_EQ_STR(e->type, "custom");
    EMB_CHECK_EQ_STR(e->data, "two\nlines");
    EMB_CHECK_EQ_STR(e->id, "2");
    e = sc_nth(&c, SC_MSG, 2);
    EMB_CHECK_EQ_STR(e->data, "\xEF\xBF\xBD");
    EMB_CHECK_EQ_STR(e->id, "2");
    EMB_CHECK_EQ_STR(s.x.accept, "text/event-stream");
    EMB_CHECK_EQ_STR(s.x.cache, "no-store");
    EMB_CHECK_EQ_STR(s.x.ids[0], "(none)");
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(a_byte_order_mark_is_dropped_on_the_wire)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/bom", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 1, 8000));
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 0)->data, "bom");
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
}

EMB_TEST(reconnects_after_retry_with_last_event_id)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/reconnect", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 3, 10000));
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 0)->data, "first");
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 0)->id, "7");
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 1)->data, "second"); /* the half event of attempt 1 was dropped */
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 1)->id, "7");
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 2)->data, "third");
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.attempts), 3);
    EMB_CHECK_EQ_STR(s.x.ids[0], "(none)");
    EMB_CHECK_EQ_STR(s.x.ids[1], "7");
    EMB_CHECK_EQ_STR(s.x.ids[2], "7");
    /* open, message, error(reconnecting), open, message, error, open, message */
    EMB_CHECK_EQ_INT(sc_count(&c, SC_OPEN), 3);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_ERROR), 2);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_FATAL), 0);
    int kinds[] = {SC_OPEN, SC_MSG, SC_ERROR, SC_OPEN, SC_MSG, SC_ERROR, SC_OPEN, SC_MSG};
    for (int i = 0; i < 8; i++) {
        EMB_CHECK_EQ_INT(c.ev[i].kind, kinds[i]);
    }
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(retry_zero_is_floored_so_a_hostile_server_cannot_make_a_tight_loop)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/retry0", 0, false, nullptr) != nullptr);
    tq_run_for(s.q, 1200);
    int attempts = SDL_GetAtomicInt(&s.x.attempts);
    EMB_CHECK(attempts >= 2);
    EMB_CHECK(attempts <= 7); /* about one per 250 ms (the floor), not hundreds */
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(the_reconnection_delay_is_respected)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    uint64_t t0 = SDL_GetTicks();
    EMB_REQUIRE(sts_open(&s, &c, "/reconnect", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 3, 10000));
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_CHECK(dt >= 70); /* two reconnects after 40 ms each */
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
}

static void scenario_fatal(const char *path, const char *needle)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, path, 50, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_FATAL, 1, 8000));
    EMB_CHECK_EQ_INT(sc_count(&c, SC_OPEN), 0);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_MSG), 0);
    EMB_CHECK(strstr(sc_nth(&c, SC_FATAL, 0)->msg, needle) != nullptr);
    tq_run_for(s.q, 300); /* no reconnect follows */
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.attempts), path[1] == 'n' || path[1] == 'w' ? 0 : 1);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_FATAL), 1);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_ERROR), 0);
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(a_wrong_content_type_fails_the_connection) { scenario_fatal("/wrongtype", "MIME type"); }
EMB_TEST(a_404_fails_the_connection) { scenario_fatal("/notfound", "status 404"); }
EMB_TEST(a_204_fails_the_connection) { scenario_fatal("/nocontent", "status 204"); }

EMB_TEST(redirects_are_followed)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/redirect", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 1, 8000));
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 0)->data, "one");
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
}

EMB_TEST(a_host_off_the_allowlist_fails_the_connection)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    embi_net *n2 = embi_net_create(nullptr, nullptr);
    sc c;
    memset(&c, 0, sizeof c);
    c.q = s.q;
    embi_sse_params p = {0};
    p.url = sts_url(&s, "/basic");
    p.cb = (embi_sse_callbacks){sc_open, sc_msg, sc_err, sc_release};
    p.user = &c;
    p.post = tq_post;
    p.post_user = s.q;
    c.es = embi_sse_open(n2, &p);
    EMB_REQUIRE(c.es != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_FATAL, 1, 8000));
    EMB_CHECK(strstr(sc_nth(&c, SC_FATAL, 0)->msg, "allowlist") != nullptr);
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    embi_net_destroy(n2);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.attempts), 0);
    sts_stop(&s);
}

EMB_TEST(a_connection_refused_is_retried)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    memset(&c, 0, sizeof c);
    c.q = s.q;
    embi_sse_params p = {0};
    p.url = "http://127.0.0.1:65535/x";
    p.retry_ms = 50;
    p.cb = (embi_sse_callbacks){sc_open, sc_msg, sc_err, sc_release};
    p.user = &c;
    p.post = tq_post;
    p.post_user = s.q;
    c.es = embi_sse_open(s.net, &p);
    EMB_REQUIRE(sc_wait(&c, SC_ERROR, 3, 15000));
    EMB_CHECK_EQ_INT(sc_count(&c, SC_FATAL), 0);
    EMB_CHECK_EQ_INT(sc_count(&c, SC_OPEN), 0);
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(close_inside_a_message_callback_stops_everything)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    memset(&c, 0, sizeof c);
    c.q = s.q;
    c.close_in_message = true;
    embi_sse_params p = {0};
    p.url = sts_url(&s, "/basic");
    p.cb = (embi_sse_callbacks){sc_open, sc_msg, sc_err, sc_release};
    p.user = &c;
    p.post = tq_post;
    p.post_user = s.q;
    c.es = embi_sse_open(s.net, &p);
    EMB_REQUIRE(tq_wait(s.q, sc_released_fn, &c, 8000));
    EMB_CHECK_EQ_INT(sc_count(&c, SC_MSG), 1); /* the other two events of the same chunk are not delivered */
    for (int i = 0; i < 200 && SDL_GetAtomicInt(&s.x.disconnects) == 0; i++) {
        SDL_Delay(10);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.disconnects), 1);
    sts_stop(&s);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(close_during_the_reconnection_wait_stops_it)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/reconnect", 100000, false, nullptr) != nullptr); /* a long wait... */
    EMB_REQUIRE(sc_wait(&c, SC_ERROR, 1, 8000));                                     /* ...the stream ended once */
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.attempts), 1);
    sts_stop(&s);
}

/* initiator: 0 none, 1 the server's own origin, 2 another origin */
static void scenario_credentials(bool creds, int initiator, bool expect_cookie)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    embi_net_cookie_set(s.net, sts_url(&s, "/"), "sid=abc; Path=/");
    char origin[80];
    SDL_snprintf(origin, sizeof origin, "http://127.0.0.1:%d", s.srv->port);
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/basic", 0, creds, initiator == 0 ? nullptr : initiator == 1 ? origin : "app://page") != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 1, 8000));
    EMB_CHECK_EQ_STR(s.x.cookies[0], expect_cookie ? "sid=abc" : "(none)");
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
}

EMB_TEST(cookies_are_sent_to_the_same_origin) { scenario_credentials(false, 1, true); }
EMB_TEST(cookies_are_not_sent_without_an_initiator) { scenario_credentials(false, 0, false); }
EMB_TEST(cookies_are_not_sent_cross_origin_without_credentials) { scenario_credentials(false, 2, false); }
EMB_TEST(with_credentials_sends_cookies_cross_origin) { scenario_credentials(true, 2, true); }

EMB_TEST(a_quiet_stream_is_not_cut_by_the_idle_timeout)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    embi_net_set_timeouts(s.net, 0, 300);
    sc c;
    EMB_REQUIRE(sts_open(&s, &c, "/slow", 0, false, nullptr) != nullptr);
    EMB_REQUIRE(sc_wait(&c, SC_MSG, 2, 8000));
    EMB_CHECK_EQ_STR(sc_nth(&c, SC_MSG, 1)->data, "after");
    EMB_CHECK_EQ_INT(sc_count(&c, SC_ERROR), 0);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&s.x.attempts), 1);
    embi_sse_close(c.es);
    EMB_CHECK(tq_wait(s.q, sc_released_fn, &c, 5000));
    sts_stop(&s);
}

EMB_TEST(open_needs_a_post_function)
{
    sts s;
    EMB_REQUIRE(sts_start(&s));
    embi_sse_params p = {0};
    p.url = sts_url(&s, "/basic");
    EMB_CHECK(embi_sse_open(s.net, &p) == nullptr);
    sts_stop(&s);
}

EMB_TEST_MAIN()
