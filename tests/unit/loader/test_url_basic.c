/*
 * Hand-written WHATWG URL cases: serialization of each host kind, file and app URLs, relative
 * resolution, opaque paths, form-urlencoded, origins and the setters' edge cases. Every test ends
 * with the allocator balance check, so a leaked buffer fails here too.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

/* Parses and serializes; returns true when it parses, and copies href into out. */
static bool href_of(const char *input, const char *base, embi_sb *out)
{
    embi_url b = {0};
    embi_url *bp = nullptr;
    if (base != nullptr) {
        if (!embi_url_parse((embi_str){base, (uint32_t)strlen(base)}, nullptr, &b)) {
            return false;
        }
        bp = &b;
    }
    embi_url u = {0};
    bool ok = embi_url_parse((embi_str){input, (uint32_t)strlen(input)}, bp, &u);
    if (ok) {
        embi_url_get(&u, EMBI_URL_HREF, out);
    }
    embi_url_free(&u);
    embi_url_free(&b);
    return ok;
}

static bool expect_href(const char *input, const char *base, const char *want)
{
    embi_sb got = {nullptr, 0, 0};
    bool ok = href_of(input, base, &got);
    bool same = ok && got.len == strlen(want) && (got.len == 0 || memcmp(got.data, want, got.len) == 0);
    if (!same) {
        fprintf(stderr, "  href of \"%s\": got %s%.*s want %s\n", input, ok ? "" : "failure ",
                (int)got.len, got.data ? got.data : "", want);
    }
    embi_sb_free(&got);
    return same;
}

static bool expect_failure(const char *input, const char *base)
{
    embi_sb got = {nullptr, 0, 0};
    bool ok = href_of(input, base, &got);
    embi_sb_free(&got);
    if (ok) {
        fprintf(stderr, "  \"%s\" should fail\n", input);
    }
    return !ok;
}

static bool sb_is(const embi_sb *s, const char *want)
{
    return s->len == strlen(want) && (s->len == 0 || memcmp(s->data, want, s->len) == 0);
}

EMB_TEST(url_special_normalization)
{
    EMB_CHECK(expect_href("https://user:pw@EXAMPLE.com:443/a/../b/./c?x#y", nullptr,
                          "https://user:pw@example.com/b/c?x#y"));
    EMB_CHECK(expect_href("HTTP://Example.COM:80", nullptr, "http://example.com/"));
    EMB_CHECK(expect_href("http://example.com:8080", nullptr, "http://example.com:8080/"));
    EMB_CHECK(expect_href("http:\\\\a\\b", nullptr, "http://a/b"));
    EMB_CHECK(expect_href("  http://x/\t\n y ", nullptr, "http://x/%20y"));
    EMB_CHECK(expect_href("http://x/?a b#c d", nullptr, "http://x/?a%20b#c%20d"));
    EMB_CHECK(expect_href("https://x/%7e/%2e%2E/z", nullptr, "https://x/z"));
    EMB_CHECK(expect_failure("http://f:b/c", nullptr));
    EMB_CHECK(expect_failure("http://a b/", nullptr));
    EMB_CHECK(expect_failure("http://%zz/", nullptr));
    EMB_CHECK(expect_failure("http:///", nullptr));
    EMB_CHECK(expect_failure("https://", nullptr));
    EMB_CHECK(expect_failure("/relative/only", nullptr));
}

EMB_TEST(url_ipv4_and_ipv6_hosts)
{
    EMB_CHECK(expect_href("http://0x7f.1/", nullptr, "http://127.0.0.1/"));
    EMB_CHECK(expect_href("http://1.2.3/", nullptr, "http://1.2.0.3/"));
    EMB_CHECK(expect_href("http://0300.0250.0.1/", nullptr, "http://192.168.0.1/"));
    EMB_CHECK(expect_href("http://4294967295/", nullptr, "http://255.255.255.255/"));
    EMB_CHECK(expect_failure("http://4294967296/", nullptr));
    EMB_CHECK(expect_failure("http://256.0.0.1/", nullptr));
    EMB_CHECK(expect_href("http://[1:0::1]/", nullptr, "http://[1::1]/"));
    EMB_CHECK(expect_href("http://[::ffff:1.2.3.4]/", nullptr, "http://[::ffff:102:304]/"));
    EMB_CHECK(expect_href("http://[2001:DB8:0:0:1:0:0:1]/", nullptr, "http://[2001:db8::1:0:0:1]/"));
    EMB_CHECK(expect_failure("http://[1::2::3]/", nullptr));
    EMB_CHECK(expect_failure("http://[1:2:3:4:5:6:7:8:9]/", nullptr));
    EMB_CHECK(expect_failure("http://[::1/", nullptr));
}

EMB_TEST(url_opaque_and_non_special)
{
    EMB_CHECK(expect_href("mailto:Joe@Example.com", nullptr, "mailto:Joe@Example.com"));
    EMB_CHECK(expect_href("sc://ñ.test/", nullptr, "sc://%C3%B1.test/"));
    EMB_CHECK(expect_failure("sc://a b/", nullptr));
    EMB_CHECK(expect_href("app://bundle/assets/x.js", nullptr, "app://bundle/assets/x.js"));
    EMB_CHECK(expect_href("./assets/y.js?v=1", "app://bundle/index.html",
                          "app://bundle/assets/y.js?v=1"));
    EMB_CHECK(expect_href("../up.css", "app://bundle/a/b/c.html", "app://bundle/a/up.css"));
    EMB_CHECK(expect_href("#frag", "app://bundle/a/b.html", "app://bundle/a/b.html#frag"));
    EMB_CHECK(expect_failure("x", "mailto:a@b"));
}

EMB_TEST(url_file_scheme)
{
    EMB_CHECK(expect_href("file:///C:/x/../y", nullptr, "file:///C:/y"));
    EMB_CHECK(expect_href("file://localhost/x", nullptr, "file:///x"));
    EMB_CHECK(expect_href("file:c:\\dir\\f", nullptr, "file:///c:/dir/f"));
    EMB_CHECK(expect_href("a.txt", "file:///C:/dir/b.html", "file:///C:/dir/a.txt"));
}

EMB_TEST(url_relative_resolution)
{
    EMB_CHECK(expect_href("", "http://a/b/c?q#f", "http://a/b/c?q"));
    EMB_CHECK(expect_href("?n", "http://a/b/c?q#f", "http://a/b/c?n"));
    EMB_CHECK(expect_href("//other/p", "https://a/b", "https://other/p"));
    EMB_CHECK(expect_href("/abs", "https://a/b/c", "https://a/abs"));
    EMB_CHECK(expect_href("g;x?y#s", "http://a/b/c/d;p?q", "http://a/b/c/g;x?y#s"));
    EMB_CHECK(expect_href("..", "http://a/b/c/d", "http://a/b/"));
    EMB_CHECK(expect_href("http:g", "http://a/b/c/d", "http://a/b/c/g"));
    EMB_CHECK(expect_href("https:g", "http://a/b/c/d", "https://g/"));
    EMB_CHECK(expect_href("https:/g", "http://a/b", "https://g/"));
}

EMB_TEST(url_form_urlencoded)
{
    embi_form f = {nullptr, 0, 0};
    embi_form_parse(EMBI_STR("a=1&b=%20x+y&&c&%C3%A9=%FF"), &f);
    EMB_CHECK_EQ_INT((int)f.len, 4);
    if (f.len == 4) {
        EMB_CHECK(sb_is(&f.data[0].name, "a") && sb_is(&f.data[0].value, "1"));
        EMB_CHECK(sb_is(&f.data[1].name, "b") && sb_is(&f.data[1].value, " x y"));
        EMB_CHECK(sb_is(&f.data[2].name, "c") && sb_is(&f.data[2].value, ""));
        EMB_CHECK(sb_is(&f.data[3].name, "\xC3\xA9") && sb_is(&f.data[3].value, "\xEF\xBF\xBD"));
    }
    embi_sb out = {nullptr, 0, 0};
    embi_form_serialize(&f, &out);
    EMB_CHECK(sb_is(&out, "a=1&b=+x+y&c=&%C3%A9=%EF%BF%BD"));
    embi_sb_free(&out);
    embi_form_free(&f);
}

EMB_TEST(url_origin_and_components)
{
    embi_url u = {0};
    EMB_REQUIRE(embi_url_parse(EMBI_STR("https://a.com:8443/x?y#z"), nullptr, &u));
    embi_sb o = {nullptr, 0, 0};
    embi_url_origin(&u, &o);
    EMB_CHECK(sb_is(&o, "https://a.com:8443"));
    embi_sb_free(&o);
    embi_url_free(&u);

    EMB_REQUIRE(embi_url_parse(EMBI_STR("https://a.com:443/"), nullptr, &u));
    o = (embi_sb){nullptr, 0, 0};
    embi_url_origin(&u, &o);
    EMB_CHECK(sb_is(&o, "https://a.com"));
    embi_sb_free(&o);
    embi_url_free(&u);

    EMB_REQUIRE(embi_url_parse(EMBI_STR("blob:https://a.com/uuid"), nullptr, &u));
    o = (embi_sb){nullptr, 0, 0};
    embi_url_origin(&u, &o);
    EMB_CHECK(sb_is(&o, "https://a.com"));
    embi_sb_free(&o);
    embi_url_free(&u);

    EMB_REQUIRE(embi_url_parse(EMBI_STR("file:///x"), nullptr, &u));
    o = (embi_sb){nullptr, 0, 0};
    embi_url_origin(&u, &o);
    EMB_CHECK(sb_is(&o, "null"));
    embi_sb_free(&o);
    embi_url_free(&u);

    /* Offsets: user "user", password "pw", host "a.com", port 8443, path "/p". */
    EMB_REQUIRE(embi_url_parse(EMBI_STR("ftp://user:pw@a.com:8443/p"), nullptr, &u));
    EMB_CHECK_EQ_INT((int)u.user_off, 6);
    EMB_CHECK_EQ_INT((int)u.user_len, 4);
    EMB_CHECK_EQ_INT((int)u.pass_len, 2);
    EMB_CHECK(u.port == 8443);
    embi_sb part = {nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HOST, &part);
    EMB_CHECK(sb_is(&part, "a.com:8443"));
    embi_sb_free(&part);
    embi_url_free(&u);
}

EMB_TEST(url_setters_edge_cases)
{
    embi_url u = {0};
    EMB_REQUIRE(embi_url_parse(EMBI_STR("https://x.org:444/a?b#c"), nullptr, &u));

    /* Default port via the port setter drops the port. */
    EMB_CHECK(embi_url_set(&u, EMBI_URL_PORT, EMBI_STR("443")));
    embi_sb s = {nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HREF, &s);
    EMB_CHECK(sb_is(&s, "https://x.org/a?b#c"));
    embi_sb_free(&s);

    /* A scheme change between special and non-special is refused. */
    EMB_CHECK(embi_url_set(&u, EMBI_URL_PROTOCOL, EMBI_STR("sc")));
    s = (embi_sb){nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HREF, &s);
    EMB_CHECK(sb_is(&s, "https://x.org/a?b#c"));
    embi_sb_free(&s);

    /* search and hash setters: the leading ? and # are stripped, empty clears. */
    EMB_CHECK(embi_url_set(&u, EMBI_URL_SEARCH, EMBI_STR("?q=1 2")));
    EMB_CHECK(embi_url_set(&u, EMBI_URL_HASH, EMBI_STR("#h h")));
    s = (embi_sb){nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HREF, &s);
    EMB_CHECK(sb_is(&s, "https://x.org/a?q=1%202#h%20h"));
    embi_sb_free(&s);
    EMB_CHECK(embi_url_set(&u, EMBI_URL_SEARCH, EMBI_STR("")));
    EMB_CHECK(embi_url_set(&u, EMBI_URL_HASH, EMBI_STR("")));
    s = (embi_sb){nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HREF, &s);
    EMB_CHECK(sb_is(&s, "https://x.org/a"));
    embi_sb_free(&s);

    /* A rejected href leaves the URL alone. */
    EMB_CHECK(!embi_url_set(&u, EMBI_URL_HREF, EMBI_STR("http://[")));
    s = (embi_sb){nullptr, 0, 0};
    embi_url_get(&u, EMBI_URL_HREF, &s);
    EMB_CHECK(sb_is(&s, "https://x.org/a"));
    embi_sb_free(&s);

    /* Opaque paths refuse pathname and host edits. */
    embi_url m = {0};
    EMB_REQUIRE(embi_url_parse(EMBI_STR("mailto:a@b"), nullptr, &m));
    EMB_CHECK(embi_url_set(&m, EMBI_URL_PATHNAME, EMBI_STR("/x")));
    s = (embi_sb){nullptr, 0, 0};
    embi_url_get(&m, EMBI_URL_PATHNAME, &s);
    EMB_CHECK(sb_is(&s, "a@b"));
    embi_sb_free(&s);
    embi_url_free(&m);

    EMB_CHECK(embi_url_can_parse(EMBI_STR("http://a"), nullptr));
    EMB_CHECK(!embi_url_can_parse(EMBI_STR("nope"), nullptr));
    embi_url_free(&u);
}

EMB_TEST(url_memory_balances)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 200; i++) {
        embi_url u = {0};
        EMB_CHECK(embi_url_parse(EMBI_STR("https://u:p@Ex.com:8/a/b?c#d"), nullptr, &u));
        embi_url_set(&u, EMBI_URL_HOST, EMBI_STR("other.example"));
        embi_url_free(&u);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(url_parse_with_unparsed_base_fails)
{
    embi_url base = {0}; /* never parsed: href is null */
    embi_url u = {0};
    EMB_CHECK(!embi_url_parse(EMBI_STR("a/b"), &base, &u));
    EMB_CHECK(u.href == nullptr);
}

LOADER_TEST_MAIN()
