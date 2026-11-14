/*
 * The Fetch standard's data: URL processor: the MIME default, base64 and percent-decoding, the
 * fragment, and the failure cases.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

/* Runs the processor. Returns true when it succeeds, with the MIME and body copied out. */
static bool data_of(const char *url, embi_sb *mime, embi_sb *body)
{
    return embi_data_url((embi_str){url, (uint32_t)strlen(url)}, mime, body);
}

static bool eq(const embi_sb *s, const char *want)
{
    return s->len == strlen(want) && (s->len == 0 || memcmp(s->data, want, s->len) == 0);
}

EMB_TEST(data_url_plain_and_default_mime)
{
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    EMB_CHECK(data_of("data:,A%20brief%20note", &mime, &body));
    EMB_CHECK(eq(&mime, "text/plain;charset=US-ASCII"));
    EMB_CHECK(eq(&body, "A brief note"));
    embi_sb_free(&mime);
    embi_sb_free(&body);

    mime = (embi_sb){nullptr, 0, 0};
    body = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(data_of("data:text/html; charset=utf-8,%3Cp%3E#frag", &mime, &body));
    EMB_CHECK(eq(&mime, "text/html;charset=utf-8"));
    EMB_CHECK(eq(&body, "<p>"));
    embi_sb_free(&mime);
    embi_sb_free(&body);

    /* A bad percent sequence is kept as written. */
    mime = (embi_sb){nullptr, 0, 0};
    body = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(data_of("data:,%zz%4", &mime, &body));
    EMB_CHECK(eq(&body, "%zz%4"));
    embi_sb_free(&mime);
    embi_sb_free(&body);
}

EMB_TEST(data_url_base64)
{
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    EMB_CHECK(data_of("data:text/plain;base64,SGVsbG8=", &mime, &body));
    EMB_CHECK(eq(&mime, "text/plain"));
    EMB_CHECK(eq(&body, "Hello"));
    embi_sb_free(&mime);
    embi_sb_free(&body);

    mime = (embi_sb){nullptr, 0, 0};
    body = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(data_of("data:;base64,AA==", &mime, &body));
    EMB_CHECK(eq(&mime, "text/plain;charset=US-ASCII"));
    EMB_CHECK(body.len == 1 && body.data[0] == 0);
    embi_sb_free(&mime);
    embi_sb_free(&body);

    /* A ";base64" suffix is matched case-insensitively. */
    mime = (embi_sb){nullptr, 0, 0};
    body = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(data_of("data:image/png;BASE64,iVBORw==", &mime, &body));
    EMB_CHECK(eq(&mime, "image/png"));
    EMB_CHECK(body.len == 4);
    embi_sb_free(&mime);
    embi_sb_free(&body);
}

EMB_TEST(data_url_base64_spacing)
{
    static const char *const urls[] = {
        "data:text/plain; base64,SGVsbG8=",
        "data:text/plain;  BASE64,SGVsbG8=",
        "data:text/plain;base64 ,SGVsbG8=",
        "data:text/plain ; base64,SGVsbG8=",
    };
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
        embi_sb mime = {nullptr, 0, 0};
        embi_sb body = {nullptr, 0, 0};
        EMB_CHECK(data_of(urls[i], &mime, &body));
        EMB_CHECK(eq(&body, "Hello"));
        EMB_CHECK(eq(&mime, "text/plain"));
        embi_sb_free(&mime);
        embi_sb_free(&body);
    }
    /* Without the ';' the word is not the base64 marker. */
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    EMB_CHECK(data_of("data:text/plain base64,SGVsbG8=", &mime, &body));
    EMB_CHECK(eq(&body, "SGVsbG8="));
    embi_sb_free(&mime);
    embi_sb_free(&body);
}

EMB_TEST(data_url_failures)
{
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    EMB_CHECK(!data_of("data:no-comma-here", &mime, &body));
    EMB_CHECK(!data_of("data:text/plain;base64,@@@@", &mime, &body));
    EMB_CHECK(!data_of("https://example.com/", &mime, &body));
    EMB_CHECK(!data_of("not a url", &mime, &body));
    embi_sb_free(&mime);
    embi_sb_free(&body);
}

EMB_TEST(data_url_allocations_balance)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 50; i++) {
        embi_sb mime = {nullptr, 0, 0};
        embi_sb body = {nullptr, 0, 0};
        (void)data_of("data:text/plain;base64,SGVsbG8=", &mime, &body);
        embi_sb_free(&mime);
        embi_sb_free(&body);
    }
    EMB_CHECK(embi_mem_live() == before);
}

LOADER_TEST_MAIN()
