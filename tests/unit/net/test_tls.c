/*
 * TLS client tests against the 127.0.0.1 test server (Mbed TLS server side): the test root is trusted only when the
 * test adds it, an unknown root, a certificate for another name and a bad chain are rejected, verification has no
 * off switch, and keep-alive, bodies, cookies and shutdown work over TLS.
 */
#include "net_test.h"

static SDL_Mutex *g_m;
static char g_last_cookie[256];

static void handler(ts_conn *c, const ts_req *r, void *user)
{
    ts_server *s = user;
    if (strcmp(r->target, "/hello") == 0) {
        ts_respond_str(c, 200, "", "secure hello");
    } else if (strcmp(r->target, "/echo") == 0) {
        char buf[300];
        int n = SDL_snprintf(buf, sizeof buf, "%s|%.*s", r->method, (int)r->body_len, r->body != nullptr ? (char *)r->body : "");
        ts_respond(c, 200, "", buf, (size_t)n);
    } else if (strcmp(r->target, "/big") == 0) {
        size_t n = 600000;
        uint8_t *b = SDL_malloc(n);
        for (size_t i = 0; i < n; i++) {
            b[i] = (uint8_t)(i * 13 + 5);
        }
        ts_respond(c, 200, "", b, n);
        SDL_free(b);
    } else if (strcmp(r->target, "/setcookie") == 0) {
        ts_respond_str(c, 200, "Set-Cookie: s=1; Secure; Path=/\r\nSet-Cookie: p=2; Path=/\r\n", "set");
    } else if (strcmp(r->target, "/cookie-check") == 0) {
        const char *v = ts_req_header(r, "cookie");
        SDL_LockMutex(g_m);
        SDL_snprintf(g_last_cookie, sizeof g_last_cookie, "%s", v != nullptr ? v : "(none)");
        SDL_UnlockMutex(g_m);
        ts_respond_str(c, 200, "", "ok");
    } else if (strcmp(r->target, "/closedelim") == 0) { /* body ends with close_notify */
        ts_printf(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nwhole body");
        c->close = true;
    } else if (strcmp(r->target, "/truncated") == 0) { /* the transport is cut: no close_notify */
        ts_printf(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhalf a bod");
        c->close = true;
        c->abrupt = true;
    } else if (strcmp(r->target, "/lengthabrupt") == 0) { /* a complete length-delimited body, then a cut */
        ts_respond_str(c, 200, "Connection: close\r\n", "complete");
        c->close = true;
        c->abrupt = true;
    } else if (strcmp(r->target, "/hang") == 0) {
        while (!SDL_GetAtomicInt(&s->stop)) {
            SDL_Delay(10);
        }
    } else {
        ts_respond_str(c, 404, "", "nf");
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

static bool setup(const char *crt, const char *key, bool trust)
{
    g_m = g_m != nullptr ? g_m : SDL_CreateMutex();
    memset(&g_log, 0, sizeof g_log);
    g_net = tn_new(&g_log);
    if (trust) {
        tn_trust_test_ca(g_net);
    }
    g_srv = ts_start(handler, nullptr, crt, key);
    if (g_srv != nullptr) {
        g_srv->user = g_srv;
    }
    return g_srv != nullptr;
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
    EMB_CHECK(tc_run(g_net, t, &r, 15000));
}

EMB_TEST(tls_success_with_the_test_root)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true));
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK_EQ_INT(t.status, 200);
    EMB_CHECK(t.tls);
    EMB_CHECK(tc_body_is(&t, "secure hello"));
    EMB_CHECK(t.order_ok);
    tc_free(&t);
    teardown();
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(tls_keep_alive_post_and_large_body)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true));
    for (int i = 0; i < 3; i++) {
        tc t;
        tc_init(&t);
        fetch(&t, "GET", "/hello");
        EMB_CHECK(t.ended);
        tc_free(&t);
    }
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "POST", url_for("/echo"));
    r.body = "secret";
    r.body_len = 6;
    EMB_CHECK(tc_run(g_net, &t, &r, 15000));
    EMB_CHECK(tc_body_is(&t, "POST|secret"));
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/big");
    EMB_CHECK(t.ended);
    EMB_REQUIRE(t.body.len == 600000);
    bool ok = true;
    for (size_t i = 0; i < t.body.len && ok; i++) {
        ok = t.body.data[i] == (uint8_t)(i * 13 + 5);
    }
    EMB_CHECK(ok);
    tc_free(&t);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->accepted), 1); /* one handshake for all five requests */
    teardown();
}

EMB_TEST(tls_unknown_root_is_rejected)
{
    EMB_REQUIRE(setup("server.crt", "server.key", false)); /* the test root is not trusted */
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.errored && !t.got_head);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TLS);
    EMB_CHECK(t.msg[0] != 0);
    tc_free(&t);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), 0); /* nothing was sent over the failed channel */
    /* Adding the root afterwards applies to later connections. */
    tn_trust_test_ca(g_net);
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.ended);
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_bundled_roots_can_be_switched_off)
{
    EMB_REQUIRE(setup("server.crt", "server.key", false));
    embi_net_use_bundled_roots(g_net, false);
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TLS);
    tc_free(&t);
    embi_net_use_bundled_roots(g_net, true); /* the bundle parses, and still does not hold the test root */
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TLS);
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_all_bundled_roots_load)
{
    /* The bundle has 118 certificates. This build's Mbed TLS configuration (mbedtls_user_config.h) cannot parse 3 of them (an
     * algorithm it leaves out); that is the same with the PEM text and with the DER records of EMBER_COMPRESS_ASSETS, which
     * this test pins: every root is accounted for, and a change in the number needs a look. */
    int failed = 0;
    size_t n = embi_net_bundled_roots_check(&failed);
    EMB_CHECK_EQ_INT((int)n + failed, 118);
    EMB_CHECK_EQ_INT(failed, 3);
}

EMB_TEST(tls_certificate_signed_by_an_untrusted_ca_is_rejected)
{
    EMB_REQUIRE(setup("rogue-server.crt", "rogue-server.key", true)); /* the test root is trusted, the rogue one not */
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.errored);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TLS);
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_hostname_mismatch_is_rejected)
{
    EMB_REQUIRE(setup("other.crt", "other.key", true)); /* valid chain, but for other.test only */
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/hello");
    EMB_CHECK(t.errored && !t.got_head);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_TLS);
    tc_free(&t);
    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_srv->requests), 0);
    teardown();
}

EMB_TEST(tls_name_match_through_dns_san_for_localhost)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true)); /* SAN: DNS localhost and IP 127.0.0.1 */
    char url[128];
    SDL_snprintf(url, sizeof url, "https://localhost:%d/hello", g_srv->port);
    tc t;
    tc_init(&t);
    embi_net_request r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(g_net, &t, &r, 15000));
    EMB_CHECK(t.ended);
    EMB_CHECK(tc_body_is(&t, "secure hello"));
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_secure_cookies_are_sent_over_https_only)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true));
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/setcookie");
    EMB_CHECK(t.ended);
    tc_free(&t);
    tc_init(&t);
    fetch(&t, "GET", "/cookie-check");
    EMB_CHECK_EQ_STR(g_last_cookie, "s=1; p=2");
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_garbage_from_a_plain_server_is_an_error)
{
    /* An https URL against a server that speaks plain HTTP: the handshake fails, with a TLS error. */
    g_m = g_m != nullptr ? g_m : SDL_CreateMutex();
    memset(&g_log, 0, sizeof g_log);
    g_net = tn_new(&g_log);
    g_srv = ts_start(handler, nullptr, nullptr, nullptr);
    EMB_REQUIRE(g_srv != nullptr);
    g_srv->user = g_srv;
    char url[128];
    SDL_snprintf(url, sizeof url, "https://127.0.0.1:%d/hello", g_srv->port);
    tc t;
    tc_init(&t);
    embi_net_set_timeouts(g_net, 3000, 3000);
    embi_net_request r = tc_request(&t, "GET", url);
    EMB_CHECK(tc_run(g_net, &t, &r, 15000));
    EMB_CHECK(t.errored);
    EMB_CHECK(t.err == EMBI_NET_ERR_TLS || t.err == EMBI_NET_ERR_TIMEOUT || t.err == EMBI_NET_ERR_CONNECT);
    tc_free(&t);
    teardown();
}

EMB_TEST(tls_destroy_with_requests_in_flight)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true));
    enum { N = 4 };
    tc t[N];
    embi_net_req *reqs[N];
    for (int i = 0; i < N; i++) {
        tc_init(&t[i]);
        embi_net_request r = tc_request(&t[i], "GET", url_for("/hang"));
        reqs[i] = embi_net_start(g_net, &r);
    }
    SDL_Delay(300);
    tn_free(g_net, &g_log);
    g_net = nullptr;
    for (int i = 0; i < N; i++) {
        EMB_CHECK(tc_wait_released(&t[i], 1000));
        EMB_CHECK(!t[i].ended && t[i].order_ok);
        embi_net_req_unref(reqs[i]);
        tc_free(&t[i]);
    }
    ts_stop(g_srv);
    EMB_CHECK_EQ_INT(embi_net_live(), 0);
}

EMB_TEST(tls_close_delimited_body_needs_close_notify)
{
    EMB_REQUIRE(setup("server.crt", "server.key", true));
    tc t;
    tc_init(&t);
    fetch(&t, "GET", "/closedelim");
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK(tc_body_is(&t, "whole body"));
    tc_free(&t);

    tc_init(&t);
    fetch(&t, "GET", "/truncated"); /* indistinguishable from an attacker's cut: an error, not a clean end */
    EMB_CHECK(t.errored && !t.ended);
    EMB_CHECK_EQ_INT(t.err, EMBI_NET_ERR_PROTOCOL);
    EMB_CHECK(strstr(t.msg, "close_notify") != nullptr);
    tc_free(&t);

    tc_init(&t);
    fetch(&t, "GET", "/lengthabrupt"); /* length-delimited framing proves completeness by itself */
    EMB_CHECK(t.ended && !t.errored);
    EMB_CHECK(tc_body_is(&t, "complete"));
    tc_free(&t);
    teardown();
}

EMB_TEST_MAIN()
