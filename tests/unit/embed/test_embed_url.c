/*
 * URL helpers of the embedding layer (W4e): resolution against a base (RFC 3986 section 5.2, the examples of its
 * appendix), file: paths and file: URLs, and data: URLs.
 */
#include "emb_test.h"

#include "embed/url.h"

#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

/* Resolves rel against base and compares the result with want. */
static bool resolves(const char *base, const char *rel, const char *want)
{
    size_t n = 0;
    char *out = embi_url_resolve((embi_str){base, (uint32_t)strlen(base)}, (embi_str){rel, (uint32_t)strlen(rel)}, &n);
    bool ok = n == strlen(want) && memcmp(out, want, n) == 0;
    if (!ok) {
        SDL_Log("resolve(%s, %s) = %.*s, want %s", base, rel, (int)n, out, want);
    }
    SDL_free(out);
    return ok;
}

EMB_TEST(url_resolve_rfc3986_examples)
{
    /* RFC 3986 appendix C.1 with the base http://a/b/c/d;p?q, written as https. */
    const char *base = "https://a/b/c/d;p?q";
    EMB_CHECK(resolves(base, "g:h", "g:h"));
    EMB_CHECK(resolves(base, "g", "https://a/b/c/g"));
    EMB_CHECK(resolves(base, "./g", "https://a/b/c/g"));
    EMB_CHECK(resolves(base, "g/", "https://a/b/c/g/"));
    EMB_CHECK(resolves(base, "/g", "https://a/g"));
    /* RFC 3986 gives "https://g" for "//g". The embedder resolves with the WHATWG URL parser from W7d on (URL Standard,
     * basic URL parser): a special scheme with an empty path serializes the root path, so the result is "https://g/".
     * The other examples below are kept as regression checks: their results are the same under both rules (the WHATWG
     * parser removes the dot segments of "." "..", "../g", "/./g" and "/../g", and an empty reference, "?y" or "#s"
     * keeps the base query or fragment as RFC 3986 section 5.2.2 says). */
    EMB_CHECK(resolves(base, "//g", "https://g/"));
    EMB_CHECK(resolves(base, "?y", "https://a/b/c/d;p?y"));
    EMB_CHECK(resolves(base, "g?y", "https://a/b/c/g?y"));
    EMB_CHECK(resolves(base, "#s", "https://a/b/c/d;p?q#s"));
    EMB_CHECK(resolves(base, "g#s", "https://a/b/c/g#s"));
    EMB_CHECK(resolves(base, "", "https://a/b/c/d;p?q"));
    EMB_CHECK(resolves(base, ".", "https://a/b/c/"));
    EMB_CHECK(resolves(base, "..", "https://a/b/"));
    EMB_CHECK(resolves(base, "../g", "https://a/b/g"));
    EMB_CHECK(resolves(base, "../../g", "https://a/g"));
    EMB_CHECK(resolves(base, "../../../g", "https://a/g"));
    EMB_CHECK(resolves(base, "/./g", "https://a/g"));
    EMB_CHECK(resolves(base, "/../g", "https://a/g"));
}

EMB_TEST(url_resolve_file_and_empty_base)
{
    EMB_CHECK(resolves("file:///C:/site/index.html", "img/a.png", "file:///C:/site/img/a.png"));
    EMB_CHECK(resolves("file:///C:/site/index.html", "../x.css", "file:///C:/x.css"));
    EMB_CHECK(resolves("file:///home/u/doc.html", "/abs.css", "file:///abs.css"));
    /* No base: the reference stands as written. */
    EMB_CHECK(resolves("", "img/a.png", "img/a.png"));
    EMB_CHECK(resolves("", "https://h/p", "https://h/p"));
}

EMB_TEST(url_scheme_detection)
{
    embi_str s;
    EMB_CHECK(embi_url_scheme((embi_str){"https://x", 9}, &s));
    EMB_CHECK(s.len == 5 && memcmp(s.ptr, "https", 5) == 0);
    /* A drive letter is not a scheme. */
    EMB_CHECK(!embi_url_scheme((embi_str){"C:/x/y.html", 11}, nullptr));
    EMB_CHECK(!embi_url_scheme((embi_str){"/abs/path", 9}, nullptr));
    EMB_CHECK(!embi_url_scheme((embi_str){"a b:c", 5}, nullptr));
    EMB_CHECK(embi_url_scheme_is((embi_str){"FILE", 4}, "file"));
    EMB_CHECK(!embi_url_scheme_is((embi_str){"files", 5}, "file"));
}

static bool path_of(const char *url, const char *want)
{
    char out[256];
    size_t n = 0;
    bool ok = embi_url_to_path((embi_str){url, (uint32_t)strlen(url)}, out, sizeof out, &n);
    return ok && n == strlen(want) && memcmp(out, want, n) == 0;
}

EMB_TEST(url_file_paths)
{
    EMB_CHECK(path_of("file:///C:/Users/a/index.html", "C:/Users/a/index.html"));
    EMB_CHECK(path_of("file:///C:/a%20b/c%C3%A9.png", "C:/a b/c\xC3\xA9.png"));
    EMB_CHECK(path_of("file:///home/u/x.html", "/home/u/x.html"));
    EMB_CHECK(path_of("file://localhost/home/u/x.html", "/home/u/x.html"));
    EMB_CHECK(path_of("C:\\Users\\a\\b.html", "C:/Users/a/b.html"));
    /* Refused: a ".." segment, a remote host, another scheme, and a malformed escape. */
    EMB_CHECK(!path_of("file:///C:/a/../../secret.txt", ""));
    EMB_CHECK(!path_of("file://server/share/x.html", ""));
    EMB_CHECK(!path_of("https://a/b.html", ""));
    EMB_CHECK(!path_of("file:///C:/a%2", ""));
    EMB_CHECK(!path_of("file:///C:/a%00b", ""));
}

/* A relative bare path is resolved against the current directory (its URL is the file's real location), and an absolute
 * path is left alone. */
EMB_TEST(url_bare_relative_path_resolves_against_cwd)
{
    char *cwd = SDL_GetCurrentDirectory();
    EMB_REQUIRE(cwd != nullptr);
    size_t cl = strlen(cwd);
    while (cl > 0 && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) {
        cl--;
    }
    cwd[cl] = '\0';
    char joined[1200];
    SDL_snprintf(joined, sizeof joined, "%s/page.html", cwd);
    SDL_free(cwd);
    char want[1400], got[1400];
    size_t wn = embi_url_from_path((embi_str){joined, (uint32_t)strlen(joined)}, want, sizeof want);
    size_t gn = embi_url_from_bare_path(EMBI_STR("page.html"), got, sizeof got);
    EMB_CHECK(gn == wn && memcmp(got, want, gn) == 0);
    EMB_CHECK(gn > strlen("file:///page.html"));
    size_t an = embi_url_from_bare_path(EMBI_STR("/tmp/x.html"), got, sizeof got);
    EMB_CHECK(an == strlen("file:///tmp/x.html") && memcmp(got, "file:///tmp/x.html", an) == 0);
}

EMB_TEST(url_from_path_round_trip)
{
    char url[256];
    size_t n = embi_url_from_path(EMBI_STR("C:\\my docs\\a#1.html"), url, sizeof url);
    EMB_CHECK(n == strlen("file:///C:/my%20docs/a%231.html"));
    EMB_CHECK(memcmp(url, "file:///C:/my%20docs/a%231.html", n) == 0);
    char back[256];
    size_t bl = 0;
    EMB_CHECK(embi_url_to_path((embi_str){url, (uint32_t)n}, back, sizeof back, &bl));
    EMB_CHECK(bl == strlen("C:/my docs/a#1.html") && memcmp(back, "C:/my docs/a#1.html", bl) == 0);
    /* A POSIX absolute path. */
    n = embi_url_from_path(EMBI_STR("/tmp/x y.css"), url, sizeof url);
    EMB_CHECK(memcmp(url, "file:///tmp/x%20y.css", n) == 0);
}

static bool data_is(const char *url, const char *want)
{
    uint8_t *bytes = nullptr;
    size_t len = 0;
    bool ok = embi_url_data((embi_str){url, (uint32_t)strlen(url)}, &bytes, &len);
    ok = ok && len == strlen(want) && (len == 0 || memcmp(bytes, want, len) == 0);
    SDL_free(bytes);
    return ok;
}

EMB_TEST(url_opaque_urls_keep_their_dots_and_empty_results_are_allocated)
{
    /* A data: URL is opaque: its payload is not a path, so dot segments in it stay. */
    EMB_CHECK(resolves("", "data:text/plain,a/./b/../c", "data:text/plain,a/./b/../c"));
    EMB_CHECK(resolves("https://a/b/c", "data:text/plain,a/./b/../c", "data:text/plain,a/./b/../c"));
    EMB_CHECK(resolves("data:text/plain,x/../y", "#f", "data:text/plain,x/../y#f"));
    /* A hierarchical URL still has them removed. */
    EMB_CHECK(resolves("", "https://a/b/./c/../d", "https://a/b/d"));
    /* An empty reference against a base without a scheme or path gives an allocated empty string, not nullptr. */
    size_t n = 7;
    char *out = embi_url_resolve(EMBI_STR("#x"), EMBI_STR(""), &n);
    EMB_CHECK(out != nullptr);
    EMB_CHECK(n == 0);
    SDL_free(out);
}

EMB_TEST(url_data_schemes)
{
    EMB_CHECK(data_is("data:text/plain;base64,aGVsbG8=", "hello"));
    EMB_CHECK(data_is("data:text/plain;base64,aGVs\nbG8", "hello"));
    EMB_CHECK(data_is("data:,a%20b", "a b"));
    EMB_CHECK(data_is("data:text/plain,hi", "hi"));
    EMB_CHECK(!data_is("data:text/plain;base64,@@@", ""));
    EMB_CHECK(!data_is("data:no-comma", ""));
    EMB_CHECK(!data_is("https://x/y", ""));
}

EMB_TEST_MAIN()
