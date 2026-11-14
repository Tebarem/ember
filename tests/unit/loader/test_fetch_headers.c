/*
 * Headers (Fetch 5.2): name and value validation, the request and response guards (forbidden
 * names are ignored, the immutable guard refuses every change), append, set (the first entry keeps
 * its place), delete, has and get with the ", " combining, sorted iteration with set-cookie kept one
 * entry per value, and the basic response filter.
 */
#include "loader_test.h"

#include "loader/fetch.h"

#include <string.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static bool get_is(const embi_headers *h, const char *name, const char *want)
{
    embi_sb out = {0};
    bool found = embi_headers_get(h, S(name), &out);
    bool ok = found && out.len == strlen(want) && memcmp(out.data, want, out.len) == 0;
    embi_sb_free(&out);
    return ok;
}

EMB_TEST(headers_names_and_values_validate)
{
    EMB_CHECK(embi_header_name_ok(S("Content-Type")));
    EMB_CHECK(!embi_header_name_ok(S("Content Type")));
    EMB_CHECK(!embi_header_name_ok(S("")));
    EMB_CHECK(!embi_header_name_ok(S("a:b")));
    EMB_CHECK(embi_header_value_ok(S("  spaced  ")));
    EMB_CHECK(!embi_header_value_ok(S("a\nb")));
    EMB_CHECK(!embi_header_value_ok(S("a\rb")));
    EMB_CHECK(!embi_header_value_ok((embi_str){"a\0b", 3}));
    embi_str t = embi_header_trim(S("\t  v \r\n"));
    EMB_CHECK(t.len == 1 && t.ptr[0] == 'v');
}

EMB_TEST(headers_append_get_combines_and_names_are_case_blind)
{
    embi_headers h = {0};
    EMB_CHECK(embi_headers_append(&h, S("X-A"), S("1")) == EMBI_HDR_OK);
    EMB_CHECK(embi_headers_append(&h, S("x-a"), S("  2  ")) == EMBI_HDR_OK);
    EMB_CHECK(get_is(&h, "X-A", "1, 2"));
    EMB_CHECK(embi_headers_has(&h, S("x-A")));
    EMB_CHECK(!embi_headers_has(&h, S("x-b")));
    EMB_CHECK(embi_headers_append(&h, S("bad name"), S("v")) == EMBI_HDR_INVALID);
    EMB_CHECK(embi_headers_append(&h, S("ok"), S("a\nb")) == EMBI_HDR_INVALID);
    embi_headers_free(&h);
}

EMB_TEST(headers_set_keeps_first_position_and_delete_removes_all)
{
    embi_headers h = {0};
    (void)embi_headers_append(&h, S("a"), S("1"));
    (void)embi_headers_append(&h, S("b"), S("2"));
    (void)embi_headers_append(&h, S("a"), S("3"));
    EMB_CHECK(embi_headers_set(&h, S("A"), S("new")) == EMBI_HDR_OK);
    EMB_CHECK(h.list.len == 2);
    EMB_CHECK(h.list.data[0].name.len == 1 && h.list.data[0].name.data[0] == 'a');
    EMB_CHECK(get_is(&h, "a", "new"));
    EMB_CHECK(embi_headers_delete(&h, S("a")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("a")));
    EMB_CHECK(h.list.len == 1);
    embi_headers_free(&h);
}

EMB_TEST(headers_request_guard_ignores_forbidden_names)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_REQUEST;
    EMB_CHECK(embi_headers_append(&h, S("Cookie"), S("a=b")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("cookie")));
    EMB_CHECK(embi_headers_append(&h, S("Sec-Fetch-Mode"), S("cors")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("sec-fetch-mode")));
    EMB_CHECK(embi_headers_set(&h, S("Accept"), S("text/html")) == EMBI_HDR_OK);
    EMB_CHECK(embi_headers_has(&h, S("accept")));
    embi_headers_free(&h);
}

EMB_TEST(headers_response_guard_hides_set_cookie)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_RESPONSE;
    EMB_CHECK(embi_headers_append(&h, S("Set-Cookie"), S("a=1")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("set-cookie")));
    embi_headers_free(&h);
}

EMB_TEST(headers_immutable_refuses_changes)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_IMMUTABLE;
    EMB_CHECK(embi_headers_append(&h, S("a"), S("1")) == EMBI_HDR_IMMUTABLE);
    EMB_CHECK(embi_headers_set(&h, S("a"), S("1")) == EMBI_HDR_IMMUTABLE);
    EMB_CHECK(embi_headers_delete(&h, S("a")) == EMBI_HDR_IMMUTABLE);
    embi_headers_free(&h);
}

EMB_TEST(headers_sorted_combines_but_keeps_set_cookie_apart)
{
    embi_headers h = {0};
    (void)embi_headers_append(&h, S("b"), S("1"));
    (void)embi_headers_append(&h, S("a"), S("2"));
    (void)embi_headers_append(&h, S("a"), S("3"));
    /* The set-cookie entries are added raw, as a response's would be. */
    embi_headers_add_raw(&h, S("Set-Cookie"), S("x=1"));
    embi_headers_add_raw(&h, S("set-cookie"), S("y=2"));
    embi_headers sorted = {0};
    EMB_CHECK(embi_headers_sorted(&h, &sorted));
    EMB_REQUIRE(sorted.list.len == 4);
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[0].name.data, sorted.list.data[0].name.len}, S("a")));
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[0].value.data, sorted.list.data[0].value.len}, S("2, 3")));
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[1].name.data, sorted.list.data[1].name.len}, S("b")));
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[2].name.data, sorted.list.data[2].name.len}, S("set-cookie")));
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[2].value.data, sorted.list.data[2].value.len}, S("x=1")));
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[3].value.data, sorted.list.data[3].value.len}, S("y=2")));
    embi_headers_free(&sorted);
    embi_headers_free(&h);
}

EMB_TEST(headers_sorted_combines_three_values)
{
    embi_headers h = {0};
    (void)embi_headers_append(&h, S("a"), S("1"));
    (void)embi_headers_append(&h, S("a"), S("2"));
    (void)embi_headers_append(&h, S("a"), S("3"));
    embi_headers sorted = {0};
    EMB_CHECK(embi_headers_sorted(&h, &sorted));
    EMB_REQUIRE(sorted.list.len == 1);
    EMB_CHECK(embi_str_eq((embi_str){sorted.list.data[0].value.data, sorted.list.data[0].value.len}, S("1, 2, 3")));
    embi_headers_free(&sorted);
    embi_headers_free(&h);
}

EMB_TEST(headers_basic_filter_hides_set_cookie)
{
    embi_headers src = {0};
    embi_headers_add_raw(&src, S("Content-Type"), S("text/plain"));
    embi_headers_add_raw(&src, S("Set-Cookie"), S("a=1"));
    embi_headers dst = {0};
    EMB_CHECK(embi_headers_copy_filtered(&src, &dst, true));
    EMB_CHECK(dst.list.len == 1);
    EMB_CHECK(embi_headers_has(&dst, S("content-type")));
    embi_headers_free(&dst);
    embi_headers_free(&src);
}

/* The request-no-cors guard (Request with mode no-cors): only the four safelisted names with a safelisted
 * value get through; anything else is ignored without an error. */
EMB_TEST(headers_no_cors_guard_keeps_only_safelisted_request_headers)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_REQUEST_NO_CORS;
    EMB_CHECK(embi_headers_append(&h, S("Accept"), S("text/html")) == EMBI_HDR_OK);
    EMB_CHECK(get_is(&h, "accept", "text/html"));
    EMB_CHECK(embi_headers_append(&h, S("Content-Type"), S("KO")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("content-type")));
    EMB_CHECK(embi_headers_append(&h, S("Content-Type"), S("text/plain;charset=utf-8")) == EMBI_HDR_OK);
    EMB_CHECK(get_is(&h, "content-type", "text/plain;charset=utf-8"));
    EMB_CHECK(embi_headers_append(&h, S("potato"), S("KO")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("potato")));
    EMB_CHECK(embi_headers_append(&h, S("Empty-Value"), S("")) == EMBI_HDR_OK);
    EMB_CHECK(!embi_headers_has(&h, S("empty-value")));
    /* set goes through the same filter: a rejected set leaves the list alone. */
    EMB_CHECK(embi_headers_set(&h, S("Accept-Language"), S("en-US,en;q=0.5")) == EMBI_HDR_OK);
    EMB_CHECK(get_is(&h, "accept-language", "en-US,en;q=0.5"));
    EMB_CHECK(embi_headers_set(&h, S("Accept-Language"), S("bad\"value")) == EMBI_HDR_OK);
    EMB_CHECK(get_is(&h, "accept-language", "en-US,en;q=0.5"));
    embi_headers_free(&h);
}

/* The 128-byte limit applies to the combined value: appending to a safelisted header past the limit is ignored.
 * A plain request guard takes the same appends. */
EMB_TEST(headers_no_cors_guard_limits_the_combined_value)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_REQUEST_NO_CORS;
    char big[100];
    memset(big, 'a', sizeof big);
    EMB_CHECK(embi_headers_append(&h, S("accept"), (embi_str){big, sizeof big}) == EMBI_HDR_OK);
    EMB_CHECK(embi_headers_append(&h, S("accept"), (embi_str){big, sizeof big}) == EMBI_HDR_OK);
    embi_sb out = {0};
    EMB_CHECK(embi_headers_get(&h, S("accept"), &out) && out.len == sizeof big);
    embi_sb_free(&out);
    embi_headers r = {0};
    r.guard = EMBI_GUARD_REQUEST;
    EMB_CHECK(embi_headers_append(&r, S("accept"), (embi_str){big, sizeof big}) == EMBI_HDR_OK);
    EMB_CHECK(embi_headers_append(&r, S("accept"), (embi_str){big, sizeof big}) == EMBI_HDR_OK);
    embi_headers_free(&r);
    embi_headers_free(&h);
}

/* Values are stored as UTF-8 of code points; the 128 limit counts code points, not UTF-8 bytes. */
EMB_TEST(headers_no_cors_counts_byte_string_units)
{
    embi_headers h = {0};
    h.guard = EMBI_GUARD_REQUEST_NO_CORS;
    char v[200];
    for (int i = 0; i < 100; i++) {
        v[2 * i] = (char)0xC3;
        v[2 * i + 1] = (char)0xA9;
    }
    EMB_CHECK(embi_headers_append(&h, S("accept"), (embi_str){v, sizeof v}) == EMBI_HDR_OK);
    EMB_CHECK(embi_headers_has(&h, S("accept")));
    embi_headers_free(&h);
}

/* U+00E9 round trip: get returns the stored code point, the wire form is the single byte 0xE9. */
EMB_TEST(headers_latin1_value_round_trip_to_wire)
{
    embi_headers h = {0};
    EMB_CHECK(embi_headers_set(&h, S("x-a"), S("é")) == EMBI_HDR_OK);
    embi_sb out = {0};
    EMB_CHECK(embi_headers_get(&h, S("x-a"), &out) && out.len == 2);
    EMB_CHECK((unsigned char)out.data[0] == 0xC3 && (unsigned char)out.data[1] == 0xA9);
    embi_sb wire = {0};
    embi_header_value_to_wire((embi_str){out.data, out.len}, &wire);
    EMB_CHECK(wire.len == 1 && (unsigned char)wire.data[0] == 0xE9);
    embi_sb_free(&wire);
    embi_sb_free(&out);
    embi_headers_free(&h);
}

LOADER_TEST_MAIN()
