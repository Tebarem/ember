/*
 * HTTP/1.1 client tests against the 127.0.0.1 test server: requests and bodies, framing, keep-alive and the pool,
 * redirects, content decoding, cookies, the allowlist, timeouts, abort and shutdown with requests in flight.
 */
#include "net_test.h"
#include "net_vectors.h"

static SDL_AtomicInt g_seen_auth;        /* requests that carried an Authorization header */
static char g_last_method[16];
static char g_last_body[256];
static char g_last_cookie[256];
static char g_last_ua[128];
static char g_last_host[128];
static char g_last_accept_enc[128];
static char g_last_ctype[128];
static SDL_Mutex *g_m;

static void note(const ts_req *r)
{
    SDL_LockMutex(g_m);
    SDL_snprintf(g_last_method, sizeof g_last_method, "%s", r->method);
    SDL_snprintf(g_last_body, sizeof g_last_body, "%.*s", (int)(r->body_len < 255 ? r->body_len : 255), r->body ? (char *)r->body : "");
    const char *v = ts_req_header(r, "cookie");
    SDL_snprintf(g_last_cookie, sizeof g_last_cookie, "%s", v ? v : "(none)");
    v = ts_req_header(r, "user-agent");
    SDL_snprintf(g_last_ua, sizeof g_last_ua, "%s", v ? v : "(none)");
    v = ts_req_header(r, "host");
    SDL_snprintf(g_last_host, sizeof g_last_host, "%s", v ? v : "(none)");
    v = ts_req_header(r, "accept-encoding");
    SDL_snprintf(g_last_accept_enc, sizeof g_last_accept_enc, "%s", v ? v : "(none)");
    v = ts_req_header(r, "content-type");
    SDL_snprintf(g_last_ctype, sizeof g_last_ctype, "%s", v ? v : "(none)");
    SDL_UnlockMutex(g_m);
    if (ts_req_header(r, "authorization") != nullptr) {
        SDL_AddAtomicInt(&g_seen_auth, 1);
    }
}

static bool is(const ts_req *r, const char *target)
{
    return strcmp(r->target, target) == 0;
}

static bool starts(const ts_req *r, const char *prefix)
{
    return strncmp(r->target, prefix, strlen(prefix)) == 0;
}

static void handler(ts_conn *c, const ts_req *r, void *user)
{
    ts_server *s = user;
    note(r);
    char loc[300];
    if (is(r, "/hello")) {
        ts_respond_str(c, 200, "Content-Type: text/plain\r\nX-Test: yes\r\n", "hello world");
    } else if (is(r, "/echo")) {
        char buf[600];
        int n = SDL_snprintf(buf, sizeof buf, "%s|%.*s", r->method, (int)r->body_len, r->body ? (char *)r->body : "");
        ts_respond(c, 200, "", buf, (size_t)n);
    } else if (is(r, "/chunked")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        ts_printf(c, "5\r\nhello\r\n");
        SDL_Delay(30);
        ts_printf(c, "6;x=y\r\n world\r\n");
        SDL_Delay(30);
        ts_printf(c, "1\r\n!\r\n0\r\nTrailer: t\r\n\r\n");
    } else if (is(r, "/closedelim")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil close");
        c->close = true;
    } else if (is(r, "/close")) {
        ts_respond_str(c, 200, "Connection: close\r\n", "bye");
        c->close = true;
    } else if (is(r, "/silentclose")) { /* keep-alive promised, connection dropped right after */
        ts_respond_str(c, 200, "", "once");
        c->close = true;
    } else if (is(r, "/slow")) {
        SDL_Delay(200);
        ts_respond_str(c, 200, "", "slow");
    } else if (is(r, "/hang")) {
        while (!SDL_GetAtomicInt(&s->stop)) {
            SDL_Delay(10);
        }
    } else if (is(r, "/stall")) { /* head and ten body bytes, then silence */
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789");
        while (!SDL_GetAtomicInt(&s->stop)) {
            SDL_Delay(10);
        }
    } else if (is(r, "/stream")) { /* 32 MB, as fast as the connection takes it */
        size_t total = (size_t)32 << 20;
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", (unsigned)total);
        static uint8_t blk[65536];
        for (size_t i = 0; i < total && !SDL_GetAtomicInt(&s->stop); i += sizeof blk) {
            if (!ts_conn_write(c, blk, sizeof blk)) {
                break;
            }
        }
    } else if (is(r, "/bomb")) { /* a deflate body of 77 MB of zeros from about 490 KB */
        embi_buf z = {0};
        make_deflate_zeros(&z, 300000);
        ts_respond(c, 200, "Content-Encoding: deflate\r\n", z.data, z.len);
        embi_buf_free(&z);
    } else if (is(r, "/bigbr")) { /* a Brotli body whose compressed size is over the 16 MB input cap */
        size_t n = (size_t)17 << 20;
        uint8_t *b = SDL_calloc(1, n);
        ts_respond(c, 200, "Content-Encoding: br\r\n", b, n);
        SDL_free(b);
    } else if (is(r, "/drop")) { /* reads the request and hangs up without a response */
        c->close = true;
    } else if (is(r, "/trickle")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n");
        for (int i = 0; i < 400 && !SDL_GetAtomicInt(&s->stop); i++) {
            char blk[100];
            memset(blk, 'x', sizeof blk);
            if (!ts_conn_write(c, blk, sizeof blk)) {
                break;
            }
            SDL_Delay(5);
        }
        c->close = true;
    } else if (is(r, "/big")) {
        size_t n = 1 << 20;
        uint8_t *b = SDL_malloc(n);
        for (size_t i = 0; i < n; i++) {
            b[i] = (uint8_t)(i * 31 + 7);
        }
        ts_respond(c, 200, "", b, n);
        SDL_free(b);
    } else if (is(r, "/head")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nX-Head: 1\r\n\r\n"); /* no body, as for HEAD */
    } else if (is(r, "/204")) {
        ts_printf(c, "HTTP/1.1 204 No Content\r\n\r\n");
    } else if (is(r, "/304")) {
        ts_printf(c, "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n");
    } else if (is(r, "/interim")) {
        ts_printf(c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: x\r\n\r\n");
        ts_respond_str(c, 200, "", "after interim");
    } else if (is(r, "/badstatus")) {
        ts_printf(c, "FTP/1.0 what\r\n\r\n");
        c->close = true;
    } else if (is(r, "/short")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly ten b");
        c->close = true;
    } else if (is(r, "/badchunk")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
        c->close = true;
    } else if (is(r, "/gzip")) {
        ts_respond(c, 200, "Content-Encoding: gzip\r\n", net_vec_gzip, sizeof net_vec_gzip);
    } else if (is(r, "/gzip-chunked")) {
        ts_printf(c, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n");
        size_t half = sizeof net_vec_gzip / 2;
        ts_printf(c, "%x\r\n", (unsigned)half);
        ts_conn_write(c, net_vec_gzip, half);
        ts_printf(c, "\r\n%x\r\n", (unsigned)(sizeof net_vec_gzip - half));
        ts_conn_write(c, net_vec_gzip + half, sizeof net_vec_gzip - half);
        ts_printf(c, "\r\n0\r\n\r\n");
    } else if (is(r, "/deflate")) {
        ts_respond(c, 200, "Content-Encoding: deflate\r\n", net_vec_zlib, sizeof net_vec_zlib);
    } else if (is(r, "/br")) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s/tests/data/brotli/10x10y.compressed", EMBER_SOURCE_DIR);
        size_t n = 0;
        void *d = SDL_LoadFile(path, &n);
        ts_respond(c, 200, "Content-Encoding: br\r\n", d, n);
        SDL_free(d);
    } else if (is(r, "/badgzip")) {
        ts_respond(c, 200, "Content-Encoding: gzip\r\n", net_vec_gzip_bad_crc, sizeof net_vec_gzip_bad_crc);
    } else if (is(r, "/unknownenc")) {
        ts_respond_str(c, 200, "Content-Encoding: compress\r\n", "xx");
    } else if (starts(r, "/redir/")) { /* /redir/<status>/<target> */
        int status = atoi(r->target + 7);
        const char *t = strchr(r->target + 7, '/');
        SDL_snprintf(loc, sizeof loc, "Location: %s\r\n", t != nullptr ? t : "/hello");
        ts_respond_str(c, status, loc, "redirect body");
    } else if (is(r, "/loop")) {
        ts_respond_str(c, 302, "Location: /loop\r\n", "");
    } else if (is(r, "/redir-abs-localhost")) {
        SDL_snprintf(loc, sizeof loc, "Location: http://localhost:%d/echo\r\n", s->port);
        ts_respond_str(c, 307, loc, "");
    } else if (is(r, "/redir-blocked")) {
        ts_respond_str(c, 302, "Location: http://blocked.test:1/x\r\n", "");
    } else if (is(r, "/redir-ftp")) {
        ts_respond_str(c, 302, "Location: ftp://127.0.0.1/x\r\n", "");
    } else if (is(r, "/redir-nolocation")) {
        ts_respond_str(c, 302, "", "no location here");
    } else if (is(r, "/setcookie")) {
        ts_respond_str(c, 200, "Set-Cookie: a=1; Path=/\r\nSet-Cookie: b=2; Path=/only\r\nSet-Cookie: c=3; Secure; Path=/\r\n"
                               "Set-Cookie: h=1; HttpOnly; Path=/\r\n",
                       "set");
    } else if (is(r, "/cookie-check")) {
        ts_respond_str(c, 200, "", "ok");
    } else if (is(r, "/only/cookie-check")) {
        ts_respond_str(c, 200, "", "ok");
    } else {
        ts_respond_str(c, 404, "", "not found");
    }
}

static embi_net *g_net;
static tlog g_log;
static ts_server *g_srv;
static char g_url[256];

static const char *url_for(const char *path)
{
    ts_url(g_srv, g_url, sizeof g_url, path);
    return g_url;
}

static void setup(void)
{
    g_m = g_m != nullptr ? g_m : SDL_CreateMutex();
    memset(&g_log, 0, sizeof g_log);
    g_net = tn_new(&g_log);
    g_srv = ts_start(handler, nullptr, nullptr, nullptr);
    if (g_srv != nullptr) {
        g_srv->user = g_srv;
    }
}

static void teardown(void)
{
    ts_stop(g_srv);
    tn_free(g_net, &g_log);
    g_srv = nullptr;
    g_net = nullptr;
}

static void fetch(tc *t, const char *method, const char *path)
{
    embi_net_request r = tc_request(t, method, url_for(path));
    EMB_CHECK(tc_run(g_net, t, &r, 10000));
}

/* ------------------------------------------------------------------ requests */

EMB_TEST(http_get_basic)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK_EQ_INT(t.status, 200);
    EMB_CHECK_EQ_STR(t.status_text, "Status");
    EMB_CHECK(tc_body_is(&t, "hello world"));
    EMB_CHECK(tc_hdr_is(&t, "content-type", "text/plain"));
    EMB_CHECK(tc_hdr_is(&t, "x-test", "yes"));
    EMB_CHECK_EQ_INT(t.content_length, 11);
    EMB_CHECK(!t.redirected);
    EMB_CHECK(!t.tls);
    EMB_CHECK(t.order_ok && t.order == 4);
    EMB_CHECK_EQ_STR(t.url, url_for("/hello"));
    EMB_CHECK_EQ_STR(g_last_method, "GET");
    EMB_CHECK(strncmp(g_last_ua, "Ember/", 6) == 0);
    EMB_CHECK_EQ_STR(g_last_accept_enc, "gzip, deflate, br");
    char host[64];
    SDL_snprintf(host, sizeof host, "127.0.0.1:%d", g_srv->port);
    EMB_CHECK_EQ_STR(g_last_host, host);
    tc_free(&t);
    teardown();
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(http_user_agent_and_headers)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    embi_net_set_user_agent(g_net, "MyApp/9");
    tc t;
    tc_init(&t);
    embi_headers h = {0};
    embi_headers_add_raw(&h, EMBI_STR("X-Custom"), EMBI_STR("v1"));
    embi_headers_add_raw(&h, EMBI_STR("Cookie"), EMBI_STR("ignored=1"));
    embi_net_request r = tc_request(&t, "GET", url_for("/hello"));
    r.headers = &h;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_STR(g_last_ua, "MyApp/9");
    EMB_CHECK_EQ_STR(g_last_cookie, "(none)"); /* a caller cannot set Cookie */
    embi_headers_free(&h);
    tc_free(&t);
    teardown();
}

EMB_TEST(http_post_and_methods)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    const char *methods[] = {"POST", "PUT", "PATCH", "DELETE"};
    for (int i = 0; i < 4; i++) {
        tc t;
        tc_init(&t);
        embi_headers h = {0};
        embi_headers_add_raw(&h, EMBI_STR("Content-Type"), EMBI_STR("text/x-test"));
        embi_net_request r = tc_request(&t, methods[i], url_for("/echo"));
        r.headers = &h;
        r.body = "payload=1";
        r.body_len = 9;
        EMB_CHECK(tc_run(g_net, &t, &r, 10000));
        char expect[64];
        SDL_snprintf(expect, sizeof expect, "%s|payload=1", methods[i]);
        EMB_CHECK(tc_body_is(&t, expect));
        EMB_CHECK_EQ_STR(g_last_ctype, "text/x-test");
        embi_headers_free(&h);
        tc_free(&t);
    }
    /* An empty POST still carries Content-Length: 0. */
    tc t;
    tc_init(&t);
    fetch(&t, "POST", "/echo");
    EMB_CHECK(tc_body_is(&t, "POST|"));
    tc_free(&t);
    teardown();
}

EMB_TEST(http_head_204_304_interim)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "HEAD", "/head");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 200);
    EMB_CHECK_EQ_INT(t.body.len, 0);
    EMB_CHECK(tc_hdr_is(&t, "x-head", "1"));
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/204");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 204);
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/304");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 304);
    EMB_CHECK_EQ_INT(t.body.len, 0);
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/interim");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 200);
    EMB_CHECK(tc_body_is(&t, "after interim"));
    tc_free(&t);
    teardown();
}

EMB_TEST(http_localhost_resolves_to_loopback_v4)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    char url[128];
    SDL_snprintf(url, sizeof url, "http://localhost:%d/hello", g_srv->port);
    embi_net_request r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK(t.ended);
    EMB_CHECK(tc_body_is(&t, "hello world"));
    tc_free(&t);
    teardown();
}

/* ------------------------------------------------------------------ framing */

EMB_TEST(http_chunked_response)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/chunked");
    EMB_CHECK(t.ended);
    EMB_CHECK(tc_body_is(&t, "hello world!"));
    EMB_CHECK(t.data_events >= 2); /* the body streams as it arrives */
    EMB_CHECK_EQ_INT(t.content_length, -1);
    tc_free(&t);
    teardown();
}

EMB_TEST(http_close_delimited_body)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/closedelim");
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK(tc_body_is(&t, "until close"));
    tc_free(&t);
    teardown();
}

EMB_TEST(http_large_body)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/big");
    EMB_CHECK(t.ended);
    EMB_REQUIRE(t.body.len == (1u << 20));
    bool ok = true;
    for (size_t i = 0; i < t.body.len && ok; i++) {
        ok = t.body.data[i] == (uint8_t)(i * 31 + 7);
    }
    EMB_CHECK(ok);
    tc_free(&t);
    teardown();
}

EMB_TEST(http_protocol_errors)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    const char *paths[] = {"/badstatus", "/short", "/badchunk"};
    for (int i = 0; i < 3; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", paths[i]);
        EMB_CHECK(t.errored && !t.ended);
        EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_PROTOCOL);
        EMB_CHECK(t.order_ok && t.order == 4);
        tc_free(&t);
    }
    teardown();
}

/* ------------------------------------------------------------------ keep-alive and the pool */

EMB_TEST(http_keep_alive_reuses_connection)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    for (int i = 0; i < 5; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", i % 2 ? "/hello" : "/chunked");
        EMB_CHECK(t.ended);
        tc_free(&t);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->accepted), 1);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), 5);
    teardown();
}

EMB_TEST(http_connection_close_is_not_reused)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    for (int i = 0; i < 3; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", "/close");
        EMB_CHECK(t.ended);
        EMB_CHECK(tc_body_is(&t, "bye"));
        tc_free(&t);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->accepted), 3);
    teardown();
}

EMB_TEST(http_stale_keep_alive_connection_is_retried)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/silentclose"); /* promises keep-alive, then hangs up */
    EMB_CHECK(t.ended);
    tc_free(&t);
    SDL_Delay(100);
    tc_init(&t);
    fetch(&t, "GET", "/hello"); /* the pooled connection is dead: a new one is used */
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK(tc_body_is(&t, "hello world"));
    tc_free(&t);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->accepted), 2);
    teardown();
}

EMB_TEST(http_pool_limit_is_six_per_origin)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    enum { N = 14 };
    tc t[N];
    embi_net_req *reqs[N];
    for (int i = 0; i < N; i++) {
        tc_init(&t[i]);
        embi_net_request r = tc_request(&t[i], "GET", url_for("/slow"));
        reqs[i] = embi_net_start(g_net, &r);
    }
    for (int i = 0; i < N; i++) {
        EMB_CHECK(tc_wait_done(&t[i], 20000));
        EMB_CHECK(t[i].ended);
        EMB_CHECK(tc_body_is(&t[i], "slow"));
        EMB_CHECK(tc_wait_released(&t[i], 5000)); /* the release callback still locks the mutex tc_free destroys */
        embi_net_req_unref(reqs[i]);
        tc_free(&t[i]);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->max_live), EMBI_NET_MAX_PER_ORIGIN);
    EMB_CHECK(SDL_GetAtomicInt(&g_srv->accepted) <= EMBI_NET_MAX_PER_ORIGIN);
    teardown();
}

/* ------------------------------------------------------------------ redirects */

EMB_TEST(redirect_follow_get)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    int statuses[] = {301, 302, 303, 307, 308};
    for (int i = 0; i < 5; i++) {
        tc t;
        tc_init(&t);
        char path[64];
        SDL_snprintf(path, sizeof path, "/redir/%d/hello", statuses[i]);
        fetch(&t, "GET", path);
        EMB_CHECK(t.ended);
        EMB_CHECK_EQ_INT(t.status, 200);
        EMB_CHECK(tc_body_is(&t, "hello world"));
        EMB_CHECK(t.redirected);
        EMB_CHECK_EQ_INT(t.redirects, 1);
        EMB_CHECK_EQ_STR(t.url, url_for("/hello"));
        tc_free(&t);
    }
    teardown();
}

EMB_TEST(redirect_method_and_body_rules)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    struct {
        int status;
        const char *method;
        const char *expect;
    } cases[] = {
        {301, "POST", "GET|"},         {302, "POST", "GET|"},         {303, "POST", "GET|"},
        {307, "POST", "POST|data"},    {308, "POST", "POST|data"},    {303, "PUT", "GET|"},
        {301, "PUT", "PUT|data"},      {302, "DELETE", "DELETE|data"}, {307, "PUT", "PUT|data"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tc t;
        tc_init(&t);
        char path[64];
        SDL_snprintf(path, sizeof path, "/redir/%d/echo", cases[i].status);
        embi_headers h = {0};
        embi_headers_add_raw(&h, EMBI_STR("Content-Type"), EMBI_STR("text/x-test"));
        embi_net_request r = tc_request(&t, cases[i].method, url_for(path));
        r.headers = &h;
        r.body = "data";
        r.body_len = 4;
        EMB_CHECK(tc_run(g_net, &t, &r, 10000));
        if (!tc_body_is(&t, cases[i].expect)) {
            SDL_Log("    case %d %s: got '%.*s'", cases[i].status, cases[i].method, (int)t.body.len, (char *)t.body.data);
        }
        EMB_CHECK(tc_body_is(&t, cases[i].expect));
        /* A converted request lost its body headers; a kept one still has them. */
        EMB_CHECK_EQ_STR(g_last_ctype, strncmp(cases[i].expect, "GET", 3) == 0 ? "(none)" : "text/x-test");
        embi_headers_free(&h);
        tc_free(&t);
    }
    teardown();
}

EMB_TEST(redirect_modes)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/redir/302/hello"));
    r.redirect = EMBI_NET_REDIRECT_MANUAL;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 302);
    EMB_CHECK(tc_hdr_is(&t, "location", "/hello"));
    EMB_CHECK(!t.redirected);
    tc_free(&t);

    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/redir/302/hello"));
    r.redirect = EMBI_NET_REDIRECT_ERROR;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK(t.errored && !t.got_head);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_REDIRECT);
    tc_free(&t);

    /* A redirect status without Location is a normal response. */
    tc_init(&t);
    fetch(&t, "GET", "/redir-nolocation");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.status, 302);
    EMB_CHECK(tc_body_is(&t, "no location here"));
    tc_free(&t);
    teardown();
}

EMB_TEST(redirect_limit_and_bad_targets)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/loop");
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_REDIRECT);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), 21); /* the first request and 20 redirects */
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/redir-ftp");
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_REDIRECT);
    tc_free(&t);
    teardown();
}

EMB_TEST(redirect_cross_origin_drops_authorization)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    SDL_SetAtomicInt(&g_seen_auth, 0);
    tc t;
    tc_init(&t);
    embi_headers h = {0};
    embi_headers_add_raw(&h, EMBI_STR("Authorization"), EMBI_STR("Bearer x"));
    /* Same origin: kept. */
    embi_net_request r = tc_request(&t, "GET", url_for("/redir/302/hello"));
    r.headers = &h;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_seen_auth), 2);
    tc_free(&t);
    /* To localhost (another origin, the same server): the second hop has no Authorization. */
    SDL_SetAtomicInt(&g_seen_auth, 0);
    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/redir-abs-localhost"));
    r.headers = &h;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_seen_auth), 1);
    embi_headers_free(&h);
    tc_free(&t);
    teardown();
}

EMB_TEST(redirect_to_disallowed_host_is_blocked)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/redir-blocked");
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_BLOCKED);
    EMB_CHECK(strstr(t.msg, "blocked.test") != nullptr);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_log.warns), 1);
    EMB_CHECK(strstr(g_log.last, "blocked.test") != nullptr);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), 1); /* only the first hop reached a server */
    tc_free(&t);
    teardown();
}

/* ------------------------------------------------------------------ decoding */

static bool is_fox_text(const tc *t)
{
    const char *unit = NET_VEC_TEXT;
    size_t ul = strlen(unit);
    if (t->body.len != ul * 20) {
        return false;
    }
    for (int i = 0; i < 20; i++) {
        if (memcmp(t->body.data + i * ul, unit, ul) != 0) {
            return false;
        }
    }
    return true;
}

EMB_TEST(decode_content_encodings)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    const char *paths[] = {"/gzip", "/gzip-chunked", "/deflate"};
    for (int i = 0; i < 3; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", paths[i]);
        EMB_CHECK(t.ended);
        EMB_CHECK(is_fox_text(&t));
        EMB_CHECK_EQ_INT(t.content_length, -1); /* the length of an encoded body says nothing about the decoded one */
        tc_free(&t);
    }
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/br");
    EMB_CHECK(t.ended);
    EMB_CHECK_EQ_INT(t.body.len, 20);
    tc_free(&t);
    teardown();
}

EMB_TEST(decode_errors)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    const char *paths[] = {"/badgzip", "/unknownenc"};
    for (int i = 0; i < 2; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", paths[i]);
        EMB_CHECK(t.errored);
        EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_DECODE);
        tc_free(&t);
    }
    teardown();
}

/* ------------------------------------------------------------------ cookies */

EMB_TEST(cookies_set_and_send)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/setcookie");
    EMB_CHECK(t.ended);
    tc_free(&t);

    tc_init(&t);
    fetch(&t, "GET", "/cookie-check");
    EMB_CHECK_EQ_STR(g_last_cookie, "a=1; h=1"); /* Secure c not over http; b is for /only */
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/only/cookie-check");
    EMB_CHECK_EQ_STR(g_last_cookie, "b=2; a=1; h=1");
    tc_free(&t);

    /* credentials: omit sends nothing and stores nothing. */
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/cookie-check"));
    r.creds = EMBI_NET_CREDS_OMIT;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_STR(g_last_cookie, "(none)");
    tc_free(&t);

    /* same-origin sends only when the initiator is the request's origin. */
    char origin[64];
    SDL_snprintf(origin, sizeof origin, "http://127.0.0.1:%d", g_srv->port);
    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/cookie-check"));
    r.creds = EMBI_NET_CREDS_SAME_ORIGIN;
    r.initiator_origin = origin;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_STR(g_last_cookie, "a=1; h=1");
    tc_free(&t);
    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/cookie-check"));
    r.creds = EMBI_NET_CREDS_SAME_ORIGIN;
    r.initiator_origin = "app://local";
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_STR(g_last_cookie, "(none)");
    tc_free(&t);

    /* document.cookie sees no HttpOnly cookie and can set its own. */
    embi_sb sb = {0};
    embi_net_cookie_get(g_net, url_for("/"), &sb);
    EMB_CHECK_EQ_INT(sb.len, 3);
    EMB_CHECK(sb.len == 3 && memcmp(sb.data, "a=1", 3) == 0);
    embi_sb_free(&sb);
    embi_net_cookie_set(g_net, url_for("/"), "doc=9");
    tc_init(&t);
    fetch(&t, "GET", "/cookie-check");
    EMB_CHECK_EQ_STR(g_last_cookie, "a=1; h=1; doc=9");
    tc_free(&t);
    teardown();
}

EMB_TEST(cookies_not_stored_with_omit)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/setcookie"));
    r.creds = EMBI_NET_CREDS_OMIT;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/cookie-check");
    EMB_CHECK_EQ_STR(g_last_cookie, "(none)");
    tc_free(&t);
    teardown();
}

/* ------------------------------------------------------------------ allowlist */

EMB_TEST(allowlist_blocks_before_any_connection)
{
    SDL_AtomicInt dummy = {0};
    (void)dummy;
    g_m = g_m != nullptr ? g_m : SDL_CreateMutex();
    memset(&g_log, 0, sizeof g_log);
    g_log.m = SDL_CreateMutex();
    embi_net *n = embi_net_create(tlog_fn, &g_log); /* nothing allowed yet */
    g_srv = ts_start(handler, nullptr, nullptr, nullptr);
    EMB_REQUIRE(g_srv != nullptr);
    g_srv->user = g_srv;
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/hello"));
    EMB_CHECK(tc_run(n, &t, &r, 10000));
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_BLOCKED);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->accepted), 0);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_log.warns), 1);
    EMB_CHECK(strstr(g_log.last, "127.0.0.1") != nullptr);
    tc_free(&t);

    char pat[64];
    /* host:port with another port: still blocked. */
    SDL_snprintf(pat, sizeof pat, "127.0.0.1:%d", g_srv->port + 1);
    EMB_CHECK(embi_net_allow_add(n, pat));
    EMB_CHECK(embi_net_allow_add(n, "*.example.com"));
    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/hello"));
    EMB_CHECK(tc_run(n, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_BLOCKED);
    tc_free(&t);

    /* The right host:port. */
    SDL_snprintf(pat, sizeof pat, "127.0.0.1:%d", g_srv->port);
    EMB_CHECK(embi_net_allow_add(n, pat));
    tc_init(&t);
    r = tc_request(&t, "GET", url_for("/hello"));
    EMB_CHECK(tc_run(n, &t, &r, 10000));
    EMB_CHECK(t.ended);
    tc_free(&t);

    /* localhost is another host name: blocked until allowed (wildcard * allows anything). */
    char url[128];
    SDL_snprintf(url, sizeof url, "http://localhost:%d/hello", g_srv->port);
    tc_init(&t);
    r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(n, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_BLOCKED);
    tc_free(&t);
    EMB_CHECK(embi_net_allow_add(n, "*"));
    tc_init(&t);
    r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(n, &t, &r, 10000));
    EMB_CHECK(t.ended);
    tc_free(&t);

    EMB_CHECK(!embi_net_allow_add(n, "bad pattern"));
    ts_stop(g_srv);
    embi_net_destroy(n);
    SDL_DestroyMutex(g_log.m);
    g_srv = nullptr;
}

EMB_TEST(bad_requests_and_connect_errors)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    embi_net_allow_add(g_net, "*");
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", "not a url");
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_URL);
    tc_free(&t);
    tc_init(&t);
    r = tc_request(&t, "GET", "ftp://127.0.0.1/x");
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_URL);
    tc_free(&t);
    tc_init(&t);
    r = tc_request(&t, "BAD METHOD", url_for("/hello"));
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_URL);
    tc_free(&t);
    /* Nothing listens on the port of a stopped server. */
    char url[128];
    int port = g_srv->port;
    ts_stop(g_srv);
    g_srv = nullptr;
    SDL_snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    tc_init(&t);
    r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_CONNECT);
    tc_free(&t);
    tn_free(g_net, &g_log);
}

/* ------------------------------------------------------------------ timeouts, abort, shutdown */

EMB_TEST(timeout_idle_while_waiting_for_the_response)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    embi_net_set_timeouts(g_net, 0, 300);
    tc t;
    tc_init(&t);
    uint64_t t0 = SDL_GetTicks();
    fetch(&t, "GET", "/hang");
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TIMEOUT);
    EMB_CHECK(dt >= 250 && dt < 5000);
    tc_free(&t);
    teardown();
}


EMB_TEST(timeout_idle_in_the_middle_of_a_body)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    embi_net_set_timeouts(g_net, 0, 300);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/stall");
    EMB_CHECK(t.got_head);
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TIMEOUT);
    EMB_CHECK(tc_body_is(&t, "0123456789"));
    tc_free(&t);
    teardown();
}

EMB_TEST(timeout_total_request_time)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/trickle")); /* bytes keep coming, but too slowly overall */
    r.timeout_ms = 400;
    EMB_CHECK(tc_run(g_net, &t, &r, 10000));
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TIMEOUT);
    EMB_CHECK(t.got_head);
    tc_free(&t);
    teardown();
}

EMB_TEST(abort_in_the_middle_of_a_body)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    t.abort_after_data = true;
    embi_net_request r = tc_request(&t, "GET", url_for("/trickle"));
    t.req = embi_net_start(g_net, &r);
    EMB_CHECK(tc_wait_released(&t, 10000));
    embi_net_abort(t.req); /* idempotent */
    embi_net_req_unref(t.req);
    EMB_CHECK(t.got_head);
    EMB_CHECK(!t.ended);
    EMB_CHECK(t.body.len < 100000);
    EMB_CHECK(t.order_ok);
    tc_free(&t);
    teardown();
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(abort_before_the_response)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/hang"));
    t.req = embi_net_start(g_net, &r);
    SDL_Delay(100);
    embi_net_abort(t.req);
    EMB_CHECK(tc_wait_released(&t, 5000));
    EMB_CHECK(!t.got_head && !t.ended);
    embi_net_req_unref(t.req);
    tc_free(&t);
    /* The service still works. */
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.ended);
    tc_free(&t);
    teardown();
}

EMB_TEST(destroy_with_requests_in_flight)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    enum { N = 8 };
    tc t[N];
    embi_net_req *reqs[N];
    for (int i = 0; i < N; i++) {
        tc_init(&t[i]);
        embi_net_request r = tc_request(&t[i], "GET", url_for(i % 2 ? "/hang" : "/trickle"));
        reqs[i] = embi_net_start(g_net, &r);
    }
    SDL_Delay(150);
    tn_free(g_net, &g_log); /* aborts them all */
    g_net = nullptr;
    for (int i = 0; i < N; i++) {
        EMB_CHECK(tc_wait_released(&t[i], 1000));
        EMB_CHECK(!t[i].ended);
        EMB_CHECK(t[i].order_ok);
        embi_net_abort(reqs[i]); /* a handle outlives the service */
        embi_net_req_unref(reqs[i]);
        tc_free(&t[i]);
    }
    ts_stop(g_srv);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(destroy_without_ever_starting)
{
    memset(&g_log, 0, sizeof g_log);
    embi_net *n = tn_new(&g_log);
    tn_free(n, &g_log);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

/* ------------------------------------------------------------------ post mode */

typedef struct pq_item {
    void (*fn)(void *);
    void (*drop)(void *);
    void *arg;
    struct pq_item *next;
} pq_item;

typedef struct pq {
    SDL_Mutex *m;
    SDL_Condition *cv;
    pq_item *head, *tail;
    SDL_ThreadID poster;
    void (*sample)(void);
} pq;

static bool pq_post(void *u, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    pq *q = u;
    pq_item *it = SDL_calloc(1, sizeof *it);
    it->fn = fn;
    it->drop = drop;
    it->arg = arg;
    SDL_LockMutex(q->m);
    q->poster = SDL_GetCurrentThreadID();
    if (q->sample != nullptr) {
        q->sample();
    }
    if (q->tail != nullptr) {
        q->tail->next = it;
    } else {
        q->head = it;
    }
    q->tail = it;
    SDL_BroadcastCondition(q->cv);
    SDL_UnlockMutex(q->m);
    return true;
}

/* Runs queued items on this thread until pred is true or the time is up. */
static int pq_pump(pq *q, tc *t, uint32_t ms, SDL_ThreadID *ran_on)
{
    uint64_t end = SDL_GetTicks() + ms;
    int ran = 0;
    while (SDL_GetTicks() < end) {
        SDL_LockMutex(q->m);
        pq_item *it = q->head;
        if (it == nullptr) {
            SDL_WaitConditionTimeout(q->cv, q->m, 20);
            it = q->head;
        }
        if (it != nullptr) {
            q->head = it->next;
            if (q->head == nullptr) {
                q->tail = nullptr;
            }
        }
        SDL_UnlockMutex(q->m);
        if (it != nullptr) {
            *ran_on = SDL_GetCurrentThreadID();
            it->fn(it->arg);
            SDL_free(it);
            ran++;
        }
        if (t->released) {
            break;
        }
    }
    return ran;
}

EMB_TEST(post_mode_delivers_in_order_on_the_receiving_thread)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    pq q = {0};
    q.m = SDL_CreateMutex();
    q.cv = SDL_CreateCondition();
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url_for("/chunked"));
    r.post = pq_post;
    r.post_user = &q;
    t.req = embi_net_start(g_net, &r);
    SDL_ThreadID ran_on = 0;
    int ran = pq_pump(&q, &t, 10000, &ran_on);
    EMB_CHECK(ran >= 4);
    EMB_CHECK(t.released && t.ended);
    EMB_CHECK(t.order_ok);
    EMB_CHECK(tc_body_is(&t, "hello world!"));
    EMB_CHECK(ran_on == SDL_GetCurrentThreadID());
    EMB_CHECK(q.poster != SDL_GetCurrentThreadID()); /* posted from the network thread */
    embi_net_req_unref(t.req);
    tc_free(&t);
    SDL_DestroyCondition(q.cv);
    SDL_DestroyMutex(q.m);
    teardown();
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

/* ------------------------------------------------------------------ memory bounds (review findings) */

typedef struct mem_run {
    size_t base, peak, total;
} mem_run;

static mem_run g_mem;

static void mem_sample(void)
{
    size_t live = embi_mem_live();
    if (live > g_mem.peak) {
        g_mem.peak = live;
    }
}

/* Runs a post-mode request whose consumer does not run for hold_ms, then drains it. Returns the bytes received. */
static size_t slow_consumer_run(const char *path, uint32_t hold_ms, tc *t, embi_net_err *err)
{
    pq q = {0};
    q.m = SDL_CreateMutex();
    q.cv = SDL_CreateCondition();
    q.sample = mem_sample;
    g_mem = (mem_run){.base = embi_mem_live()};
    g_mem.peak = g_mem.base;
    embi_net_request r = tc_request(t, "GET", url_for(path));
    r.post = pq_post;
    r.post_user = &q;
    t->req = embi_net_start(g_net, &r);
    SDL_Delay(hold_ms); /* the consumer is busy: nothing is delivered meanwhile */
    SDL_ThreadID ran_on = 0;
    pq_pump(&q, t, 60000, &ran_on);
    *err = t->err;
    embi_net_req_unref(t->req);
    SDL_DestroyCondition(q.cv);
    SDL_DestroyMutex(q.m);
    return t->body.len;
}

EMB_TEST(backpressure_bounds_memory_for_a_slow_consumer)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    t.drop_body = true; /* count the bytes without keeping them */
    embi_net_err err = 0;
    (void)slow_consumer_run("/stream", 1500, &t, &err);
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK_EQ_INT(t.body_total, (uint64_t)32 << 20);
    /* Without backpressure 32 MB would pile up in the queue; with it about 4 MB (plus a decode step). */
    SDL_Log("    peak held above baseline: %u KB", (unsigned)((g_mem.peak - g_mem.base) >> 10));
    EMB_CHECK(g_mem.peak - g_mem.base < ((size_t)12 << 20));
    tc_free(&t);
    teardown();
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(decompression_bomb_stays_bounded)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    t.drop_body = true;
    embi_net_err err = 0;
    (void)slow_consumer_run("/bomb", 1000, &t, &err);
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK_EQ_INT(t.body_total, 1 + (uint64_t)300000 * 258);
    EMB_CHECK(g_mem.peak - g_mem.base < ((size_t)12 << 20)); /* 77 MB of output, never more than a few MB held */
    tc_free(&t);
    teardown();
}

EMB_TEST(brotli_input_over_the_cap_is_an_error)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    tc t;
    tc_init(&t);
    t.drop_body = true;
    fetch(&t, "GET", "/bigbr");
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_DECODE);
    tc_free(&t);
    teardown();
}

/* ------------------------------------------------------------------ retries and pool */

EMB_TEST(stale_connection_retry_only_for_idempotent_methods)
{
    const char *methods[] = {"GET", "PUT", "POST", "PATCH"};
    const int expect_requests[] = {3, 3, 2, 2}; /* the first request, then one try (and for idempotent ones one retry) */
    for (int i = 0; i < 4; i++) {
        setup();
        EMB_REQUIRE(g_srv != nullptr);
        tc t;
        tc_init(&t);
        fetch(&t, "GET", "/hello"); /* the connection is now pooled and used */
        EMB_CHECK(t.ended);
        tc_free(&t);
        tc_init(&t);
        embi_net_request r = tc_request(&t, methods[i], url_for("/drop"));
        r.body = "x";
        r.body_len = 1;
        EMB_CHECK(tc_run(g_net, &t, &r, 10000));
        EMB_CHECK(t.errored);
        if (SDL_GetAtomicInt(&g_srv->requests) != expect_requests[i]) {
            SDL_Log("    %s: %d server requests, expected %d", methods[i], SDL_GetAtomicInt(&g_srv->requests), expect_requests[i]);
        }
        EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), expect_requests[i]);
        tc_free(&t);
        teardown();
    }
}

EMB_TEST(idle_pooled_connections_are_closed_without_further_requests)
{
    setup();
    EMB_REQUIRE(g_srv != nullptr);
    embi_net_set_timeouts(g_net, 0, 300);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.ended);
    tc_free(&t);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->live), 1); /* pooled */
    uint64_t t0 = SDL_GetTicks();
    while (SDL_GetAtomicInt(&g_srv->live) != 0 && SDL_GetTicks() - t0 < 5000) {
        SDL_Delay(20);
    }
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->live), 0); /* the network thread closed it with no job to wake it */
    EMB_CHECK(SDL_GetTicks() - t0 >= 150);
    teardown();
}

EMB_TEST_MAIN()
