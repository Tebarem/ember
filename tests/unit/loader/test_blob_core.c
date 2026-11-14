/*
 * Blob core (blob.h): concatenation of parts, the media type normalization, the slice rules with
 * negative and out-of-range indices, slices that outlive their root, and the blob: URL registry
 * (create, resolve with and without a fragment, revoke, free).
 */
#include "loader_test.h"

#include "loader/blob.h"

#include <string.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static bool bytes_are(const embi_blob *b, const char *want)
{
    size_t n = 0;
    const uint8_t *p = embi_blob_bytes(b, &n);
    size_t w = strlen(want);
    if (n != w) {
        return false;
    }
    return n == 0 || memcmp(p, want, n) == 0;
}

EMB_TEST(blob_new_concatenates_parts)
{
    embi_blob *inner = embi_blob_new(&(embi_blob_part){.bytes = (const uint8_t *)"ef", .len = 2}, 1, S(""));
    embi_blob_part parts[3] = {
        {.bytes = (const uint8_t *)"ab", .len = 2},
        {.bytes = (const uint8_t *)"cd", .len = 2},
        {.blob = inner},
    };
    embi_blob *b = embi_blob_new(parts, 3, S("Text/Plain"));
    EMB_CHECK(b->size == 6);
    EMB_CHECK(bytes_are(b, "abcdef"));
    EMB_CHECK(b->type.len == 10 && memcmp(b->type.data, "text/plain", 10) == 0);
    embi_blob_release(inner);
    embi_blob_release(b);
}

EMB_TEST(blob_empty_has_no_bytes)
{
    embi_blob *b = embi_blob_new(nullptr, 0, S(""));
    size_t n = 7;
    EMB_CHECK(embi_blob_bytes(b, &n) == nullptr && n == 0);
    embi_blob *s = embi_blob_slice(b, 0, 10, S(""));
    EMB_CHECK(s->size == 0);
    embi_blob_release(s);
    embi_blob_release(b);
}

EMB_TEST(blob_type_normalization)
{
    embi_sb t = {0};
    embi_blob_type_normalize(S("Image/PNG"), &t);
    EMB_CHECK(t.len == 9 && memcmp(t.data, "image/png", 9) == 0);
    embi_sb_free(&t);
    t = (embi_sb){0};
    /* A code point outside U+0020..U+007E makes the type empty. */
    embi_blob_type_normalize((embi_str){"text/\xc3\xa9", 7}, &t);
    EMB_CHECK(t.len == 0);
    embi_sb_free(&t);
}

EMB_TEST(blob_slice_relative_indices)
{
    embi_blob *b = embi_blob_new(&(embi_blob_part){.bytes = (const uint8_t *)"abcdef", .len = 6}, 1, S(""));
    embi_blob *s1 = embi_blob_slice(b, -3, -1, S("x/y"));
    EMB_CHECK(bytes_are(s1, "de"));
    EMB_CHECK(s1->type.len == 3 && memcmp(s1->type.data, "x/y", 3) == 0);
    embi_blob *s2 = embi_blob_slice(b, 4, 2, S(""));
    EMB_CHECK(s2->size == 0);
    embi_blob *s3 = embi_blob_slice(b, 2, 99, S(""));
    EMB_CHECK(bytes_are(s3, "cdef"));
    embi_blob *s4 = embi_blob_slice(b, -100, 3, S(""));
    EMB_CHECK(bytes_are(s4, "abc"));
    embi_blob *s5 = embi_blob_slice(s3, 1, 3, S(""));
    EMB_CHECK(bytes_are(s5, "de"));
    embi_blob_release(s1);
    embi_blob_release(s2);
    embi_blob_release(s3);
    embi_blob_release(s4);
    embi_blob_release(s5);
    embi_blob_release(b);
}

EMB_TEST(blob_slice_outlives_its_root)
{
    embi_blob *b = embi_blob_new(&(embi_blob_part){.bytes = (const uint8_t *)"stay", .len = 4}, 1, S(""));
    embi_blob *s = embi_blob_slice(b, 1, 3, S(""));
    embi_blob_release(b);
    EMB_CHECK(bytes_are(s, "ta"));
    embi_blob_release(s);
}

EMB_TEST(blob_url_registry_create_resolve_revoke)
{
    embi_rng rng = {0};
    embi_blob_urls reg = {0};
    embi_blob *b = embi_blob_new(&(embi_blob_part){.bytes = (const uint8_t *)"x", .len = 1}, 1, S(""));
    embi_sb url = {0};
    embi_blob_url_create(&reg, &rng, S("app://bundle"), b, &url);
    EMB_CHECK(url.len > 20 && memcmp(url.data, "blob:app://bundle/", 18) == 0);
    embi_str u = {url.data, url.len};
    EMB_CHECK(embi_blob_url_resolve(&reg, u) == b);
    /* A fragment on the URL is ignored. */
    embi_sb frag = {0};
    embi_sb_append(&frag, u);
    embi_sb_append(&frag, S("#part"));
    EMB_CHECK(embi_blob_url_resolve(&reg, (embi_str){frag.data, frag.len}) == b);
    EMB_CHECK(embi_blob_url_resolve(&reg, S("blob:app://bundle/other")) == nullptr);
    embi_sb_free(&frag);
    /* A second URL of the same blob is a separate entry. */
    embi_sb url2 = {0};
    embi_blob_url_create(&reg, &rng, S("app://bundle"), b, &url2);
    EMB_CHECK(!embi_str_eq((embi_str){url2.data, url2.len}, u));
    embi_blob_url_revoke(&reg, u);
    EMB_CHECK(embi_blob_url_resolve(&reg, u) == nullptr);
    EMB_CHECK(embi_blob_url_resolve(&reg, (embi_str){url2.data, url2.len}) == b);
    embi_sb_free(&url);
    embi_sb_free(&url2);
    embi_blob_release(b);
    embi_blob_urls_free(&reg);
}

LOADER_TEST_MAIN()
