/*
 * Unit tests of module net that need no sockets: allowlist patterns, the cookie jar, HTTP head parsing, message
 * framing and the chunked decoder, and the content decoders.
 */
#include "emb_test.h"

#include "net/net_int.h"
#include "net_vectors.h"

/* ------------------------------------------------------------------ allowlist */

static bool match(const char *pattern, const char *host, int port)
{
    embi_net_pattern p;
    if (!embi_net_pattern_parse(pattern, &p)) {
        return false;
    }
    bool r = embi_net_pattern_match(&p, (embi_str){host, (uint32_t)strlen(host)}, port);
    embi_net_pattern_free(&p);
    return r;
}

EMB_TEST(allow_host_patterns)
{
    EMB_CHECK(match("example.com", "example.com", 443));
    EMB_CHECK(match("example.com", "example.com", 80));
    EMB_CHECK(match("EXAMPLE.com", "example.com", 80)); /* normalized to lowercase */
    EMB_CHECK(!match("example.com", "www.example.com", 80));
    EMB_CHECK(!match("example.com", "example.org", 80));
    EMB_CHECK(match("127.0.0.1", "127.0.0.1", 8080));
    EMB_CHECK(match("[::1]", "[::1]", 8080));
    EMB_CHECK(match("bücher.example", "xn--bcher-kva.example", 443)); /* IDNA */
}

EMB_TEST(allow_host_port_patterns)
{
    EMB_CHECK(match("example.com:8443", "example.com", 8443));
    EMB_CHECK(!match("example.com:8443", "example.com", 443));
    EMB_CHECK(match("example.com:443", "example.com", 443));
    EMB_CHECK(match("[::1]:8080", "[::1]", 8080));
    EMB_CHECK(!match("[::1]:8080", "[::1]", 80));
}

EMB_TEST(allow_wildcard_patterns)
{
    EMB_CHECK(match("*.example.com", "a.example.com", 443));
    EMB_CHECK(match("*.example.com", "a.b.example.com", 443));
    EMB_CHECK(!match("*.example.com", "example.com", 443)); /* subdomains only */
    EMB_CHECK(!match("*.example.com", "badexample.com", 443));
    EMB_CHECK(!match("*.example.com", "example.com.evil.org", 443));
    EMB_CHECK(match("*.example.com:8080", "x.example.com", 8080));
    EMB_CHECK(!match("*.example.com:8080", "x.example.com", 80));
    EMB_CHECK(match("*", "anything.test", 1));
    EMB_CHECK(match("*", "127.0.0.1", 65535));
}

EMB_TEST(allow_malformed_patterns)
{
    const char *bad[] = {"", " ", "a b", "http://example.com", "example.com/", "example.com:", "example.com:0",
                         "example.com:65536", "example.com:80x", "*.", "*.:80", "**", "*example.com", "a@b", "[::1",
                         "*.[::1]", "%41.example.com", nullptr};
    for (int i = 0; bad[i] != nullptr; i++) {
        embi_net_pattern p;
        bool ok = embi_net_pattern_parse(bad[i], &p);
        if (ok) {
            embi_net_pattern_free(&p);
        }
        if (ok) {
            SDL_Log("    pattern '%s' was accepted", bad[i]);
        }
        EMB_CHECK(!ok);
    }
}

/* ------------------------------------------------------------------ cookies */

static embi_url *parse_url(embi_url *u, const char *s)
{
    *u = (embi_url){0};
    EMB_CHECK(embi_url_parse((embi_str){s, (uint32_t)strlen(s)}, nullptr, u));
    return u;
}

static bool set_cookie(embi_cookiejar *j, const char *url, const char *sc, int64_t now)
{
    embi_url u;
    parse_url(&u, url);
    bool r = embi_cookiejar_set(j, &u, (embi_str){sc, (uint32_t)strlen(sc)}, false, now);
    embi_url_free(&u);
    return r;
}

static void header_for(embi_cookiejar *j, const char *url, int64_t now, bool http_only, char *out, size_t cap)
{
    embi_url u;
    parse_url(&u, url);
    embi_sb sb = {0};
    embi_cookiejar_header(j, &u, http_only, now, &sb);
    SDL_snprintf(out, cap, "%.*s", (int)sb.len, sb.data != nullptr ? sb.data : "");
    embi_sb_free(&sb);
    embi_url_free(&u);
}

EMB_TEST(cookie_set_and_send)
{
    embi_cookiejar j = {0};
    char h[256];
    EMB_CHECK(set_cookie(&j, "http://example.com/a/b", "k=v", 1000));
    header_for(&j, "http://example.com/a/b", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "k=v");
    header_for(&j, "http://www.example.com/a/b", 1000, true, h, sizeof h); /* host-only */
    EMB_CHECK_EQ_STR(h, "");
    header_for(&j, "http://example.org/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    /* The default path is /a: it matches /a/x but not /b. */
    header_for(&j, "http://example.com/a/x", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "k=v");
    header_for(&j, "http://example.com/b", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_path_attribute_and_order)
{
    embi_cookiejar j = {0};
    char h[256];
    set_cookie(&j, "http://example.com/", "a=1; Path=/", 1000);
    set_cookie(&j, "http://example.com/", "b=2; Path=/x/y", 1001);
    set_cookie(&j, "http://example.com/", "c=3; Path=/x", 1002);
    header_for(&j, "http://example.com/x/y/z", 1100, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "b=2; c=3; a=1"); /* longer paths first */
    header_for(&j, "http://example.com/xy", 1100, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=1"); /* /x does not match /xy */
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_domain_attribute)
{
    embi_cookiejar j = {0};
    char h[256];
    EMB_CHECK(set_cookie(&j, "http://www.example.com/", "a=1; Domain=example.com", 1000));
    header_for(&j, "http://api.example.com/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=1");
    header_for(&j, "http://example.com/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=1");
    header_for(&j, "http://badexample.com/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    /* A domain that does not cover the host is refused, as is a dotless one. */
    EMB_CHECK(!set_cookie(&j, "http://www.example.com/", "b=1; Domain=other.com", 1000));
    EMB_CHECK(!set_cookie(&j, "http://www.example.com/", "b=1; Domain=com", 1000));
    EMB_CHECK(set_cookie(&j, "http://localhost/", "c=1; Domain=localhost", 1000));
    /* An IP host only takes itself. */
    EMB_CHECK(!set_cookie(&j, "http://127.0.0.1/", "d=1; Domain=0.0.1", 1000));
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_secure)
{
    embi_cookiejar j = {0};
    char h[256];
    EMB_CHECK(!set_cookie(&j, "http://example.com/", "a=1; Secure", 1000)); /* not from an insecure origin */
    EMB_CHECK(set_cookie(&j, "https://example.com/", "a=1; Secure", 1000));
    header_for(&j, "https://example.com/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=1");
    header_for(&j, "http://example.com/", 1000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    /* __Secure- and __Host- prefixes. */
    EMB_CHECK(!set_cookie(&j, "https://example.com/", "__Secure-x=1", 1000));
    EMB_CHECK(set_cookie(&j, "https://example.com/", "__Secure-x=1; Secure", 1000));
    EMB_CHECK(!set_cookie(&j, "https://example.com/", "__Host-y=1; Secure; Path=/x", 1000));
    EMB_CHECK(set_cookie(&j, "https://example.com/", "__Host-y=1; Secure; Path=/", 1000));
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_expiry)
{
    embi_cookiejar j = {0};
    char h[256];
    set_cookie(&j, "http://example.com/", "m=1; Max-Age=10", 1000);
    set_cookie(&j, "http://example.com/", "e=1; Expires=Thu, 01 Jan 1970 00:00:20 GMT", 1000);
    set_cookie(&j, "http://example.com/", "s=1", 1000);
    header_for(&j, "http://example.com/", 5000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "m=1; e=1; s=1");
    header_for(&j, "http://example.com/", 12000, true, h, sizeof h); /* Max-Age passed */
    EMB_CHECK_EQ_STR(h, "e=1; s=1");
    header_for(&j, "http://example.com/", 25000, true, h, sizeof h); /* Expires passed */
    EMB_CHECK_EQ_STR(h, "s=1");
    /* Max-Age wins over Expires; zero and past values delete. */
    set_cookie(&j, "http://example.com/", "s=1; Max-Age=0", 26000);
    header_for(&j, "http://example.com/", 26000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    set_cookie(&j, "http://example.com/", "p=1", 30000);
    set_cookie(&j, "http://example.com/", "p=2; Expires=Thu, 01 Jan 1970 00:00:10 GMT", 30000);
    header_for(&j, "http://example.com/", 30000, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "");
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_replace_and_http_only)
{
    embi_cookiejar j = {0};
    char h[256];
    set_cookie(&j, "http://example.com/", "a=1", 1000);
    set_cookie(&j, "http://example.com/", "a=2", 1001);
    set_cookie(&j, "http://example.com/", "h=1; HttpOnly", 1002);
    header_for(&j, "http://example.com/", 1100, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=2; h=1");
    header_for(&j, "http://example.com/", 1100, false, h, sizeof h); /* document.cookie */
    EMB_CHECK_EQ_STR(h, "a=2");
    EMB_CHECK_EQ_INT(embi_cookiejar_count(&j), 2);
    /* Script cannot set or overwrite an HttpOnly cookie. */
    embi_url u;
    parse_url(&u, "http://example.com/");
    EMB_CHECK(!embi_cookiejar_set(&j, &u, EMBI_STR("n=1; HttpOnly"), true, 1200));
    EMB_CHECK(!embi_cookiejar_set(&j, &u, EMBI_STR("h=2"), true, 1200));
    EMB_CHECK(embi_cookiejar_set(&j, &u, EMBI_STR("n=1"), true, 1200));
    embi_url_free(&u);
    header_for(&j, "http://example.com/", 1300, true, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "a=2; h=1; n=1");
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_date_formats)
{
    int64_t ms;
    EMB_CHECK(embi_cookie_parse_date(EMBI_STR("Sun, 06 Nov 1994 08:49:37 GMT"), &ms));
    EMB_CHECK_EQ_INT(ms, 784111777000LL);
    EMB_CHECK(embi_cookie_parse_date(EMBI_STR("Sunday, 06-Nov-94 08:49:37 GMT"), &ms));
    EMB_CHECK_EQ_INT(ms, 784111777000LL);
    EMB_CHECK(embi_cookie_parse_date(EMBI_STR("Sun Nov  6 08:49:37 1994"), &ms));
    EMB_CHECK_EQ_INT(ms, 784111777000LL);
    EMB_CHECK(!embi_cookie_parse_date(EMBI_STR("not a date"), &ms));
    EMB_CHECK(!embi_cookie_parse_date(EMBI_STR("Sun, 32 Nov 1994 08:49:37 GMT"), &ms));
}

/* ------------------------------------------------------------------ HTTP parsing */

static embi_http_parse parse_head(const char *s, embi_http_head *h, size_t *used)
{
    return embi_http_parse_head((const uint8_t *)s, strlen(s), used, h);
}

EMB_TEST(http_head_parse)
{
    embi_http_head h;
    size_t used = 0;
    const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                      "X-Empty:\r\n\r\nBODY";
    EMB_CHECK_EQ_INT(parse_head(msg, &h, &used), EMBI_HTTP_DONE);
    EMB_CHECK_EQ_INT(h.status, 404);
    EMB_CHECK_EQ_INT(h.minor, 1);
    EMB_CHECK_EQ_STR(h.reason, "Not Found");
    EMB_CHECK_EQ_INT(h.headers.list.len, 4);
    EMB_CHECK_EQ_INT(used, strlen(msg) - 4);
    embi_str ct = embi_headers_first(&h.headers, EMBI_STR("content-type"));
    EMB_CHECK(embi_str_eq(ct, EMBI_STR("text/plain")));
    embi_http_head_free(&h);

    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 200 OK\r\nA: b\r\n", &h, &used), EMBI_HTTP_NEED_MORE);
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 204\r\n\r\n", &h, &used), EMBI_HTTP_DONE);
    EMB_CHECK_EQ_INT(h.status, 204);
    embi_http_head_free(&h);
}

EMB_TEST(http_head_rejects_malformed)
{
    embi_http_head h;
    size_t used = 0;
    EMB_CHECK_EQ_INT(parse_head("HTTP/2 200 OK\r\n\r\n", &h, &used), EMBI_HTTP_BAD);
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 20 OK\r\n\r\n", &h, &used), EMBI_HTTP_BAD);
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 200 OK\r\n folded: x\r\n\r\n", &h, &used), EMBI_HTTP_BAD); /* obs-fold */
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 200 OK\r\nA : b\r\n\r\n", &h, &used), EMBI_HTTP_BAD);        /* space before colon */
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 200 OK\r\nno colon\r\n\r\n", &h, &used), EMBI_HTTP_BAD);
    EMB_CHECK_EQ_INT(parse_head("HTTP/1.1 200 OK\nA: b\n\r\n\r\n", &h, &used), EMBI_HTTP_BAD); /* bare LF */
}

static embi_body_kind kind_of(const char *head, uint64_t *len, bool head_req)
{
    embi_http_head h;
    size_t used;
    embi_body_kind k = EMBI_BODY_NONE;
    *len = 0;
    if (parse_head(head, &h, &used) != EMBI_HTTP_DONE) {
        return (embi_body_kind)99;
    }
    if (!embi_http_body_kind(&h, head_req, &k, len)) {
        k = (embi_body_kind)98; /* invalid */
    }
    embi_http_head_free(&h);
    return k;
}

EMB_TEST(http_body_framing)
{
    uint64_t len;
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n", &len, false), EMBI_BODY_LENGTH);
    EMB_CHECK_EQ_INT(len, 12);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\n", &len, false), EMBI_BODY_LENGTH);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\n", &len, false), 98);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", &len, false), 98);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", &len, false), 98);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n", &len, false),
                     EMBI_BODY_CHUNKED); /* chunked wins */
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\n\r\n", &len, false), EMBI_BODY_CLOSE);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 204 No Content\r\nContent-Length: 4\r\n\r\n", &len, false), EMBI_BODY_NONE);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 304 Not Modified\r\n\r\n", &len, false), EMBI_BODY_NONE);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n", &len, true), EMBI_BODY_NONE); /* HEAD */
}

static bool chunked(const char *wire, size_t step, const char *expect, bool expect_ok)
{
    embi_http_body b;
    embi_http_body_init(&b, EMBI_BODY_CHUNKED, 0);
    embi_buf out = {0};
    size_t n = strlen(wire), pos = 0;
    while (pos < n && !b.done && !b.bad) {
        size_t take = n - pos < step ? n - pos : step;
        pos += embi_http_body_feed(&b, (const uint8_t *)wire + pos, take, &out);
    }
    bool ok = b.done && !b.bad;
    bool same = out.len == strlen(expect) && (out.len == 0 || memcmp(out.data, expect, out.len) == 0);
    embi_buf_free(&out);
    return expect_ok ? (ok && same) : !ok;
}

EMB_TEST(http_chunked_decoder)
{
    const char *good = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
    for (size_t step = 1; step <= 64; step += (step < 8 ? 1 : 28)) { /* every split of the input */
        EMB_CHECK(chunked(good, step, "hello world", true));
    }
    EMB_CHECK(chunked("A\r\n0123456789\r\n0\r\n\r\n", 3, "0123456789", true));
    EMB_CHECK(chunked("0\r\n\r\n", 1, "", true));
    EMB_CHECK(chunked("g\r\n", 8, "", false));                   /* not hex */
    EMB_CHECK(chunked("5\r\nhelloXX0\r\n\r\n", 8, "", false));  /* missing CRLF after data */
    EMB_CHECK(chunked("\r\n5\r\nhello\r\n", 8, "", false));     /* empty size line */
    EMB_CHECK(chunked("FFFFFFFFFFFFFFFFFF\r\n", 32, "", false)); /* size overflow */
}

/* ------------------------------------------------------------------ decoders */

static bool decode_all(const char *coding, const unsigned char *in, size_t n, size_t step, embi_buf *out)
{
    embi_net_decoder *d = embi_net_decoder_new((embi_str){coding, (uint32_t)strlen(coding)});
    if (d == nullptr) {
        return false;
    }
    bool ok = true;
    for (size_t pos = 0; pos < n && ok; pos += step) {
        ok = embi_net_decoder_feed(d, in + pos, n - pos < step ? n - pos : step, out);
        while (ok && embi_net_decoder_pending(d)) {
            ok = embi_net_decoder_pump(d, out);
        }
    }
    ok = ok && embi_net_decoder_finish(d, out);
    embi_net_decoder_free(d);
    return ok;
}

static bool is_text(const embi_buf *b)
{
    const char *unit = NET_VEC_TEXT;
    size_t ul = strlen(unit);
    if (b->len != ul * 20) {
        return false;
    }
    for (int i = 0; i < 20; i++) {
        if (memcmp(b->data + i * ul, unit, ul) != 0) {
            return false;
        }
    }
    return true;
}

EMB_TEST(decode_gzip_and_deflate)
{
    for (size_t step = 1; step <= 4096; step *= 7) {
        embi_buf out = {0};
        EMB_CHECK(decode_all("gzip", net_vec_gzip, sizeof net_vec_gzip, step, &out));
        EMB_CHECK(is_text(&out));
        embi_buf_free(&out);
        out = (embi_buf){0};
        EMB_CHECK(decode_all("x-gzip", net_vec_gzip, sizeof net_vec_gzip, step, &out));
        embi_buf_free(&out);
        out = (embi_buf){0};
        EMB_CHECK(decode_all("deflate", net_vec_zlib, sizeof net_vec_zlib, step, &out));
        EMB_CHECK(is_text(&out));
        embi_buf_free(&out);
        out = (embi_buf){0};
        EMB_CHECK(decode_all("deflate", net_vec_rawdeflate, sizeof net_vec_rawdeflate, step, &out)); /* raw streams too */
        EMB_CHECK(is_text(&out));
        embi_buf_free(&out);
    }
}

EMB_TEST(decode_rejects_corrupt)
{
    embi_buf out = {0};
    EMB_CHECK(!decode_all("gzip", net_vec_gzip_bad_crc, sizeof net_vec_gzip_bad_crc, 64, &out));
    embi_buf_free(&out);
    out = (embi_buf){0};
    EMB_CHECK(!decode_all("gzip", net_vec_gzip, sizeof net_vec_gzip - 10, 64, &out)); /* truncated */
    embi_buf_free(&out);
    out = (embi_buf){0};
    EMB_CHECK(!decode_all("gzip", (const unsigned char *)"not gzip data at all", 20, 64, &out));
    embi_buf_free(&out);
    out = (embi_buf){0};
    EMB_CHECK(!decode_all("deflate", net_vec_zlib, sizeof net_vec_zlib - 4, 64, &out));
    embi_buf_free(&out);
    EMB_CHECK(embi_net_decoder_new(EMBI_STR("compress")) == nullptr);
    EMB_CHECK(embi_net_decoder_new(EMBI_STR("identity")) == nullptr);
}

EMB_TEST(decode_brotli)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/tests/data/brotli/10x10y.compressed", EMBER_SOURCE_DIR);
    size_t n = 0;
    unsigned char *in = SDL_LoadFile(path, &n);
    EMB_REQUIRE(in != nullptr);
    embi_buf out = {0};
    EMB_CHECK(decode_all("br", in, n, 5, &out));
    EMB_CHECK_EQ_INT(out.len, 20);
    embi_buf_free(&out);
    out = (embi_buf){0};
    EMB_CHECK(!decode_all("br", in, n / 2, 5, &out)); /* truncated */
    embi_buf_free(&out);
    SDL_free(in);
}

EMB_TEST(http_transfer_encoding_lines_are_combined)
{
    uint64_t len;
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", &len, false), EMBI_BODY_CHUNKED);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &len, false), EMBI_BODY_CHUNKED);
    /* Several lines are one list: chunked in the last line is the final coding. */
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n", &len, false),
                     EMBI_BODY_CHUNKED);
    /* chunked that is not the last coding, or given twice, is an error (RFC 9112 6.1). */
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n", &len, false), 98);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", &len, false), 98);
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", &len, false), 98);
    /* No chunked at all: read to the end of the connection. */
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", &len, false), EMBI_BODY_CLOSE);
    /* Transfer-Encoding wins over Content-Length. */
    EMB_CHECK_EQ_INT(kind_of("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n", &len, false),
                     EMBI_BODY_CHUNKED);
}

EMB_TEST(decode_bomb_is_bounded_per_step)
{
    uint32_t matches = 200000; /* 51.6 MB of zeros from about 325 KB */
    embi_buf in = {0};
    make_deflate_zeros(&in, matches);
    embi_net_decoder *d = embi_net_decoder_new(EMBI_STR("deflate"));
    EMB_REQUIRE(d != nullptr);
    uint64_t total = 0;
    size_t biggest = 0;
    embi_buf out = {0};
    EMB_CHECK(embi_net_decoder_feed(d, in.data, in.len, &out)); /* the whole input at once */
    biggest = out.len;
    total += out.len;
    out.len = 0;
    int steps = 1;
    while (embi_net_decoder_pending(d)) {
        EMB_REQUIRE(embi_net_decoder_pump(d, &out));
        if (out.len > biggest) {
            biggest = out.len;
        }
        total += out.len;
        out.len = 0;
        steps++;
    }
    EMB_CHECK(embi_net_decoder_finish(d, &out));
    EMB_CHECK_EQ_INT(total, 1 + (uint64_t)matches * 258);
    EMB_CHECK(biggest <= (1u << 20) + 16384 + 258); /* a step is about 1 MB, never the whole expansion */
    EMB_CHECK(steps > 40);
    embi_net_decoder_free(d);
    embi_buf_free(&out);
    embi_buf_free(&in);
}

EMB_TEST(cookie_max_age_does_not_overflow)
{
    embi_cookiejar j = {0};
    EMB_CHECK(set_cookie(&j, "http://example.com/", "a=1; Max-Age=99999999999999999", 1000));
    EMB_CHECK(set_cookie(&j, "http://example.com/", "b=2; Max-Age=9223372036854775807", 1000));
    EMB_CHECK(!set_cookie(&j, "http://example.com/", "c=3; Max-Age=-99999999999999999999", 1000)); /* expires at once */
    char h[128];
    header_for(&j, "http://example.com/", INT64_C(100000000000000), false, h, sizeof h); /* far in the future */
    EMB_CHECK_EQ_STR(h, "a=1; b=2");
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_insecure_origin_cannot_shadow_secure_cookie)
{
    embi_cookiejar j = {0};
    char h[128];
    EMB_CHECK(set_cookie(&j, "https://example.com/", "s=1; Secure; Path=/", 1000));
    EMB_CHECK(!set_cookie(&j, "http://example.com/", "s=2; Path=/", 1001));        /* overwrite: ignored */
    EMB_CHECK(!set_cookie(&j, "http://example.com/", "s=3; Path=/app", 1001));     /* overlapping path: ignored */
    EMB_CHECK(!set_cookie(&j, "http://sub.example.com/", "s=4; Domain=example.com", 1001)); /* overlapping domain */
    EMB_CHECK(set_cookie(&j, "http://example.com/", "other=5", 1001));             /* another name is fine */
    header_for(&j, "https://example.com/", 1002, false, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "s=1; other=5");
    /* A secure origin may overwrite it, with or without Secure. */
    EMB_CHECK(set_cookie(&j, "https://example.com/", "s=6; Path=/", 1003));
    header_for(&j, "https://example.com/", 1004, false, h, sizeof h);
    EMB_CHECK(strstr(h, "s=6") != nullptr);
    embi_cookiejar_free(&j);
}

EMB_TEST(cookie_per_domain_limit_evicts_the_oldest)
{
    embi_cookiejar j = {0};
    char sc[64], h[8192];
    EMB_CHECK(set_cookie(&j, "http://other.test/", "keep=1", 1));
    for (int i = 0; i < 200; i++) {
        SDL_snprintf(sc, sizeof sc, "k%d=v", i);
        EMB_CHECK(set_cookie(&j, "http://example.com/", sc, 10 + i));
    }
    EMB_CHECK_EQ_INT(embi_cookiejar_count(&j), 181); /* 180 for example.com and the one of another domain */
    header_for(&j, "http://example.com/", 1000, false, h, sizeof h);
    EMB_CHECK(strstr(h, "k0=") == nullptr);
    EMB_CHECK(strstr(h, "k19=") == nullptr);
    EMB_CHECK(strstr(h, "k20=") != nullptr);
    EMB_CHECK(strstr(h, "k199=") != nullptr);
    header_for(&j, "http://other.test/", 1000, false, h, sizeof h);
    EMB_CHECK_EQ_STR(h, "keep=1");
    embi_cookiejar_free(&j);
}


EMB_TEST_MAIN()
